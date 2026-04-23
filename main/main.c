#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/rmt_tx.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "driver/temperature_sensor.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"

#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

#include "soc/soc_caps.h"

#include "channel_plan.h"
#include "ble_security_policy.h"
#include "scan_policy.h"
#include "sniffer_logic.h"
#include "wg_payload.h"
#include "wg_protocol.h"

extern void ble_store_config_init(void);

#if CONFIG_IDF_TARGET_ESP32C6
#define WG_DEVICE_NAME "WIFINDER-C6"
#else
#define WG_DEVICE_NAME "WIFINDER-S3"
#endif

#define WG_DEFAULT_HOP_MS 250
#define WG_MIN_HOP_MS 50
#define WG_MAX_HOP_MS 2000
#define WG_DEFAULT_CHANNEL_MASK 0x1FFFU
#define WG_NODE_5GHZ_MASK_BIT_COUNT 25U
#define WG_NODE_5GHZ_MASK_ALL ((1ULL << WG_NODE_5GHZ_MASK_BIT_COUNT) - 1ULL)
#define WG_SIGHTING_NOTIFY_INTERVAL_MS 5000
#define WG_SEEN_CAPACITY 512
#define WG_UNIQUE_HLL_P 12
#define WG_UNIQUE_HLL_REGISTERS (1U << WG_UNIQUE_HLL_P)
#define WG_SCAN_SEED_MAX_APS 128
#define WG_BLE_PASSKEY 123456U
#define WG_BLE_ATT_PREFERRED_MTU 517
#define WG_BLE_NOTIFY_FRAME_MAX_PAYLOAD_MAX \
  (WG_BLE_ATT_PREFERRED_MTU - 3 - WG_FRAME_HEADER_SIZE)
#define WG_BLE_CONN_ITVL_MIN_1P25MS 6
#define WG_BLE_CONN_ITVL_MAX_1P25MS 6
#define WG_BLE_CONN_LATENCY 0
#define WG_BLE_CONN_SUPERVISION_TIMEOUT_10MS 400
#define WG_BLE_DATA_LEN_OCTETS 251
#define WG_BLE_DATA_LEN_TIME_US 2120
_Static_assert(WG_BLE_NOTIFY_FRAME_MAX_PAYLOAD_MAX > 0,
               "WG_BLE_NOTIFY_FRAME_MAX_PAYLOAD_MAX must be positive");
_Static_assert((WG_UNIQUE_HLL_REGISTERS & (WG_UNIQUE_HLL_REGISTERS - 1)) == 0,
               "WG_UNIQUE_HLL_REGISTERS must be power-of-two");
_Static_assert(WG_NODE_5GHZ_MASK_BIT_COUNT < 64, "WG_NODE_5GHZ_MASK_BIT_COUNT must be <64");

#define WG_GPS_UART_NUM UART_NUM_1
#define WG_GPS_UART_TX_GPIO 10
#define WG_GPS_UART_RX_GPIO 11
#define WG_GPS_UART_BAUD 9600
#define WG_GPS_UART_TARGET_BAUD 38400
#define WG_GPS_MAX_AGE_MS 3000
#define WG_GPS_PHONE_MAX_AGE_MS 5000
#define WG_GPS_UART_ACCURACY_FALLBACK_CM 5000
#define WG_GPS_GSV_FRESH_MS 5000
#define WG_GPS_TALKER_BUCKETS 8
#define WG_GPS_RATE_EVAL_MS 2000
#define WG_GPS_RATE_SWITCH_MIN_MS 12000
#define WG_GPS_RATE_SPEED_2HZ_UP_MMPS 3000
#define WG_GPS_RATE_SPEED_2HZ_DOWN_MMPS 2200
#define WG_GPS_RATE_SPEED_4HZ_UP_MMPS 13000
#define WG_GPS_RATE_SPEED_4HZ_DOWN_MMPS 10500
#define WG_GPS_NAV_MODE_AUTO 0
#define WG_GPS_NAV_MODE_1HZ 1
#define WG_GPS_NAV_MODE_2HZ 2
#define WG_GPS_NAV_MODE_4HZ 4
#define WG_GPS_NMEA_LOG_RAW 0
#define WG_UBX_CFG_LAYER_RAM 0x01
#define WG_UBX_CFG_KEY_NMEA_PROTVER 0x20930001U
#define WG_UBX_CFG_KEY_PM_OPERATEMODE 0x20D00001U
#define WG_UBX_CFG_KEY_SIGNAL_GPS_ENA 0x1031001FU
#define WG_UBX_CFG_KEY_SIGNAL_SBAS_ENA 0x10310020U
#define WG_UBX_CFG_KEY_SIGNAL_GAL_ENA 0x10310021U
#define WG_UBX_CFG_KEY_SIGNAL_BDS_ENA 0x10310022U
#define WG_UBX_CFG_KEY_SIGNAL_QZSS_ENA 0x10310024U
#define WG_UBX_CFG_KEY_SIGNAL_GLO_ENA 0x10310025U
#define WG_UBX_CFG_PM_OPERATEMODE_FULL 0
#define WG_UBX_CFG_NMEA_PROTVER_41 0x41

#define WG_STORAGE_SPIFFS_BASE_PATH "/spiffs"
#define WG_STORAGE_SD_BASE_PATH "/sdcard"
#define WG_STORAGE_FILE_PREFIX "wq_"
#define WG_STORAGE_BLOCK_SIZE 1024
#define WG_STORAGE_BLOCK_MAGIC 0x314B4257U
#define WG_STORAGE_META_MAGIC 0x314D5157U
#define WG_STORAGE_META_SLOT_B_SUFFIX ".b"
#define WG_STORAGE_VERSION 1
#define WG_STORAGE_MANIFEST_GEN_MAGIC 0xA5
#define WG_STORAGE_BLOCK_FLUSH_MS 1200
#define WG_STORAGE_RECORD_HDR_SIZE 2
#define WG_STORAGE_REPLAY_BUDGET_PER_TICK 8
#define WG_STORAGE_BLOB_BUDGET_PER_TICK 16
#define WG_REPLAY_TASK_INTERVAL_MS 2
#define WG_REPLAY_TASK_PUMP_ROUNDS 4
#define WG_REPLAY_BATCH_MAX_RECORDS 8
#define WG_REPLAY_BATCH_HEADER_SIZE 1
#define WG_REPLAY_BATCH_RECORD_HDR_SIZE 2
#define WG_DEBUG_SEED_DEFAULT_BYTES (768U * 1024U)
#define WG_DEBUG_SEED_MIN_BYTES (128U * 1024U)
#define WG_DEBUG_SEED_MAX_BYTES (1024U * 1024U)
#define WG_BLOB_META_PAYLOAD_SIZE 20
#define WG_BLOB_CHUNK_HEADER_SIZE 14
#define WG_BLOB_DONE_PAYLOAD_SIZE 16
#define WG_BLOB_CHUNK_DATA_MAX_MAX \
  (WG_BLE_NOTIFY_FRAME_MAX_PAYLOAD_MAX - WG_BLOB_CHUNK_HEADER_SIZE)
#define WG_BLOB_CHUNK_ACK_TIMEOUT_MS 350
#define WG_BLOB_CHUNK_WINDOW_BYTES 2048
#define WG_STORAGE_MIN_FREE_BYTES (WG_STORAGE_BLOCK_SIZE * 8)
#define WG_STORAGE_PERSIST_MIN_STATIONARY_MS 30000
#define WG_STORAGE_PERSIST_MIN_MOVING_MS 8000
#define WG_STORAGE_PERSIST_MAX_INTERVAL_MS 120000
#define WG_STORAGE_PERSIST_RSSI_DELTA_DB 6
#define WG_STORAGE_MOVING_SPEED_MMPS 2000
#define WG_STORAGE_GPX_POINT_MIN_INTERVAL_MS 1000
#define WG_STORAGE_GPX_POINT_MIN_DISTANCE_M 5.0
#define WG_STORAGE_GPX_FLUSH_INTERVAL_MS 2000
#define WG_STORAGE_PATH_BUF_SIZE 128
#define WG_SD_SPI_CS_GPIO GPIO_NUM_20
#define WG_SD_SPI_SCK_GPIO GPIO_NUM_21
#define WG_SD_SPI_MOSI_GPIO GPIO_NUM_22
#define WG_SD_SPI_MISO_GPIO GPIO_NUM_23

#define WG_WIFI_DUMP_AP_SSID "WIFINDER-DUMP"
#define WG_WIFI_DUMP_AP_CHANNEL 6
#define WG_WIFI_DUMP_HTTP_PORT 80
#define WG_WIFI_DUMP_MANIFEST_URI "/wifinder/dump/manifest.json"
#define WG_WIFI_DUMP_FILE_URI "/wifinder/dump/file"
#define WG_WIFI_DUMP_COMMIT_URI "/wifinder/dump/commit"
#define WG_WIFI_DUMP_ABORT_URI "/wifinder/dump/abort"
#define WG_WIFI_DUMP_JOURNAL_FILE "wq_dump_journal.bin"
#define WG_WIFI_DUMP_JOURNAL_MAGIC 0x314A4457U
#define WG_WIFI_DUMP_JOURNAL_VERSION 1
#define WG_WIFI_DUMP_HTTP_CHUNK 2048

#define WG_NODE_UART_NUM LP_UART_NUM_0
#define WG_NODE_UART_TX_GPIO 5
#define WG_NODE_UART_RX_GPIO 4
#define WG_NODE_UART_BAUD 115200
#define WG_NODE_FRAME_SYNC0 0xA5
#define WG_NODE_FRAME_SYNC1 0x5A
#define WG_NODE_FRAME_VERSION 1
#define WG_NODE_FRAME_HEADER_SIZE 10
#define WG_NODE_FRAME_MAX_PAYLOAD 96
#define WG_NODE_STALE_MS 3000

#define WG_HOST_FRAME_MAX_PAYLOAD 244
#define WG_HOST_RX_BUF_SIZE (WG_FRAME_HEADER_SIZE + WG_HOST_FRAME_MAX_PAYLOAD)
#define WG_HOST_SERIAL_READ_MS 30
#define WG_HOST_UART_NUM UART_NUM_0
#define WG_HOST_UART_BAUD 115200
#define WG_HOST_UART_RX_BUF_SIZE 4096
#define WG_HOST_UART_TX_BUF_SIZE 4096
#define WG_FRAME_MAGIC0 0x57
#define WG_FRAME_MAGIC1 0x47

#define WG_SIGHTING_FLAG_NEW 0x01
#define WG_SIGHTING_FLAG_SNAPSHOT 0x02
#define WG_SIGHTING_FLAG_UPDATED 0x04

#define WG_ACK_OK 0
#define WG_ERR_BAD_CMD 1
#define WG_ERR_BAD_ARG 2
#define WG_ERR_INTERNAL 3
#define WG_ERR_UNAUTHORIZED 4
#define WG_ERR_BUSY 5

#ifndef CONFIG_WG_RGB_LED_FRAME_MS
#define CONFIG_WG_RGB_LED_FRAME_MS 40
#endif

#ifndef CONFIG_WG_RGB_LED_BT_PULSE_MS
#define CONFIG_WG_RGB_LED_BT_PULSE_MS 140
#endif

#ifndef CONFIG_WG_RGB_LED_BRIGHTNESS_PCT
#define CONFIG_WG_RGB_LED_BRIGHTNESS_PCT 8
#endif

#define WG_LED_RMT_RESOLUTION_HZ 10000000
#define WG_LED_BT_TX_PULSE_PEAK 170
#define WG_LED_BT_RX_PULSE_PEAK 110

#if CONFIG_IDF_TARGET_ESP32C6
#define WG_LED_PIXEL_ORDER_RGB 1
#else
#define WG_LED_PIXEL_ORDER_RGB 0
#endif

typedef struct __attribute__((packed)) {
  uint16_t frame_ctrl;
  uint16_t duration;
  uint8_t addr1[6];
  uint8_t addr2[6];
  uint8_t addr3[6];
  uint16_t seq_ctrl;
} ieee80211_mgmt_hdr_t;

typedef struct {
  bool in_use;
  uint8_t bssid[6];
  char ssid[33];
  uint8_t ssid_len;
  uint8_t auth_mode;
  uint8_t proto_flags;
  uint8_t akm_flags;
  uint8_t cipher_flags;
  uint8_t channel;
  int8_t rssi;
  int64_t last_seen_ms;
  int64_t last_notified_ms;
  int64_t last_persisted_ms;
  int8_t last_persisted_rssi;
} ap_entry_t;

typedef struct {
  bool valid;
  uint8_t flags;
  int32_t lat_e7;
  int32_t lon_e7;
  int32_t alt_mm;
  uint32_t speed_mmps;
  uint32_t bearing_mdeg;
  uint32_t unix_time_s;
  uint16_t accuracy_cm;
  uint8_t sat_in_use;
  uint8_t sat_in_view;
  uint8_t sat_count;
  uint16_t hdop_centi;
  uint16_t pdop_centi;
  uint16_t vdop_centi;
  int64_t received_ms;
} gps_fix_t;

typedef struct {
  uint16_t key;
  uint8_t sat_count;
  int64_t updated_ms;
} gps_talker_sat_t;

typedef enum {
  WG_GPS_SRC_NONE = 0,
  WG_GPS_SRC_UART = 1,
  WG_GPS_SRC_PHONE = 2,
} wg_gps_source_t;

typedef enum {
  WG_NODE_MSG_HELLO = 0x01,
  WG_NODE_MSG_HELLO_ACK = 0x02,
  WG_NODE_MSG_CONFIG = 0x03,
  WG_NODE_MSG_PING = 0x04,
  WG_NODE_MSG_PONG = 0x05,
  WG_NODE_MSG_STATUS = 0x06,
  WG_NODE_MSG_SIGHTING = 0x07,
} wg_node_msg_type_t;

typedef struct {
  bool link_up;
  uint16_t packets_per_sec;
  uint16_t forwarded_sightings;
  uint8_t channel;
  uint16_t channel_mask;
  uint64_t channel_mask_5ghz;
  int64_t last_rx_ms;
} node_link_status_t;

typedef enum {
  WG_SESSION_STATE_OPEN = 1,
  WG_SESSION_STATE_CLOSED = 2,
  WG_SESSION_STATE_ABORTED = 3,
} wg_session_state_t;

typedef enum {
  WG_STORAGE_BACKEND_NONE = 0,
  WG_STORAGE_BACKEND_SD = 1,
  WG_STORAGE_BACKEND_SPIFFS = 2,
} wg_storage_backend_t;

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint64_t session_id;
  uint32_t first_seq;
  uint16_t record_count;
  uint16_t payload_len;
  uint32_t crc32;
} wg_store_block_header_t;

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint64_t session_id;
  uint8_t state;
  uint8_t unclean_shutdown;
  uint16_t reserved0;
  uint32_t start_unix_time_s;
  uint32_t end_unix_time_s;
  uint16_t start_hop_ms;
  uint16_t start_channel_mask;
  uint32_t records_written;
  uint32_t records_acked;
  uint32_t records_dropped;
  uint32_t crc_failures;
  uint32_t bytes_written;
  uint32_t last_seq_written;
  uint32_t last_seq_acked;
  uint8_t queue_full;
  uint8_t reserved1[3];
  uint32_t crc32;
} wg_session_manifest_t;

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint64_t run_id;
  uint8_t committed;
  uint8_t reserved[7];
  uint32_t session_count;
  uint32_t crc32;
} wg_dump_journal_header_t;

typedef struct __attribute__((packed)) {
  uint64_t session_id;
  uint32_t ack_seq;
} wg_dump_journal_entry_t;

typedef struct {
  uint64_t session_id;
  uint32_t data_bytes;
  uint32_t gpx_bytes;
  uint32_t written_seq;
  uint32_t acked_seq;
} wg_dump_session_t;

static const char *TAG = "wifinder";

static ap_entry_t s_seen[WG_SEEN_CAPACITY];
static uint8_t s_unique_hll_regs[WG_UNIQUE_HLL_REGISTERS] = {0};
static bool s_unique_hll_dirty = false;
static uint32_t s_unique_bssid_estimate = 0;

static bool s_scanning = false;
static bool s_auto_scan_paused = false;
static uint16_t s_hop_ms = WG_DEFAULT_HOP_MS;
static uint16_t s_channel_mask = WG_DEFAULT_CHANNEL_MASK;
static uint8_t s_current_channel = 1;
static uint8_t s_boot_mode = WG_BOOT_MANUAL;

static esp_timer_handle_t s_hop_timer = NULL;
static bool s_hop_timer_started = false;
static SemaphoreHandle_t s_scan_mutex = NULL;
static TaskHandle_t s_status_task = NULL;
static TaskHandle_t s_replay_task = NULL;

static int64_t s_rate_window_start_ms = 0;
static uint32_t s_rate_packet_acc = 0;
static uint16_t s_packets_per_sec = 0;
static uint16_t s_notify_drops = 0;
static uint32_t s_notify_drop_by_hs_rc[32] = {0};
static uint32_t s_notify_drop_att = 0;
static uint32_t s_notify_drop_hci = 0;
static uint32_t s_notify_drop_l2c = 0;
static uint32_t s_notify_drop_other = 0;
static int s_notify_drop_last_rc = 0;
static uint16_t s_seq = 1;
static gps_fix_t s_latest_gps = {0};
static gps_fix_t s_uart_gps = {0};
static gps_fix_t s_phone_gps = {0};
static wg_gps_source_t s_gps_source = WG_GPS_SRC_NONE;
static bool s_gps_notify_enabled = false;
static volatile uint32_t s_gps_uart_rx_bytes = 0;
static volatile uint32_t s_gps_nmea_lines_total = 0;
static volatile uint32_t s_gps_nmea_gga = 0;
static volatile uint32_t s_gps_nmea_rmc = 0;
static volatile uint32_t s_gps_nmea_gsa = 0;
static volatile uint32_t s_gps_nmea_gsv = 0;
static volatile uint32_t s_gps_nmea_other = 0;
static volatile uint32_t s_gps_fix_updates = 0;
static volatile int64_t s_gps_last_rx_ms = 0;
static volatile uint32_t s_gps_gga_reject_latlon = 0;
static volatile uint32_t s_gps_gga_reject_fix = 0;
static volatile uint32_t s_gps_rmc_reject_status = 0;
static volatile uint32_t s_gps_rmc_reject_latlon = 0;
static volatile int s_gps_last_fix_quality = -1;
static volatile int s_gps_last_sats = -1;
static volatile int s_gps_last_sats_in_view = -1;
static volatile char s_gps_last_rmc_status = '?';
static volatile int64_t s_gps_last_gsa_ms = 0;
static volatile int64_t s_gps_last_gsv_ms = 0;
static gps_talker_sat_t s_gps_gsa_sat_by_talker[WG_GPS_TALKER_BUCKETS] = {0};
static gps_talker_sat_t s_gps_gsv_sat_by_talker[WG_GPS_TALKER_BUCKETS] = {0};
static volatile int s_gps_uart_baud_active = WG_GPS_UART_BAUD;
static volatile uint8_t s_gps_nav_rate_hz = 1;
static volatile uint8_t s_gps_nav_mode = WG_GPS_NAV_MODE_AUTO;
static volatile int64_t s_gps_last_rate_change_ms = 0;
#if SOC_TEMP_SENSOR_SUPPORTED
static temperature_sensor_handle_t s_die_temp_handle = NULL;
static bool s_die_temp_valid = false;
static int16_t s_die_temp_centi = INT16_MIN;
#endif

static node_link_status_t s_node_status = {0};
static uint16_t s_node_seq = 1;
static uint16_t s_node_channel_mask = 0;
static uint64_t s_node_channel_mask_5ghz = WG_NODE_5GHZ_MASK_ALL;
static uint16_t s_local_channel_mask = WG_DEFAULT_CHANNEL_MASK;
static bool s_node_report_enable = false;
static bool s_node_seen_hello = false;
static volatile uint32_t s_node_uart_rx_bytes = 0;
static volatile uint32_t s_node_rx_frames = 0;
static volatile uint32_t s_node_tx_frames = 0;
static volatile uint32_t s_node_tx_failures = 0;
static volatile uint8_t s_node_last_rx_type = 0;
static volatile uint16_t s_node_last_rx_payload_len = 0;
static volatile uint8_t s_node_last_tx_type = 0;
static volatile uint16_t s_node_last_tx_payload_len = 0;
static volatile int64_t s_node_last_tx_ms = 0;
static int64_t s_diag_last_log_ms = 0;
static bool s_node_no_rx_warned = false;

static uint8_t s_node_rx_state = 0;
static uint8_t s_node_hdr[WG_NODE_FRAME_HEADER_SIZE] = {0};
static uint8_t s_node_hdr_pos = 0;
static uint8_t s_node_payload[WG_NODE_FRAME_MAX_PAYLOAD] = {0};
static uint16_t s_node_payload_len = 0;
static uint16_t s_node_payload_pos = 0;
static uint8_t s_node_crc_bytes[2] = {0};
static uint8_t s_node_crc_pos = 0;
static uint16_t s_node_rx_crc_failures = 0;

static char s_gps_line[128] = {0};
static size_t s_gps_line_len = 0;

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_ble_encrypted = false;
static bool s_status_notify_enabled = false;
static bool s_sighting_notify_enabled = false;
static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static bool s_security_retry_attempted = false;
static uint16_t s_ble_conn_itvl_1p25ms = 0;
static uint16_t s_ble_conn_latency = 0;
static uint16_t s_ble_conn_supervision_10ms = 0;
static uint16_t s_ble_att_mtu = 0;
static uint8_t s_ble_tx_phy = 0;
static uint8_t s_ble_rx_phy = 0;
static uint16_t s_ble_max_tx_octets = 0;
static uint16_t s_ble_max_tx_time_us = 0;
static uint16_t s_ble_max_rx_octets = 0;
static uint16_t s_ble_max_rx_time_us = 0;

static uint16_t s_control_handle;
static uint16_t s_status_handle;
static uint16_t s_sighting_handle;
static uint16_t s_config_handle;
static uint16_t s_device_info_handle;

typedef enum {
  WG_HOST_BACKEND_NONE = 0,
  WG_HOST_BACKEND_UART0 = 1,
  WG_HOST_BACKEND_USB = 2,
} wg_host_backend_t;
typedef struct {
  uint8_t buf[WG_HOST_RX_BUF_SIZE];
  size_t pos;
  size_t expected;
  uint32_t frames;
  uint32_t errors;
} wg_host_rx_parser_t;
static bool s_host_serial_enabled = false;
static bool s_host_serial_active = false;
static bool s_host_frame_only_logs = false;
static bool s_host_uart_enabled = false;
static bool s_host_usb_enabled = false;
static wg_host_backend_t s_host_backend = WG_HOST_BACKEND_NONE;
static wg_host_backend_t s_host_tx_backend = WG_HOST_BACKEND_NONE;
static wg_host_rx_parser_t s_host_uart_rx = {0};
static wg_host_rx_parser_t s_host_usb_rx = {0};
static uint32_t s_host_rx_frames = 0;
static uint32_t s_host_tx_frames = 0;
static uint32_t s_host_rx_errors = 0;

static int (*s_prev_log_vprintf)(const char *fmt, va_list args) = NULL;
static SemaphoreHandle_t s_host_tx_mutex = NULL;

static SemaphoreHandle_t s_store_mutex = NULL;
static bool s_storage_ready = false;
static wg_storage_backend_t s_storage_backend = WG_STORAGE_BACKEND_NONE;
static sdmmc_card_t *s_storage_sd_card = NULL;
static bool s_storage_fsync_unsupported_warned = false;
static bool s_session_open = false;
static uint64_t s_session_id = 0;
static uint64_t s_last_session_id = 0;
static uint32_t s_session_next_seq = 1;
static wg_session_manifest_t s_session_manifest = {0};
static uint8_t s_store_block_payload[WG_STORAGE_BLOCK_SIZE - sizeof(wg_store_block_header_t)] = {0};
static uint16_t s_store_block_payload_len = 0;
static uint16_t s_store_block_record_count = 0;
static uint32_t s_store_block_first_seq = 0;
static int64_t s_store_block_started_ms = 0;
static uint32_t s_queue_backlog_records = 0;
static uint32_t s_queue_backlog_bytes = 0;
static bool s_queue_full = false;
static uint32_t s_dropped_flash_full = 0;

static bool s_replay_active = false;
static bool s_replay_requested = false;
static uint64_t s_replay_session_id = 0;
static uint32_t s_replay_cursor_seq = 0;
static uint32_t s_replay_acked_seq = 0;
static FILE *s_replay_fp = NULL;
static uint8_t s_replay_block[WG_STORAGE_BLOCK_SIZE] = {0};
static uint16_t s_replay_block_record_count = 0;
static uint16_t s_replay_block_index = 0;
static uint16_t s_replay_block_payload_len = 0;
static uint16_t s_replay_block_payload_pos = 0;
static uint32_t s_replay_block_first_seq = 0;
static bool s_replay_pending_valid = false;
static uint8_t s_replay_pending_payload[WG_HOST_FRAME_MAX_PAYLOAD] = {0};
static uint8_t s_replay_pending_msg_type = WG_MSG_SIGHTING;
static uint16_t s_replay_pending_len = 0;
static uint64_t s_replay_pending_session_id = 0;
static uint32_t s_replay_pending_seq = 0;
static bool s_replay_prefetch_valid = false;
static uint8_t s_replay_prefetch_payload[WG_HOST_FRAME_MAX_PAYLOAD] = {0};
static uint16_t s_replay_prefetch_len = 0;
static uint64_t s_replay_prefetch_session_id = 0;
static uint32_t s_replay_prefetch_seq = 0;

static bool s_blob_active = false;
static bool s_blob_requested = false;
static bool s_blob_meta_sent = false;
static bool s_blob_done_sent = false;
static bool s_blob_waiting_ack = false;
static uint64_t s_blob_session_id = 0;
static uint32_t s_blob_file_bytes = 0;
static uint32_t s_blob_bytes_sent = 0;
static uint32_t s_blob_bytes_acked = 0;
static uint32_t s_blob_written_seq = 0;
static uint32_t s_blob_acked_seq = 0;
static int64_t s_blob_last_progress_ms = 0;
static FILE *s_blob_fp = NULL;
static bool s_blob_pending_valid = false;
static uint8_t s_blob_pending_msg_type = WG_MSG_BACKLOG_BLOB_META;
static uint8_t s_blob_pending_payload[WG_BLE_NOTIFY_FRAME_MAX_PAYLOAD_MAX] = {0};
static uint16_t s_blob_pending_len = 0;
static uint16_t s_blob_pending_chunk_len = 0;
static FILE *s_gpx_fp = NULL;
static uint64_t s_gpx_session_id = 0;
static char s_gpx_path[WG_STORAGE_PATH_BUF_SIZE] = {0};
static char s_gpx_tmp_path[WG_STORAGE_PATH_BUF_SIZE] = {0};
static bool s_gpx_has_last_point = false;
static int32_t s_gpx_last_lat_e7 = 0;
static int32_t s_gpx_last_lon_e7 = 0;
static int64_t s_gpx_last_point_ms = 0;
static int64_t s_gpx_last_flush_ms = 0;
static uint32_t s_gpx_points_written = 0;

static httpd_handle_t s_wifi_dump_httpd = NULL;
static bool s_wifi_dump_requested = false;
static bool s_wifi_dump_active = false;
static bool s_wifi_dump_ap_running = false;
static uint64_t s_wifi_dump_run_id = 0;
static int64_t s_wifi_dump_started_ms = 0;
static wg_dump_session_t *s_wifi_dump_sessions = NULL;
static size_t s_wifi_dump_session_count = 0;
static size_t s_wifi_dump_session_capacity = 0;

#if CONFIG_WG_RGB_LED_ENABLE
typedef struct {
  uint8_t r;
  uint8_t g;
  uint8_t b;
} rgb_color_t;

typedef struct {
  volatile bool connected;
  volatile bool encrypted;
  volatile bool scanning;
  volatile bool gps_valid;
  volatile uint8_t gps_source;
  volatile bool replay_active;
  volatile bool backlog_present;
} led_runtime_state_t;

typedef enum {
  LED_MODE_IDLE_NO_GPS = 0,
  LED_MODE_IDLE_GPS_UART,
  LED_MODE_IDLE_GPS_PHONE,
  LED_MODE_IDLE_BACKLOG,
  LED_MODE_SCAN_NO_GPS,
  LED_MODE_SCAN_GPS_UART,
  LED_MODE_SCAN_GPS_PHONE,
  LED_MODE_REPLAY_ACTIVE,
} led_mode_t;

static rmt_channel_handle_t s_led_chan = NULL;
static rmt_encoder_handle_t s_led_encoder = NULL;
static bool s_led_ready = false;
static led_runtime_state_t s_led_state = {0};

static const rmt_symbol_word_t s_ws2812_zero = {
    .level0 = 1,
    .duration0 = 0.3 * WG_LED_RMT_RESOLUTION_HZ / 1000000,
    .level1 = 0,
    .duration1 = 0.9 * WG_LED_RMT_RESOLUTION_HZ / 1000000,
};

static const rmt_symbol_word_t s_ws2812_one = {
    .level0 = 1,
    .duration0 = 0.9 * WG_LED_RMT_RESOLUTION_HZ / 1000000,
    .level1 = 0,
    .duration1 = 0.3 * WG_LED_RMT_RESOLUTION_HZ / 1000000,
};

static const rmt_symbol_word_t s_ws2812_reset = {
    .level0 = 0,
    .duration0 = WG_LED_RMT_RESOLUTION_HZ / 1000000 * 50 / 2,
    .level1 = 0,
    .duration1 = WG_LED_RMT_RESOLUTION_HZ / 1000000 * 50 / 2,
};
#endif

static const ble_uuid128_t WG_SERVICE_UUID =
    BLE_UUID128_INIT(0x30, 0x31, 0xA4, 0x8D, 0x52, 0x89, 0xA2, 0x94, 0x11, 0x4A, 0xFD, 0xD7,
                     0x90, 0xE6, 0xE2, 0x6E);
static const ble_uuid128_t WG_CONTROL_UUID =
    BLE_UUID128_INIT(0x31, 0x31, 0xA4, 0x8D, 0x52, 0x89, 0xA2, 0x94, 0x11, 0x4A, 0xFD, 0xD7,
                     0x90, 0xE6, 0xE2, 0x6E);
static const ble_uuid128_t WG_STATUS_UUID =
    BLE_UUID128_INIT(0x32, 0x31, 0xA4, 0x8D, 0x52, 0x89, 0xA2, 0x94, 0x11, 0x4A, 0xFD, 0xD7,
                     0x90, 0xE6, 0xE2, 0x6E);
static const ble_uuid128_t WG_SIGHTING_UUID =
    BLE_UUID128_INIT(0x33, 0x31, 0xA4, 0x8D, 0x52, 0x89, 0xA2, 0x94, 0x11, 0x4A, 0xFD, 0xD7,
                     0x90, 0xE6, 0xE2, 0x6E);
static const ble_uuid128_t WG_CONFIG_UUID =
    BLE_UUID128_INIT(0x34, 0x31, 0xA4, 0x8D, 0x52, 0x89, 0xA2, 0x94, 0x11, 0x4A, 0xFD, 0xD7,
                     0x90, 0xE6, 0xE2, 0x6E);
static const ble_uuid128_t WG_DEVICE_INFO_UUID =
    BLE_UUID128_INIT(0x35, 0x31, 0xA4, 0x8D, 0x52, 0x89, 0xA2, 0x94, 0x11, 0x4A, 0xFD, 0xD7,
                     0x90, 0xE6, 0xE2, 0x6E);

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg);
static bool ble_notify_framed(uint16_t attr_handle, uint8_t type, const uint8_t *payload,
                              uint16_t payload_len);
static int log_mux_vprintf(const char *fmt, va_list args);
static bool notify_status_frame(void);
static bool notify_gps_frame(void);
static esp_err_t start_scan(void);
static esp_err_t stop_scan(void);
static bool notify_sighting_frame_ex(const ap_entry_t *entry, uint8_t flags, uint8_t node_id,
                                     uint64_t session_id, uint32_t record_seq,
                                     uint8_t source_flags, const gps_fix_t *gps,
                                     uint8_t gps_source);
static void publish_sighting_event(ap_entry_t *entry, uint8_t flags, uint8_t node_id);
static bool host_serial_notify_framed(uint8_t type, const uint8_t *payload, uint16_t payload_len);
static bool send_ack_host(uint8_t cmd_id, uint8_t code);
static void init_host_serial_bridge(void);
static uint8_t process_control_frame(const uint8_t *data, size_t data_len, uint8_t *cmd_id_out);
static ap_entry_t *upsert_ap_entry(const uint8_t bssid[6], const char *ssid, uint8_t ssid_len,
                                   uint8_t auth_mode, uint8_t proto_flags, uint8_t akm_flags,
                                   uint8_t cipher_flags, uint8_t channel, int8_t rssi,
                                   int64_t ts_ms, bool allow_ssid_replace, bool *is_new,
                                   bool *is_updated);
static bool should_notify_entry(ap_entry_t *entry, bool is_new, bool is_updated, int64_t ts_ms);
static bool storage_has_output_link(void);
static const char *storage_base_path(void);
static bool storage_get_fs_info(uint64_t *total_bytes_out, uint64_t *used_bytes_out);
static bool storage_commit_file(FILE *fp, const char *kind, uint64_t session_id);
static bool storage_parse_meta_filename(const char *name, uint64_t *session_id_out);
static bool storage_open_session(void);
static bool storage_open_session_locked(void);
static void storage_close_session(wg_session_state_t final_state);
static void storage_close_session_locked(wg_session_state_t final_state);
static bool storage_append_gpx_point_locked(const gps_fix_t *gps, bool force_commit);
static void storage_gpx_tick(void);
static bool storage_flush_pending(bool force);
static void storage_refresh_backlog(bool reclaim);
static bool storage_apply_replay_ack(uint64_t session_id, uint32_t highest_seq);
static bool storage_apply_blob_chunk_reply(uint64_t session_id, uint32_t offset, uint16_t chunk_len,
                                           uint8_t reply_code);
static bool storage_run_replay_tick(void);
static void storage_set_replay_enabled(bool enabled);
static bool storage_run_blob_tick(void);
static void storage_set_blob_enabled(bool enabled);
static bool storage_seed_synthetic(uint32_t target_bytes, uint32_t *records_added_out,
                                   uint32_t *bytes_added_out);
static bool storage_clear_sessions(void);
static bool storage_patch_sighting_source(uint8_t *payload, uint16_t payload_len, uint8_t source_flags);
static void storage_refresh_backlog_locked(bool reclaim);
static bool storage_read_manifest(uint64_t session_id, wg_session_manifest_t *out);
static bool storage_flush_pending_locked(bool force);
static void storage_reset_replay_locked(void);
static void storage_reset_blob_locked(void);
static bool storage_set_wifi_dump_enabled(bool enabled);
static bool storage_commit_wifi_dump_run(uint64_t run_id);
static void storage_recover_wifi_dump_journal(void);
static uint16_t ble_notify_frame_payload_limit(void);
static void wake_replay_task(void);
static void request_fast_ble_conn_params(uint16_t conn_handle);
static void request_fast_ble_phy_data_len(uint16_t conn_handle);
static void refresh_ble_link_metrics(uint16_t conn_handle, const char *reason_tag);
static const char *ble_phy_name(uint8_t phy);
static void clear_peer_bond_for_conn(uint16_t conn_handle, const char *reason_tag);
static void request_link_security(uint16_t conn_handle, const char *reason_tag);
static void init_die_temp_sensor(void);
static void update_die_temp_sample(void);
static uint16_t rd_u16_le(const uint8_t *in);
static void wr_u16_le(uint8_t *out, uint16_t value);
static void wr_u32_le(uint8_t *out, uint32_t value);
static void wr_u64_le(uint8_t *out, uint64_t value);
static void scan_state_lock(void);
static void scan_state_unlock(void);
static void storage_dump_reset_sessions_locked(void);
static bool storage_dump_ensure_capacity_locked(size_t needed);
static bool storage_dump_collect_sessions_locked(void);
static bool storage_dump_journal_write_locked(bool committed);
static bool storage_dump_journal_load_locked(wg_dump_journal_header_t *header_out,
                                             wg_dump_journal_entry_t **entries_out);
static bool storage_dump_journal_delete_locked(void);
static bool storage_dump_start_locked(void);
static void storage_dump_stop_locked(bool keep_journal);
static bool storage_dump_commit_locked(uint64_t run_id);
static bool wifi_dump_http_start_locked(void);
static void wifi_dump_http_stop_locked(void);
static bool wifi_dump_ap_start_locked(void);
static void wifi_dump_ap_stop_locked(void);
static int wifi_dump_manifest_handler(httpd_req_t *req);
static int wifi_dump_file_handler(httpd_req_t *req);

static const struct ble_gatt_svc_def gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &WG_SERVICE_UUID.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &WG_CONTROL_UUID.u,
                    .access_cb = gatt_access_cb,
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                    .val_handle = &s_control_handle,
                },
                {
                    .uuid = &WG_STATUS_UUID.u,
                    .access_cb = gatt_access_cb,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &s_status_handle,
                },
                {
                    .uuid = &WG_SIGHTING_UUID.u,
                    .access_cb = gatt_access_cb,
                    .flags = BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &s_sighting_handle,
                },
                {
                    .uuid = &WG_CONFIG_UUID.u,
                    .access_cb = gatt_access_cb,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
                    .val_handle = &s_config_handle,
                },
                {
                    .uuid = &WG_DEVICE_INFO_UUID.u,
                    .access_cb = gatt_access_cb,
                    .flags = BLE_GATT_CHR_F_READ,
                    .val_handle = &s_device_info_handle,
                },
                {0},
            },
    },
    {0},
};

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void scan_state_lock(void) {
  if (s_scan_mutex != NULL) {
    (void)xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
  }
}

static void scan_state_unlock(void) {
  if (s_scan_mutex != NULL) {
    (void)xSemaphoreGive(s_scan_mutex);
  }
}

static uint8_t pick_first_channel(uint16_t mask) {
  uint8_t next = 1;
  if (wg_channel_next(mask, 13, &next)) {
    return next;
  }
  return 1;
}

static uint32_t ble_device_ms(void) {
  const int64_t ms = now_ms();
  return ms < 0 ? 0 : (uint32_t)ms;
}

static int log_mux_vprintf(const char *fmt, va_list args) {
  if (s_host_frame_only_logs) {
    return 0;
  }
  if (s_prev_log_vprintf != NULL) {
    return s_prev_log_vprintf(fmt, args);
  }
  return vprintf(fmt, args);
}

static const char *host_backend_name(wg_host_backend_t backend) {
  switch (backend) {
    case WG_HOST_BACKEND_UART0:
      return "uart0";
    case WG_HOST_BACKEND_USB:
      return "usb";
    case WG_HOST_BACKEND_NONE:
    default:
      return "none";
  }
}

static wg_host_rx_parser_t *host_parser_for_backend(wg_host_backend_t backend) {
  switch (backend) {
    case WG_HOST_BACKEND_UART0:
      return &s_host_uart_rx;
    case WG_HOST_BACKEND_USB:
      return &s_host_usb_rx;
    case WG_HOST_BACKEND_NONE:
    default:
      return NULL;
  }
}

static void host_serial_reset_rx(wg_host_rx_parser_t *parser) {
  if (parser == NULL) {
    return;
  }
  parser->pos = 0;
  parser->expected = 0;
}

static void host_serial_mark_active(void) {
  if (s_host_serial_active) {
    return;
  }
  ESP_LOGI(TAG, "Host serial protocol active (frame-only stream)");
  s_host_serial_active = true;
  s_host_frame_only_logs = true;
}

static int host_serial_read_bytes(wg_host_backend_t backend, uint8_t *buf, size_t len,
                                  TickType_t wait_ticks) {
  if (!s_host_serial_enabled) {
    return -1;
  }
  switch (backend) {
    case WG_HOST_BACKEND_UART0:
      if (!s_host_uart_enabled) {
        return -1;
      }
      return uart_read_bytes(WG_HOST_UART_NUM, buf, len, wait_ticks);
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    case WG_HOST_BACKEND_USB:
      if (!s_host_usb_enabled) {
        return -1;
      }
      return usb_serial_jtag_read_bytes(buf, len, wait_ticks);
#endif
    case WG_HOST_BACKEND_NONE:
    default:
      return -1;
  }
}

static int host_serial_write_bytes_backend(wg_host_backend_t backend, const uint8_t *buf, size_t len,
                                           TickType_t wait_ticks) {
  if (!s_host_serial_enabled) {
    return -1;
  }
  switch (backend) {
    case WG_HOST_BACKEND_UART0:
      if (!s_host_uart_enabled) {
        return -1;
      }
      return uart_write_bytes(WG_HOST_UART_NUM, (const char *)buf, len);
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    case WG_HOST_BACKEND_USB:
      if (!s_host_usb_enabled) {
        return -1;
      }
      return usb_serial_jtag_write_bytes((const void *)buf, len, wait_ticks);
#endif
    case WG_HOST_BACKEND_NONE:
    default:
      return -1;
  }
}

static int host_serial_write_bytes(const uint8_t *buf, size_t len, TickType_t wait_ticks) {
  if (!s_host_serial_enabled) {
    return -1;
  }
  if (s_host_tx_backend != WG_HOST_BACKEND_NONE) {
    int n = host_serial_write_bytes_backend(s_host_tx_backend, buf, len, wait_ticks);
    if (n > 0) {
      return n;
    }
  }

  int best = -1;
  if (s_host_uart_enabled && s_host_tx_backend != WG_HOST_BACKEND_UART0) {
    int n = host_serial_write_bytes_backend(WG_HOST_BACKEND_UART0, buf, len, wait_ticks);
    if (n > best) {
      best = n;
    }
  }
#if SOC_USB_SERIAL_JTAG_SUPPORTED
  if (s_host_usb_enabled && s_host_tx_backend != WG_HOST_BACKEND_USB) {
    int n = host_serial_write_bytes_backend(WG_HOST_BACKEND_USB, buf, len, wait_ticks);
    if (n > best) {
      best = n;
    }
  }
#endif
  return best;
}

static bool host_serial_notify_framed(uint8_t type, const uint8_t *payload, uint16_t payload_len) {
  if (!s_host_serial_enabled || !s_host_serial_active) {
    return false;
  }
  if ((size_t)payload_len > WG_HOST_FRAME_MAX_PAYLOAD) {
    return false;
  }
  uint8_t frame[WG_FRAME_HEADER_SIZE + WG_HOST_FRAME_MAX_PAYLOAD] = {0};
  wg_frame_t out_frame = {
      .version = WG_PROTOCOL_VERSION,
      .type = type,
      .seq = s_seq++,
      .len = payload_len,
      .device_ms = ble_device_ms(),
      .payload = payload,
  };
  size_t frame_len = 0;
  if (wg_frame_encode(&out_frame, frame, sizeof(frame), &frame_len) != WG_OK) {
    return false;
  }
  if (s_host_tx_mutex != NULL) {
    if (xSemaphoreTake(s_host_tx_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
      return false;
    }
  }
  size_t sent = 0;
  while (sent < frame_len) {
    int n = host_serial_write_bytes(&frame[sent], frame_len - sent, pdMS_TO_TICKS(20));
    if (n <= 0) {
      if (s_host_tx_mutex != NULL) {
        xSemaphoreGive(s_host_tx_mutex);
      }
      return false;
    }
    sent += (size_t)n;
  }
  if (s_host_tx_mutex != NULL) {
    xSemaphoreGive(s_host_tx_mutex);
  }
  s_host_tx_frames++;
  return true;
}

static bool send_ack_host(uint8_t cmd_id, uint8_t code) {
  uint8_t payload[2] = {cmd_id, code};
  return host_serial_notify_framed(code == WG_ACK_OK ? WG_MSG_ACK : WG_MSG_ERROR, payload,
                                   sizeof(payload));
}

static uint64_t bssid_hash64(const uint8_t bssid[6]) {
  uint64_t h = 1469598103934665603ULL;  // FNV-1a 64-bit
  for (int i = 0; i < 6; ++i) {
    h ^= (uint64_t)bssid[i];
    h *= 1099511628211ULL;
  }
  return h == 0 ? 1ULL : h;
}

static void unique_bssid_refresh_estimate(void) {
  if (!s_unique_hll_dirty) {
    return;
  }
  const double m = (double)WG_UNIQUE_HLL_REGISTERS;
  const double alpha = 0.7213 / (1.0 + 1.079 / m);
  double sum = 0.0;
  uint32_t zero_regs = 0;
  for (uint32_t i = 0; i < WG_UNIQUE_HLL_REGISTERS; ++i) {
    uint8_t reg = s_unique_hll_regs[i];
    if (reg == 0) {
      zero_regs++;
    }
    sum += ldexp(1.0, -(int)reg);
  }
  if (sum <= 0.0) {
    s_unique_bssid_estimate = UINT32_MAX;
    s_unique_hll_dirty = false;
    return;
  }
  double estimate = alpha * m * m / sum;
  if (estimate <= (2.5 * m) && zero_regs > 0) {
    estimate = m * log(m / (double)zero_regs);
  }
  if (estimate < 0.0) {
    estimate = 0.0;
  } else if (estimate > (double)UINT32_MAX) {
    estimate = (double)UINT32_MAX;
  }
  s_unique_bssid_estimate = (uint32_t)(estimate + 0.5);
  s_unique_hll_dirty = false;
}

static void unique_bssid_track(const uint8_t bssid[6]) {
  const uint64_t hash = bssid_hash64(bssid);
  const uint32_t idx = (uint32_t)(hash & (WG_UNIQUE_HLL_REGISTERS - 1u));
  const uint64_t w = hash >> WG_UNIQUE_HLL_P;
  const uint8_t max_rank = (uint8_t)((64 - WG_UNIQUE_HLL_P) + 1);
  uint8_t rank = 0;
  if (w == 0) {
    rank = max_rank;
  } else {
    unsigned leading = (unsigned)__builtin_clzll(w);
    if (leading > WG_UNIQUE_HLL_P) {
      leading -= WG_UNIQUE_HLL_P;
    } else {
      leading = 0;
    }
    rank = (uint8_t)(leading + 1u);
    if (rank > max_rank) {
      rank = max_rank;
    }
  }
  if (rank > s_unique_hll_regs[idx]) {
    s_unique_hll_regs[idx] = rank;
    s_unique_hll_dirty = true;
  }
}

#if CONFIG_WG_RGB_LED_ENABLE
static uint8_t led_scale_u8(uint8_t value) {
  uint16_t scaled = (uint16_t)value * (uint16_t)CONFIG_WG_RGB_LED_BRIGHTNESS_PCT;
  scaled = (scaled + 99U) / 100U;
  return (uint8_t)(scaled > 255U ? 255U : scaled);
}

static rgb_color_t led_apply_brightness(rgb_color_t color) {
  return (rgb_color_t){
      .r = led_scale_u8(color.r),
      .g = led_scale_u8(color.g),
      .b = led_scale_u8(color.b),
  };
}

static void led_sync_runtime_state(void) {
  s_led_state.connected = (s_conn_handle != BLE_HS_CONN_HANDLE_NONE);
  s_led_state.encrypted = s_ble_encrypted;
  s_led_state.scanning = s_scanning;
  s_led_state.gps_valid = s_latest_gps.valid;
  s_led_state.gps_source = s_latest_gps.valid ? (uint8_t)s_gps_source : 0;
  s_led_state.replay_active = s_replay_active || s_blob_active;
  s_led_state.backlog_present = s_queue_backlog_records > 0;
}

static void led_sync_link_state(void) { led_sync_runtime_state(); }

static void led_sync_scan_gps_state(void) { led_sync_runtime_state(); }

static led_mode_t led_mode_from_state(void);

static rgb_color_t led_base_color(void) {
  switch (led_mode_from_state()) {
    case LED_MODE_IDLE_NO_GPS:
      return (rgb_color_t){.r = 56, .g = 0, .b = 0};
    case LED_MODE_IDLE_GPS_UART:
      return (rgb_color_t){.r = 0, .g = 16, .b = 46};
    case LED_MODE_IDLE_GPS_PHONE:
      return (rgb_color_t){.r = 0, .g = 34, .b = 34};
    case LED_MODE_IDLE_BACKLOG:
      return (rgb_color_t){.r = 0, .g = 0, .b = 60};
    case LED_MODE_SCAN_NO_GPS:
      return (rgb_color_t){.r = 52, .g = 40, .b = 0};
    case LED_MODE_SCAN_GPS_UART:
      return (rgb_color_t){.r = 0, .g = 60, .b = 0};
    case LED_MODE_SCAN_GPS_PHONE:
      return (rgb_color_t){.r = 0, .g = 52, .b = 36};
    case LED_MODE_REPLAY_ACTIVE:
      return (rgb_color_t){.r = 44, .g = 0, .b = 52};
    default:
      return (rgb_color_t){.r = 56, .g = 0, .b = 0};
  }
}

static led_mode_t led_mode_from_state(void) {
  if (s_led_state.replay_active) {
    return LED_MODE_REPLAY_ACTIVE;
  }
  if (s_led_state.scanning) {
    if (!s_led_state.gps_valid) {
      return LED_MODE_SCAN_NO_GPS;
    }
    if (s_led_state.gps_source == WG_GPS_SRC_UART) {
      return LED_MODE_SCAN_GPS_UART;
    }
    if (s_led_state.gps_source == WG_GPS_SRC_PHONE) {
      return LED_MODE_SCAN_GPS_PHONE;
    }
    return LED_MODE_SCAN_NO_GPS;
  }
  if (s_led_state.backlog_present) {
    return LED_MODE_IDLE_BACKLOG;
  }
  if (s_led_state.gps_valid) {
    if (s_led_state.gps_source == WG_GPS_SRC_UART) {
      return LED_MODE_IDLE_GPS_UART;
    }
    if (s_led_state.gps_source == WG_GPS_SRC_PHONE) {
      return LED_MODE_IDLE_GPS_PHONE;
    }
  }
  return LED_MODE_IDLE_NO_GPS;
}

static bool led_pattern_on(led_mode_t mode, uint32_t now_ms_u32) {
  switch (mode) {
    case LED_MODE_IDLE_NO_GPS: {
      const uint32_t p = now_ms_u32 % 1200U;
      return p < 180U;  // red slow blink
    }
    case LED_MODE_IDLE_GPS_UART: {
      const uint32_t p = now_ms_u32 % 1000U;
      return p < 90U;  // blue heartbeat
    }
    case LED_MODE_IDLE_GPS_PHONE: {
      const uint32_t p = now_ms_u32 % 1000U;
      return p < 70U;  // cyan heartbeat
    }
    case LED_MODE_IDLE_BACKLOG: {
      const uint32_t p = now_ms_u32 % 1200U;
      return p < 90U || (p >= 180U && p < 300U);  // blue double heartbeat
    }
    case LED_MODE_SCAN_NO_GPS:
      return (now_ms_u32 % 1000U) < 120U ||
             ((now_ms_u32 % 1000U) >= 260U && (now_ms_u32 % 1000U) < 380U);  // yellow double blink
    case LED_MODE_SCAN_GPS_UART:
    case LED_MODE_SCAN_GPS_PHONE:
      return true;
    case LED_MODE_REPLAY_ACTIVE:
      return (now_ms_u32 % 320U) < 170U;  // fast pulse while dumping
    default:
      return false;
  }
}

static void led_log_state_if_changed(void) {
  static bool s_prev_valid = false;
  static led_runtime_state_t s_prev = {0};
  if (s_prev_valid && s_prev.connected == s_led_state.connected &&
      s_prev.encrypted == s_led_state.encrypted && s_prev.scanning == s_led_state.scanning &&
      s_prev.gps_valid == s_led_state.gps_valid &&
      s_prev.gps_source == s_led_state.gps_source &&
      s_prev.replay_active == s_led_state.replay_active &&
      s_prev.backlog_present == s_led_state.backlog_present) {
    return;
  }
  s_prev = s_led_state;
  s_prev_valid = true;
  ESP_LOGI(TAG, "LED state conn=%d enc=%d scan=%d gps_valid=%d gps_src=%u replay=%d backlog=%d",
           s_led_state.connected, s_led_state.encrypted, s_led_state.scanning,
           s_led_state.gps_valid, (unsigned)s_led_state.gps_source, s_led_state.replay_active,
           s_led_state.backlog_present);
}

static size_t led_encoder_callback(const void *data, size_t data_size, size_t symbols_written,
                                   size_t symbols_free, rmt_symbol_word_t *symbols, bool *done,
                                   void *arg) {
  (void)arg;
  if (symbols_free < 8) {
    return 0;
  }

  size_t data_pos = symbols_written / 8;
  const uint8_t *bytes = (const uint8_t *)data;
  if (data_pos < data_size) {
    size_t symbol_pos = 0;
    for (int mask = 0x80; mask != 0; mask >>= 1) {
      symbols[symbol_pos++] = (bytes[data_pos] & mask) ? s_ws2812_one : s_ws2812_zero;
    }
    return symbol_pos;
  }

  symbols[0] = s_ws2812_reset;
  *done = true;
  return 1;
}

static esp_err_t led_write_rgb(uint8_t r, uint8_t g, uint8_t b) {
  if (!s_led_ready) {
    return ESP_ERR_INVALID_STATE;
  }

#if WG_LED_PIXEL_ORDER_RGB
  uint8_t pixel[3] = {r, g, b};
#else
  uint8_t pixel[3] = {g, r, b};
#endif
  rmt_transmit_config_t tx_cfg = {
      .loop_count = 0,
  };
  esp_err_t rc = rmt_transmit(s_led_chan, s_led_encoder, pixel, sizeof(pixel), &tx_cfg);
  if (rc == ESP_ERR_INVALID_STATE) {
    // Transient channel state issue: try re-enable once and retry.
    (void)rmt_enable(s_led_chan);
    rc = rmt_transmit(s_led_chan, s_led_encoder, pixel, sizeof(pixel), &tx_cfg);
  }
  if (rc != ESP_OK) {
    return rc;
  }
  return rmt_tx_wait_all_done(s_led_chan, pdMS_TO_TICKS(200));
}

static void led_note_bt_rx(void) {}

static void led_note_bt_tx(void) {}

static void status_led_task(void *arg) {
  (void)arg;
  uint8_t consecutive_failures = 0;
  while (true) {
    led_sync_runtime_state();
    led_log_state_if_changed();
    uint32_t now = ble_device_ms();
    led_mode_t mode = led_mode_from_state();
    rgb_color_t color = led_base_color();

    if (!led_pattern_on(mode, now)) {
      color = (rgb_color_t){0};
    }

    color = led_apply_brightness(color);

    esp_err_t rc = led_write_rgb(color.r, color.g, color.b);
    if (rc != ESP_OK) {
      consecutive_failures++;
      ESP_LOGW(TAG, "status LED update failed rc=%s (%d) streak=%u", esp_err_to_name(rc), rc,
               (unsigned int)consecutive_failures);
      if (consecutive_failures >= 12) {
        ESP_LOGW(TAG, "status LED disabled after repeated failures");
        s_led_ready = false;
        vTaskDelete(NULL);
        return;
      }
    } else {
      consecutive_failures = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(CONFIG_WG_RGB_LED_FRAME_MS));
  }
}

static void init_status_led(void) {
  rmt_tx_channel_config_t tx_chan_cfg = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .gpio_num = CONFIG_WG_RGB_LED_GPIO,
      .mem_block_symbols = 64,
      .resolution_hz = WG_LED_RMT_RESOLUTION_HZ,
      .trans_queue_depth = 4,
  };
  esp_err_t rc = rmt_new_tx_channel(&tx_chan_cfg, &s_led_chan);
  if (rc != ESP_OK) {
    ESP_LOGW(TAG, "status LED disabled: rmt channel init failed rc=%s", esp_err_to_name(rc));
    return;
  }

  rmt_simple_encoder_config_t enc_cfg = {
      .callback = led_encoder_callback,
  };
  rc = rmt_new_simple_encoder(&enc_cfg, &s_led_encoder);
  if (rc != ESP_OK) {
    ESP_LOGW(TAG, "status LED disabled: encoder init failed rc=%s", esp_err_to_name(rc));
    rmt_del_channel(s_led_chan);
    s_led_chan = NULL;
    return;
  }

  rc = rmt_enable(s_led_chan);
  if (rc != ESP_OK) {
    ESP_LOGW(TAG, "status LED disabled: channel enable failed rc=%s", esp_err_to_name(rc));
    rmt_del_encoder(s_led_encoder);
    s_led_encoder = NULL;
    rmt_del_channel(s_led_chan);
    s_led_chan = NULL;
    return;
  }

  s_led_ready = true;
  led_sync_link_state();
  led_sync_scan_gps_state();
  led_log_state_if_changed();

  // Quick self-test to confirm the RGB LED and channel mapping are alive.
  rgb_color_t test_red = led_apply_brightness((rgb_color_t){.r = 255, .g = 0, .b = 0});
  rgb_color_t test_green = led_apply_brightness((rgb_color_t){.r = 0, .g = 255, .b = 0});
  rgb_color_t test_blue = led_apply_brightness((rgb_color_t){.r = 0, .g = 0, .b = 255});
  (void)led_write_rgb(test_red.r, test_red.g, test_red.b);
  vTaskDelay(pdMS_TO_TICKS(160));
  (void)led_write_rgb(test_green.r, test_green.g, test_green.b);
  vTaskDelay(pdMS_TO_TICKS(160));
  (void)led_write_rgb(test_blue.r, test_blue.g, test_blue.b);
  vTaskDelay(pdMS_TO_TICKS(160));
  (void)led_write_rgb(0, 0, 0);

  if (xTaskCreate(status_led_task, "wg_status_led", 3072, NULL, 4, NULL) != pdPASS) {
    ESP_LOGW(TAG, "status LED disabled: task create failed");
    s_led_ready = false;
    rmt_disable(s_led_chan);
    rmt_del_encoder(s_led_encoder);
    s_led_encoder = NULL;
    rmt_del_channel(s_led_chan);
    s_led_chan = NULL;
    return;
  }

  ESP_LOGI(TAG, "status RGB LED enabled on GPIO %d", CONFIG_WG_RGB_LED_GPIO);
}
#else
static void led_note_bt_rx(void) {}

static void led_note_bt_tx(void) {}

static void init_status_led(void) {}

static void led_log_state_if_changed(void) {}
#endif

static uint16_t clamp_u16_u32(uint32_t value) {
  if (value > UINT16_MAX) {
    return UINT16_MAX;
  }
  return (uint16_t)value;
}

static uint16_t crc16_ccitt_seed(uint16_t seed, const uint8_t *data, size_t len) {
  uint16_t crc = seed;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      if (crc & 0x8000) {
        crc = (uint16_t)((crc << 1) ^ 0x1021);
      } else {
        crc <<= 1;
      }
    }
  }
  return crc;
}

static uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
  return crc16_ccitt_seed(0xFFFF, data, len);
}

static uint32_t crc32_ieee(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint32_t)data[i];
    for (int bit = 0; bit < 8; ++bit) {
      uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc = (crc >> 1) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

static bool storage_lock(TickType_t wait_ticks) {
  if (s_store_mutex == NULL) {
    return false;
  }
  return xSemaphoreTake(s_store_mutex, wait_ticks) == pdTRUE;
}

static void storage_unlock(void) {
  if (s_store_mutex != NULL) {
    xSemaphoreGive(s_store_mutex);
  }
}

static bool storage_has_output_link(void) { return s_sighting_notify_enabled || s_host_serial_active; }

static const char *storage_backend_name(wg_storage_backend_t backend) {
  switch (backend) {
    case WG_STORAGE_BACKEND_SD:
      return "sd";
    case WG_STORAGE_BACKEND_SPIFFS:
      return "spiffs";
    case WG_STORAGE_BACKEND_NONE:
    default:
      return "none";
  }
}

static const char *storage_base_path(void) {
  if (s_storage_backend == WG_STORAGE_BACKEND_SD) {
    return WG_STORAGE_SD_BASE_PATH;
  }
  return WG_STORAGE_SPIFFS_BASE_PATH;
}

static bool storage_get_fs_info(uint64_t *total_bytes_out, uint64_t *used_bytes_out) {
  if (total_bytes_out == NULL || used_bytes_out == NULL) {
    return false;
  }
  *total_bytes_out = 0;
  *used_bytes_out = 0;
  if (!s_storage_ready) {
    return false;
  }

  if (s_storage_backend == WG_STORAGE_BACKEND_SPIFFS) {
    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("spiffs", &total, &used) != ESP_OK) {
      return false;
    }
    *total_bytes_out = total;
    *used_bytes_out = used;
    return true;
  }

  if (s_storage_backend == WG_STORAGE_BACKEND_SD) {
    uint64_t total = 0;
    uint64_t free = 0;
    if (esp_vfs_fat_info(WG_STORAGE_SD_BASE_PATH, &total, &free) != ESP_OK) {
      return false;
    }
    *total_bytes_out = total;
    *used_bytes_out = (total >= free) ? (total - free) : 0;
    return true;
  }

  return false;
}

static void storage_session_paths(uint64_t session_id, char *meta_path, size_t meta_size,
                                  char *data_path, size_t data_size) {
  const char *base_path = storage_base_path();
  if (meta_path != NULL && meta_size > 0) {
    snprintf(meta_path, meta_size, "%s/" WG_STORAGE_FILE_PREFIX "%016llX.meta", base_path,
             (unsigned long long)session_id);
  }
  if (data_path != NULL && data_size > 0) {
    snprintf(data_path, data_size, "%s/" WG_STORAGE_FILE_PREFIX "%016llX.dat", base_path,
             (unsigned long long)session_id);
  }
}

static void storage_manifest_paths(uint64_t session_id, char *slot_a_path, size_t slot_a_size,
                                   char *slot_b_path, size_t slot_b_size) {
  char primary[96] = {0};
  storage_session_paths(session_id, primary, sizeof(primary), NULL, 0);
  if (slot_a_path != NULL && slot_a_size > 0) {
    snprintf(slot_a_path, slot_a_size, "%s", primary);
  }
  if (slot_b_path != NULL && slot_b_size > 0) {
    snprintf(slot_b_path, slot_b_size, "%s" WG_STORAGE_META_SLOT_B_SUFFIX, primary);
  }
}

static void storage_gpx_paths(uint64_t session_id, char *gpx_path, size_t gpx_size,
                              char *gpx_tmp_path, size_t gpx_tmp_size) {
  const char *base_path = storage_base_path();
  if (gpx_path != NULL && gpx_size > 0) {
    snprintf(gpx_path, gpx_size, "%s/" WG_STORAGE_FILE_PREFIX "%016llX.gpx", base_path,
             (unsigned long long)session_id);
  }
  if (gpx_tmp_path != NULL && gpx_tmp_size > 0) {
    snprintf(gpx_tmp_path, gpx_tmp_size, "%s/" WG_STORAGE_FILE_PREFIX "%016llX.gpx.tmp", base_path,
             (unsigned long long)session_id);
  }
}

static void storage_dump_journal_path(char *path_out, size_t path_size) {
  if (path_out == NULL || path_size == 0) {
    return;
  }
  snprintf(path_out, path_size, "%s/%s", storage_base_path(), WG_WIFI_DUMP_JOURNAL_FILE);
}

static void storage_dump_reset_sessions_locked(void) {
  if (s_wifi_dump_sessions != NULL) {
    free(s_wifi_dump_sessions);
  }
  s_wifi_dump_sessions = NULL;
  s_wifi_dump_session_count = 0;
  s_wifi_dump_session_capacity = 0;
}

static bool storage_dump_ensure_capacity_locked(size_t needed) {
  if (needed <= s_wifi_dump_session_capacity) {
    return true;
  }
  size_t next_cap = s_wifi_dump_session_capacity > 0 ? s_wifi_dump_session_capacity : 8;
  while (next_cap < needed) {
    if (next_cap > (SIZE_MAX / 2U)) {
      next_cap = needed;
      break;
    }
    next_cap *= 2U;
  }
  wg_dump_session_t *grown = (wg_dump_session_t *)realloc(
      s_wifi_dump_sessions, next_cap * sizeof(wg_dump_session_t));
  if (grown == NULL) {
    return false;
  }
  s_wifi_dump_sessions = grown;
  s_wifi_dump_session_capacity = next_cap;
  return true;
}

static int storage_dump_session_cmp(const void *a, const void *b) {
  const wg_dump_session_t *lhs = (const wg_dump_session_t *)a;
  const wg_dump_session_t *rhs = (const wg_dump_session_t *)b;
  if (lhs->session_id < rhs->session_id) {
    return -1;
  }
  if (lhs->session_id > rhs->session_id) {
    return 1;
  }
  return 0;
}

static bool storage_dump_collect_sessions_locked(void) {
  storage_dump_reset_sessions_locked();

  DIR *dir = opendir(storage_base_path());
  if (dir == NULL) {
    return false;
  }

  struct dirent *ent = NULL;
  while ((ent = readdir(dir)) != NULL) {
    uint64_t sid = 0;
    if (!storage_parse_meta_filename(ent->d_name, &sid)) {
      continue;
    }

    wg_session_manifest_t manifest = {0};
    if (!storage_read_manifest(sid, &manifest)) {
      continue;
    }
    if (manifest.state == WG_SESSION_STATE_OPEN || manifest.records_written == 0 ||
        manifest.records_acked >= manifest.records_written ||
        manifest.last_seq_written == 0) {
      continue;
    }

    if (!storage_dump_ensure_capacity_locked(s_wifi_dump_session_count + 1U)) {
      closedir(dir);
      return false;
    }

    char data_path[96] = {0};
    char gpx_path[WG_STORAGE_PATH_BUF_SIZE] = {0};
    storage_session_paths(sid, NULL, 0, data_path, sizeof(data_path));
    storage_gpx_paths(sid, gpx_path, sizeof(gpx_path), NULL, 0);
    struct stat st_data = {0};
    struct stat st_gpx = {0};
    uint32_t data_bytes = 0;
    uint32_t gpx_bytes = 0;
    if (stat(data_path, &st_data) == 0 && st_data.st_size > 0) {
      if ((uint64_t)st_data.st_size > UINT32_MAX) {
        data_bytes = UINT32_MAX;
      } else {
        data_bytes = (uint32_t)st_data.st_size;
      }
    }
    if (stat(gpx_path, &st_gpx) == 0 && st_gpx.st_size > 0) {
      if ((uint64_t)st_gpx.st_size > UINT32_MAX) {
        gpx_bytes = UINT32_MAX;
      } else {
        gpx_bytes = (uint32_t)st_gpx.st_size;
      }
    }
    if (data_bytes == 0) {
      continue;
    }

    wg_dump_session_t *slot = &s_wifi_dump_sessions[s_wifi_dump_session_count++];
    slot->session_id = sid;
    slot->data_bytes = data_bytes;
    slot->gpx_bytes = gpx_bytes;
    slot->written_seq = manifest.last_seq_written;
    slot->acked_seq = manifest.last_seq_acked;
  }
  closedir(dir);

  if (s_wifi_dump_session_count > 1U) {
    qsort(s_wifi_dump_sessions, s_wifi_dump_session_count, sizeof(wg_dump_session_t),
          storage_dump_session_cmp);
  }
  return true;
}

static double gpx_distance_meters_e7(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7) {
  static const double k_earth_radius_m = 6371000.0;
  const double lat1 = ((double)lat1_e7 / 10000000.0) * (M_PI / 180.0);
  const double lat2 = ((double)lat2_e7 / 10000000.0) * (M_PI / 180.0);
  const double dlat = ((double)(lat2_e7 - lat1_e7) / 10000000.0) * (M_PI / 180.0);
  const double dlon = ((double)(lon2_e7 - lon1_e7) / 10000000.0) * (M_PI / 180.0);
  const double sin_dlat = sin(dlat / 2.0);
  const double sin_dlon = sin(dlon / 2.0);
  const double h = sin_dlat * sin_dlat + cos(lat1) * cos(lat2) * sin_dlon * sin_dlon;
  const double c = 2.0 * atan2(sqrt(h), sqrt(fmax(0.0, 1.0 - h)));
  return k_earth_radius_m * c;
}

static bool gpx_format_time_utc(uint32_t unix_time_s, char *out, size_t out_size) {
  if (out == NULL || out_size == 0) {
    return false;
  }
  if (unix_time_s == 0) {
    unix_time_s = (uint32_t)(now_ms() / 1000LL);
  }
  time_t epoch = (time_t)unix_time_s;
  struct tm tm_utc = {0};
  if (gmtime_r(&epoch, &tm_utc) == NULL) {
    return false;
  }
  return strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) > 0;
}

static void storage_reset_gpx_state_locked(bool close_file, bool remove_tmp) {
  if (close_file && s_gpx_fp != NULL) {
    fclose(s_gpx_fp);
  }
  s_gpx_fp = NULL;
  if (remove_tmp && s_gpx_tmp_path[0] != '\0') {
    (void)unlink(s_gpx_tmp_path);
  }
  s_gpx_session_id = 0;
  s_gpx_path[0] = '\0';
  s_gpx_tmp_path[0] = '\0';
  s_gpx_has_last_point = false;
  s_gpx_last_lat_e7 = 0;
  s_gpx_last_lon_e7 = 0;
  s_gpx_last_point_ms = 0;
  s_gpx_last_flush_ms = 0;
  s_gpx_points_written = 0;
}

static bool storage_open_gpx_session_locked(uint64_t session_id) {
  if (session_id == 0) {
    return false;
  }
  storage_reset_gpx_state_locked(true, false);
  storage_gpx_paths(session_id, s_gpx_path, sizeof(s_gpx_path), s_gpx_tmp_path, sizeof(s_gpx_tmp_path));
  (void)unlink(s_gpx_tmp_path);

  FILE *fp = fopen(s_gpx_tmp_path, "wb");
  if (fp == NULL) {
    ESP_LOGW(TAG, "gpx open failed sid=%016llX path=%s errno=%d", (unsigned long long)session_id,
             s_gpx_tmp_path, errno);
    storage_reset_gpx_state_locked(false, false);
    return false;
  }

  int n = fprintf(fp,
                  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                  "<gpx version=\"1.1\" creator=\"wifinder\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
                  "<trk><name>session-%016llX</name><trkseg>\n",
                  (unsigned long long)session_id);
  if (n <= 0 || !storage_commit_file(fp, "gpx header", session_id)) {
    ESP_LOGW(TAG, "gpx header failed sid=%016llX errno=%d", (unsigned long long)session_id, errno);
    fclose(fp);
    (void)unlink(s_gpx_tmp_path);
    storage_reset_gpx_state_locked(false, false);
    return false;
  }

  s_gpx_fp = fp;
  s_gpx_session_id = session_id;
  s_gpx_last_flush_ms = now_ms();
  return true;
}

static void storage_close_gpx_session_locked(bool finalize) {
  FILE *fp = s_gpx_fp;
  if (fp == NULL) {
    storage_reset_gpx_state_locked(false, false);
    return;
  }

  bool ok = true;
  if (finalize) {
    if (fprintf(fp, "</trkseg></trk></gpx>\n") <= 0) {
      ok = false;
    }
  }
  if (ok && !storage_commit_file(fp, finalize ? "gpx finalize" : "gpx close", s_gpx_session_id)) {
    ok = false;
  }
  fclose(fp);
  s_gpx_fp = NULL;

  if (finalize && ok) {
    (void)unlink(s_gpx_path);
    if (rename(s_gpx_tmp_path, s_gpx_path) != 0) {
      ESP_LOGW(TAG, "gpx rename failed sid=%016llX from=%s to=%s errno=%d",
               (unsigned long long)s_gpx_session_id, s_gpx_tmp_path, s_gpx_path, errno);
      ok = false;
    }
  }
  if (!ok) {
    ESP_LOGW(TAG, "gpx close incomplete sid=%016llX", (unsigned long long)s_gpx_session_id);
  }
  storage_reset_gpx_state_locked(false, !ok || !finalize);
}

static bool storage_finalize_stale_gpx_session(uint64_t session_id) {
  if (session_id == 0) {
    return false;
  }
  char gpx_path[WG_STORAGE_PATH_BUF_SIZE] = {0};
  char gpx_tmp_path[WG_STORAGE_PATH_BUF_SIZE] = {0};
  storage_gpx_paths(session_id, gpx_path, sizeof(gpx_path), gpx_tmp_path, sizeof(gpx_tmp_path));
  struct stat st = {0};
  if (stat(gpx_tmp_path, &st) != 0 || st.st_size <= 0) {
    return false;
  }
  FILE *fp = fopen(gpx_tmp_path, "ab");
  if (fp == NULL) {
    return false;
  }
  bool ok = (fprintf(fp, "</trkseg></trk></gpx>\n") > 0) &&
            storage_commit_file(fp, "gpx stale finalize", session_id);
  fclose(fp);
  if (!ok) {
    return false;
  }
  (void)unlink(gpx_path);
  if (rename(gpx_tmp_path, gpx_path) != 0) {
    ESP_LOGW(TAG, "gpx stale rename failed sid=%016llX errno=%d", (unsigned long long)session_id, errno);
    return false;
  }
  return true;
}

static bool storage_parse_meta_filename(const char *name, uint64_t *session_id_out) {
  if (name == NULL || session_id_out == NULL) {
    return false;
  }
  const size_t prefix_len = strlen(WG_STORAGE_FILE_PREFIX);
  const size_t n = strlen(name);
  if (n != (prefix_len + 16 + 5) || strncmp(name, WG_STORAGE_FILE_PREFIX, prefix_len) != 0 ||
      strcmp(name + prefix_len + 16, ".meta") != 0) {
    return false;
  }
  char hex[17] = {0};
  memcpy(hex, name + prefix_len, 16);
  errno = 0;
  char *end = NULL;
  unsigned long long parsed = strtoull(hex, &end, 16);
  if (errno != 0 || end == NULL || *end != '\0') {
    return false;
  }
  *session_id_out = (uint64_t)parsed;
  return true;
}

static uint32_t storage_manifest_generation_get(const wg_session_manifest_t *manifest,
                                                bool *valid_out) {
  bool valid = (manifest != NULL && manifest->reserved1[2] == WG_STORAGE_MANIFEST_GEN_MAGIC);
  if (valid_out != NULL) {
    *valid_out = valid;
  }
  if (!valid) {
    return 0;
  }
  return (uint32_t)manifest->reserved0 | ((uint32_t)manifest->reserved1[0] << 16) |
         ((uint32_t)manifest->reserved1[1] << 24);
}

static void storage_manifest_generation_set(wg_session_manifest_t *manifest, uint32_t generation) {
  if (manifest == NULL) {
    return;
  }
  manifest->reserved0 = (uint16_t)(generation & 0xFFFFU);
  manifest->reserved1[0] = (uint8_t)((generation >> 16) & 0xFFU);
  manifest->reserved1[1] = (uint8_t)((generation >> 24) & 0xFFU);
  manifest->reserved1[2] = WG_STORAGE_MANIFEST_GEN_MAGIC;
}

static int storage_manifest_generation_cmp(uint32_t lhs, uint32_t rhs) {
  if (lhs == rhs) {
    return 0;
  }
  uint32_t delta = lhs - rhs;
  return (delta < 0x80000000U) ? 1 : -1;
}

static int storage_manifest_compare_freshness(const wg_session_manifest_t *lhs,
                                              const wg_session_manifest_t *rhs) {
  if (lhs == NULL || rhs == NULL) {
    return 0;
  }
  bool lhs_has_gen = false;
  bool rhs_has_gen = false;
  uint32_t lhs_gen = storage_manifest_generation_get(lhs, &lhs_has_gen);
  uint32_t rhs_gen = storage_manifest_generation_get(rhs, &rhs_has_gen);
  if (lhs_has_gen || rhs_has_gen) {
    if (lhs_has_gen && rhs_has_gen) {
      int cmp = storage_manifest_generation_cmp(lhs_gen, rhs_gen);
      if (cmp != 0) {
        return cmp;
      }
    } else {
      return lhs_has_gen ? 1 : -1;
    }
  }

  if (lhs->last_seq_written != rhs->last_seq_written) {
    return (lhs->last_seq_written > rhs->last_seq_written) ? 1 : -1;
  }
  if (lhs->records_written != rhs->records_written) {
    return (lhs->records_written > rhs->records_written) ? 1 : -1;
  }
  if (lhs->records_acked != rhs->records_acked) {
    return (lhs->records_acked > rhs->records_acked) ? 1 : -1;
  }
  if (lhs->state != rhs->state) {
    return (lhs->state > rhs->state) ? 1 : -1;
  }
  return 0;
}

static bool storage_read_manifest_path(const char *meta_path, wg_session_manifest_t *out) {
  if (meta_path == NULL || out == NULL) {
    return false;
  }
  FILE *fp = fopen(meta_path, "rb");
  if (fp == NULL) {
    return false;
  }
  wg_session_manifest_t manifest = {0};
  size_t n = fread(&manifest, 1, sizeof(manifest), fp);
  fclose(fp);
  if (n != sizeof(manifest) || manifest.magic != WG_STORAGE_META_MAGIC ||
      manifest.version != WG_STORAGE_VERSION || manifest.size != sizeof(manifest)) {
    return false;
  }
  uint32_t crc_expected = manifest.crc32;
  manifest.crc32 = 0;
  uint32_t crc_actual = crc32_ieee((const uint8_t *)&manifest, sizeof(manifest));
  if (crc_expected != crc_actual) {
    return false;
  }
  manifest.crc32 = crc_expected;
  *out = manifest;
  return true;
}

static bool storage_read_manifest(uint64_t session_id, wg_session_manifest_t *out) {
  if (out == NULL || session_id == 0) {
    return false;
  }
  char meta_a_path[96] = {0};
  char meta_b_path[100] = {0};
  storage_manifest_paths(session_id, meta_a_path, sizeof(meta_a_path), meta_b_path, sizeof(meta_b_path));

  wg_session_manifest_t slot_a_manifest = {0};
  wg_session_manifest_t slot_b_manifest = {0};
  bool has_a = storage_read_manifest_path(meta_a_path, &slot_a_manifest) &&
               slot_a_manifest.session_id == session_id;
  bool has_b = storage_read_manifest_path(meta_b_path, &slot_b_manifest) &&
               slot_b_manifest.session_id == session_id;
  if (!has_a && !has_b) {
    return false;
  }
  if (has_a && has_b) {
    *out = (storage_manifest_compare_freshness(&slot_a_manifest, &slot_b_manifest) >= 0)
               ? slot_a_manifest
               : slot_b_manifest;
    return true;
  }
  *out = has_a ? slot_a_manifest : slot_b_manifest;
  return true;
}

static bool storage_commit_file(FILE *fp, const char *kind, uint64_t session_id) {
  if (fp == NULL || kind == NULL) {
    return false;
  }
  if (fflush(fp) != 0) {
    ESP_LOGW(TAG, "%s flush failed sid=%016llX errno=%d", kind, (unsigned long long)session_id, errno);
    return false;
  }
  int fd = fileno(fp);
  if (fd < 0) {
    ESP_LOGW(TAG, "%s fileno failed sid=%016llX errno=%d", kind, (unsigned long long)session_id, errno);
    return false;
  }
  if (fsync(fd) != 0) {
    int saved_errno = errno;
    if (saved_errno == EINVAL || saved_errno == ENOSYS
#ifdef ENOTSUP
        || saved_errno == ENOTSUP
#endif
#ifdef EOPNOTSUPP
        || saved_errno == EOPNOTSUPP
#endif
    ) {
      if (!s_storage_fsync_unsupported_warned) {
        s_storage_fsync_unsupported_warned = true;
        ESP_LOGW(TAG, "%s fsync unsupported sid=%016llX errno=%d", kind,
                 (unsigned long long)session_id, saved_errno);
      }
      return true;
    }
    ESP_LOGW(TAG, "%s fsync failed sid=%016llX errno=%d", kind, (unsigned long long)session_id,
             saved_errno);
    return false;
  }
  return true;
}

static bool storage_write_manifest_path(const char *meta_path, const wg_session_manifest_t *manifest) {
  if (meta_path == NULL || manifest == NULL) {
    return false;
  }
  FILE *fp = fopen(meta_path, "r+b");
  if (fp == NULL) {
    fp = fopen(meta_path, "wb");
  }
  if (fp == NULL) {
    ESP_LOGW(TAG, "manifest write open failed sid=%016llX path=%s errno=%d",
             (unsigned long long)manifest->session_id, meta_path, errno);
    return false;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    ESP_LOGW(TAG, "manifest seek failed sid=%016llX path=%s errno=%d",
             (unsigned long long)manifest->session_id, meta_path, errno);
    fclose(fp);
    return false;
  }
  size_t written = fwrite(manifest, 1, sizeof(*manifest), fp);
  bool committed = (written == sizeof(*manifest)) &&
                   storage_commit_file(fp, "manifest write", manifest->session_id);
  fclose(fp);
  if (!committed) {
    ESP_LOGW(TAG, "manifest write failed sid=%016llX path=%s errno=%d",
             (unsigned long long)manifest->session_id, meta_path, errno);
    return false;
  }
  return true;
}

static bool storage_write_manifest(const wg_session_manifest_t *manifest_in) {
  if (manifest_in == NULL) {
    return false;
  }
  wg_session_manifest_t manifest = *manifest_in;
  manifest.magic = WG_STORAGE_META_MAGIC;
  manifest.version = WG_STORAGE_VERSION;
  manifest.size = sizeof(manifest);
  char meta_a_path[96] = {0};
  char meta_b_path[100] = {0};
  storage_manifest_paths(manifest.session_id, meta_a_path, sizeof(meta_a_path), meta_b_path,
                         sizeof(meta_b_path));

  wg_session_manifest_t slot_a_manifest = {0};
  wg_session_manifest_t slot_b_manifest = {0};
  bool has_a = storage_read_manifest_path(meta_a_path, &slot_a_manifest) &&
               slot_a_manifest.session_id == manifest.session_id;
  bool has_b = storage_read_manifest_path(meta_b_path, &slot_b_manifest) &&
               slot_b_manifest.session_id == manifest.session_id;

  bool write_slot_a = true;
  uint32_t next_generation = 1;
  if (has_a || has_b) {
    bool newer_is_a = has_a;
    wg_session_manifest_t newer_manifest = {0};
    if (has_a && has_b) {
      newer_is_a = (storage_manifest_compare_freshness(&slot_a_manifest, &slot_b_manifest) >= 0);
      newer_manifest = newer_is_a ? slot_a_manifest : slot_b_manifest;
    } else {
      newer_manifest = has_a ? slot_a_manifest : slot_b_manifest;
    }
    bool has_gen = false;
    uint32_t current_generation = storage_manifest_generation_get(&newer_manifest, &has_gen);
    if (has_gen) {
      next_generation = current_generation + 1U;
      if (next_generation == 0) {
        next_generation = 1;
      }
    }
    write_slot_a = !newer_is_a;
  }
  storage_manifest_generation_set(&manifest, next_generation);
  manifest.crc32 = 0;
  manifest.crc32 = crc32_ieee((const uint8_t *)&manifest, sizeof(manifest));

  const char *primary_path = write_slot_a ? meta_a_path : meta_b_path;
  const char *fallback_path = write_slot_a ? meta_b_path : meta_a_path;
  if (storage_write_manifest_path(primary_path, &manifest)) {
    return true;
  }
  return storage_write_manifest_path(fallback_path, &manifest);
}

static bool storage_dump_journal_write_locked(bool committed) {
  char journal_path[WG_STORAGE_PATH_BUF_SIZE] = {0};
  storage_dump_journal_path(journal_path, sizeof(journal_path));
  FILE *fp = fopen(journal_path, "wb");
  if (fp == NULL) {
    ESP_LOGW(TAG, "wifi dump journal open failed path=%s errno=%d", journal_path, errno);
    return false;
  }

  const uint32_t session_count =
      (s_wifi_dump_session_count > UINT32_MAX) ? UINT32_MAX : (uint32_t)s_wifi_dump_session_count;
  const size_t entries_size = (size_t)session_count * sizeof(wg_dump_journal_entry_t);
  uint8_t *entries_buf = NULL;
  if (entries_size > 0) {
    entries_buf = (uint8_t *)calloc(1, entries_size);
    if (entries_buf == NULL) {
      fclose(fp);
      return false;
    }
    for (uint32_t i = 0; i < session_count; ++i) {
      wg_dump_journal_entry_t *entry = (wg_dump_journal_entry_t *)(entries_buf + (i * sizeof(*entry)));
      entry->session_id = s_wifi_dump_sessions[i].session_id;
      entry->ack_seq = s_wifi_dump_sessions[i].written_seq;
    }
  }

  wg_dump_journal_header_t header = {
      .magic = WG_WIFI_DUMP_JOURNAL_MAGIC,
      .version = WG_WIFI_DUMP_JOURNAL_VERSION,
      .header_size = sizeof(wg_dump_journal_header_t),
      .run_id = s_wifi_dump_run_id,
      .committed = committed ? 1 : 0,
      .reserved = {0},
      .session_count = session_count,
      .crc32 = entries_size > 0 ? crc32_ieee(entries_buf, entries_size) : 0,
  };

  bool ok = (fwrite(&header, 1, sizeof(header), fp) == sizeof(header));
  if (ok && entries_size > 0) {
    ok = (fwrite(entries_buf, 1, entries_size, fp) == entries_size);
  }
  if (entries_buf != NULL) {
    free(entries_buf);
  }
  if (ok) {
    ok = storage_commit_file(fp, "wifi dump journal", s_wifi_dump_run_id);
  }
  fclose(fp);
  if (!ok) {
    ESP_LOGW(TAG, "wifi dump journal write failed run=%016llX errno=%d",
             (unsigned long long)s_wifi_dump_run_id, errno);
  }
  return ok;
}

static bool storage_dump_journal_load_locked(wg_dump_journal_header_t *header_out,
                                             wg_dump_journal_entry_t **entries_out) {
  if (header_out == NULL || entries_out == NULL) {
    return false;
  }
  *entries_out = NULL;
  memset(header_out, 0, sizeof(*header_out));

  char journal_path[WG_STORAGE_PATH_BUF_SIZE] = {0};
  storage_dump_journal_path(journal_path, sizeof(journal_path));
  FILE *fp = fopen(journal_path, "rb");
  if (fp == NULL) {
    return false;
  }

  wg_dump_journal_header_t header = {0};
  bool ok = (fread(&header, 1, sizeof(header), fp) == sizeof(header));
  if (!ok || header.magic != WG_WIFI_DUMP_JOURNAL_MAGIC ||
      header.version != WG_WIFI_DUMP_JOURNAL_VERSION ||
      header.header_size != sizeof(wg_dump_journal_header_t)) {
    fclose(fp);
    return false;
  }

  const size_t entries_size = (size_t)header.session_count * sizeof(wg_dump_journal_entry_t);
  wg_dump_journal_entry_t *entries = NULL;
  if (entries_size > 0) {
    entries = (wg_dump_journal_entry_t *)calloc(1, entries_size);
    if (entries == NULL) {
      fclose(fp);
      return false;
    }
    ok = (fread(entries, 1, entries_size, fp) == entries_size);
    if (!ok) {
      free(entries);
      fclose(fp);
      return false;
    }
    const uint32_t crc = crc32_ieee((const uint8_t *)entries, entries_size);
    if (crc != header.crc32) {
      free(entries);
      fclose(fp);
      return false;
    }
  } else if (header.crc32 != 0) {
    fclose(fp);
    return false;
  }

  fclose(fp);
  *header_out = header;
  *entries_out = entries;
  return true;
}

static bool storage_dump_journal_delete_locked(void) {
  char journal_path[WG_STORAGE_PATH_BUF_SIZE] = {0};
  storage_dump_journal_path(journal_path, sizeof(journal_path));
  if (unlink(journal_path) == 0 || errno == ENOENT) {
    return true;
  }
  ESP_LOGW(TAG, "wifi dump journal delete failed path=%s errno=%d", journal_path, errno);
  return false;
}

static const wg_dump_session_t *storage_dump_find_session_locked(uint64_t sid) {
  if (sid == 0 || s_wifi_dump_sessions == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < s_wifi_dump_session_count; ++i) {
    if (s_wifi_dump_sessions[i].session_id == sid) {
      return &s_wifi_dump_sessions[i];
    }
  }
  return NULL;
}

static bool wifi_dump_parse_query_value(httpd_req_t *req, const char *key, char *value_out,
                                        size_t value_size) {
  if (req == NULL || key == NULL || value_out == NULL || value_size == 0) {
    return false;
  }
  int query_len = httpd_req_get_url_query_len(req);
  if (query_len <= 0 || query_len >= 192) {
    return false;
  }
  char query[192] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return false;
  }
  return httpd_query_key_value(query, key, value_out, value_size) == ESP_OK;
}

static int wifi_dump_manifest_handler(httpd_req_t *req) {
  if (req == NULL) {
    return ESP_FAIL;
  }
  if (!storage_lock(pdMS_TO_TICKS(200))) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "storage lock failed");
    return ESP_FAIL;
  }
  if (!s_wifi_dump_active) {
    storage_unlock();
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "wifi dump not active");
    return ESP_FAIL;
  }

  uint64_t run_id = s_wifi_dump_run_id;
  size_t count = s_wifi_dump_session_count;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  char line[256] = {0};
  snprintf(line, sizeof(line),
           "{\"run_id\":\"%016llX\",\"ap_ssid\":\"%s\",\"session_count\":%lu,\"sessions\":[",
           (unsigned long long)run_id, WG_WIFI_DUMP_AP_SSID, (unsigned long)count);
  esp_err_t rc = httpd_resp_sendstr_chunk(req, line);
  if (rc != ESP_OK) {
    storage_unlock();
    return rc;
  }

  for (size_t i = 0; i < count; ++i) {
    const wg_dump_session_t *s = &s_wifi_dump_sessions[i];
    snprintf(line, sizeof(line),
             "%s{\"sid\":\"%016llX\",\"dat_bytes\":%lu,\"gpx_bytes\":%lu,\"written_seq\":%lu,\"acked_seq\":%lu}",
             (i == 0U) ? "" : ",", (unsigned long long)s->session_id,
             (unsigned long)s->data_bytes, (unsigned long)s->gpx_bytes,
             (unsigned long)s->written_seq, (unsigned long)s->acked_seq);
    rc = httpd_resp_sendstr_chunk(req, line);
    if (rc != ESP_OK) {
      storage_unlock();
      return rc;
    }
  }
  storage_unlock();

  if (httpd_resp_sendstr_chunk(req, "]}") != ESP_OK) {
    return ESP_FAIL;
  }
  return httpd_resp_send_chunk(req, NULL, 0);
}

static int wifi_dump_file_handler(httpd_req_t *req) {
  if (req == NULL) {
    return ESP_FAIL;
  }
  char sid_hex[24] = {0};
  char type[8] = {0};
  if (!wifi_dump_parse_query_value(req, "sid", sid_hex, sizeof(sid_hex)) ||
      !wifi_dump_parse_query_value(req, "type", type, sizeof(type))) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "missing sid/type");
    return ESP_FAIL;
  }

  errno = 0;
  char *sid_end = NULL;
  uint64_t sid = strtoull(sid_hex, &sid_end, 16);
  if (errno != 0 || sid_end == sid_hex || (sid_end != NULL && *sid_end != '\0')) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "bad sid");
    return ESP_FAIL;
  }

  bool want_dat = strcmp(type, "dat") == 0;
  bool want_gpx = strcmp(type, "gpx") == 0;
  if (!want_dat && !want_gpx) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "bad type");
    return ESP_FAIL;
  }

  char path[WG_STORAGE_PATH_BUF_SIZE] = {0};
  uint32_t expected_bytes = 0;
  if (!storage_lock(pdMS_TO_TICKS(200))) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "storage lock failed");
    return ESP_FAIL;
  }
  if (!s_wifi_dump_active) {
    storage_unlock();
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "wifi dump not active");
    return ESP_FAIL;
  }
  const wg_dump_session_t *session = storage_dump_find_session_locked(sid);
  if (session == NULL) {
    storage_unlock();
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_sendstr(req, "unknown session");
    return ESP_FAIL;
  }
  if (want_dat) {
    storage_session_paths(sid, NULL, 0, path, sizeof(path));
    expected_bytes = session->data_bytes;
  } else {
    storage_gpx_paths(sid, path, sizeof(path), NULL, 0);
    expected_bytes = session->gpx_bytes;
  }
  storage_unlock();

  if (expected_bytes == 0) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_sendstr(req, "file absent");
    return ESP_FAIL;
  }

  struct stat st = {0};
  if (stat(path, &st) != 0 || st.st_size <= 0) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_sendstr(req, "file missing");
    return ESP_FAIL;
  }
  uint32_t file_size = ((uint64_t)st.st_size > UINT32_MAX) ? UINT32_MAX : (uint32_t)st.st_size;
  if (file_size == 0) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_sendstr(req, "empty file");
    return ESP_FAIL;
  }

  uint32_t start = 0;
  bool partial = false;
  int range_len = httpd_req_get_hdr_value_len(req, "Range");
  if (range_len > 0 && range_len < 64) {
    char range[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) == ESP_OK &&
        strncmp(range, "bytes=", 6) == 0) {
      errno = 0;
      char *end = NULL;
      unsigned long parsed = strtoul(range + 6, &end, 10);
      if (errno == 0 && end != (range + 6)) {
        if (parsed > UINT32_MAX) {
          start = UINT32_MAX;
        } else {
          start = (uint32_t)parsed;
        }
        partial = true;
      }
    }
  }
  if (start >= file_size) {
    httpd_resp_set_status(req, "416 Range Not Satisfiable");
    httpd_resp_sendstr(req, "range out of bounds");
    return ESP_FAIL;
  }

  FILE *fp = fopen(path, "rb");
  if (fp == NULL) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "open failed");
    return ESP_FAIL;
  }
  if (start > 0 && fseek(fp, (long)start, SEEK_SET) != 0) {
    fclose(fp);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "seek failed");
    return ESP_FAIL;
  }

  uint32_t remain = file_size - start;
  char hdr[96] = {0};
  if (want_dat) {
    httpd_resp_set_type(req, "application/octet-stream");
  } else {
    httpd_resp_set_type(req, "application/gpx+xml");
  }
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (partial) {
    httpd_resp_set_status(req, "206 Partial Content");
    snprintf(hdr, sizeof(hdr), "bytes %lu-%lu/%lu", (unsigned long)start,
             (unsigned long)(file_size - 1U), (unsigned long)file_size);
    httpd_resp_set_hdr(req, "Content-Range", hdr);
  }
  snprintf(hdr, sizeof(hdr), "%lu", (unsigned long)remain);
  httpd_resp_set_hdr(req, "Content-Length", hdr);

  uint8_t chunk[WG_WIFI_DUMP_HTTP_CHUNK] = {0};
  while (remain > 0) {
    size_t want = remain > sizeof(chunk) ? sizeof(chunk) : (size_t)remain;
    size_t n = fread(chunk, 1, want, fp);
    if (n == 0) {
      fclose(fp);
      httpd_resp_send_chunk(req, NULL, 0);
      return ESP_FAIL;
    }
    if (httpd_resp_send_chunk(req, (const char *)chunk, n) != ESP_OK) {
      fclose(fp);
      return ESP_FAIL;
    }
    remain -= (uint32_t)n;
  }
  fclose(fp);
  return httpd_resp_send_chunk(req, NULL, 0);
}

static bool wifi_dump_http_start_locked(void) {
  if (s_wifi_dump_httpd != NULL) {
    return true;
  }
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = WG_WIFI_DUMP_HTTP_PORT;
  cfg.max_uri_handlers = 6;
  cfg.stack_size = 6144;
  cfg.lru_purge_enable = true;
  cfg.max_open_sockets = 4;

  if (httpd_start(&s_wifi_dump_httpd, &cfg) != ESP_OK) {
    s_wifi_dump_httpd = NULL;
    return false;
  }

  const httpd_uri_t manifest_uri = {
      .uri = WG_WIFI_DUMP_MANIFEST_URI,
      .method = HTTP_GET,
      .handler = wifi_dump_manifest_handler,
      .user_ctx = NULL,
  };
  const httpd_uri_t file_uri = {
      .uri = WG_WIFI_DUMP_FILE_URI,
      .method = HTTP_GET,
      .handler = wifi_dump_file_handler,
      .user_ctx = NULL,
  };
  if (httpd_register_uri_handler(s_wifi_dump_httpd, &manifest_uri) != ESP_OK ||
      httpd_register_uri_handler(s_wifi_dump_httpd, &file_uri) != ESP_OK) {
    httpd_stop(s_wifi_dump_httpd);
    s_wifi_dump_httpd = NULL;
    return false;
  }
  return true;
}

static void wifi_dump_http_stop_locked(void) {
  if (s_wifi_dump_httpd != NULL) {
    httpd_handle_t handle = s_wifi_dump_httpd;
    s_wifi_dump_httpd = NULL;
    httpd_stop(handle);
  }
}

static bool wifi_dump_ap_start_locked(void) {
  if (s_wifi_dump_ap_running) {
    return true;
  }
  wifi_config_t ap_cfg = {0};
  memcpy(ap_cfg.ap.ssid, WG_WIFI_DUMP_AP_SSID, strlen(WG_WIFI_DUMP_AP_SSID));
  ap_cfg.ap.ssid_len = strlen(WG_WIFI_DUMP_AP_SSID);
  ap_cfg.ap.channel = WG_WIFI_DUMP_AP_CHANNEL;
  ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
  ap_cfg.ap.max_connection = 2;
  ap_cfg.ap.beacon_interval = 100;

  esp_err_t rc = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (rc != ESP_OK) {
    ESP_LOGW(TAG, "wifi dump set mode APSTA failed rc=%s", esp_err_to_name(rc));
    return false;
  }
  rc = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
  if (rc != ESP_OK) {
    ESP_LOGW(TAG, "wifi dump AP config failed rc=%s", esp_err_to_name(rc));
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    return false;
  }
  s_wifi_dump_ap_running = true;
  ESP_LOGI(TAG, "wifi dump AP ready ssid=%s channel=%u", WG_WIFI_DUMP_AP_SSID,
           (unsigned)WG_WIFI_DUMP_AP_CHANNEL);
  return true;
}

static void wifi_dump_ap_stop_locked(void) {
  if (!s_wifi_dump_ap_running) {
    return;
  }
  (void)esp_wifi_set_mode(WIFI_MODE_STA);
  s_wifi_dump_ap_running = false;
}

static bool storage_dump_start_locked(void) {
  if (!s_storage_ready || s_scanning) {
    return false;
  }

  s_replay_requested = false;
  storage_reset_replay_locked();
  s_blob_requested = false;
  storage_reset_blob_locked();
  (void)storage_flush_pending_locked(true);

  if (s_session_open) {
    storage_close_session_locked(WG_SESSION_STATE_CLOSED);
  }
  if (!storage_dump_collect_sessions_locked()) {
    return false;
  }
  s_wifi_dump_run_id = ((uint64_t)(uint32_t)now_ms() << 32) ^ (uint64_t)esp_random();
  if (s_wifi_dump_run_id == 0) {
    s_wifi_dump_run_id = 1;
  }
  if (!storage_dump_journal_write_locked(false)) {
    storage_dump_reset_sessions_locked();
    s_wifi_dump_run_id = 0;
    return false;
  }
  s_wifi_dump_requested = true;
  s_wifi_dump_active = true;
  s_wifi_dump_started_ms = now_ms();
  ESP_LOGI(TAG, "wifi dump start run=%016llX sessions=%lu", (unsigned long long)s_wifi_dump_run_id,
           (unsigned long)s_wifi_dump_session_count);
  return true;
}

static void storage_dump_stop_locked(bool keep_journal) {
  s_wifi_dump_requested = false;
  s_wifi_dump_active = false;
  s_wifi_dump_run_id = 0;
  s_wifi_dump_started_ms = 0;
  storage_dump_reset_sessions_locked();
  if (!keep_journal) {
    (void)storage_dump_journal_delete_locked();
  }
}

static bool storage_dump_commit_locked(uint64_t run_id) {
  if (!s_wifi_dump_active || run_id == 0 || run_id != s_wifi_dump_run_id) {
    return false;
  }
  return storage_dump_journal_write_locked(true);
}

static bool storage_set_wifi_dump_enabled(bool enabled) {
  bool ok = true;
  if (!storage_lock(pdMS_TO_TICKS(500))) {
    return false;
  }

  if (enabled) {
    if (s_wifi_dump_active || s_wifi_dump_requested) {
      storage_unlock();
      notify_status_frame();
      return true;
    }
    if (!storage_dump_start_locked()) {
      storage_unlock();
      return false;
    }
    if (!wifi_dump_ap_start_locked() || !wifi_dump_http_start_locked()) {
      wifi_dump_http_stop_locked();
      wifi_dump_ap_stop_locked();
      storage_dump_stop_locked(false);
      ok = false;
    }
  } else {
    if (!s_wifi_dump_active && !s_wifi_dump_requested) {
      storage_unlock();
      notify_status_frame();
      return true;
    }
    wifi_dump_http_stop_locked();
    wifi_dump_ap_stop_locked();
    storage_dump_stop_locked(false);
  }

  storage_unlock();
  notify_status_frame();
  return ok;
}

static bool storage_commit_wifi_dump_run(uint64_t run_id) {
  if (run_id == 0) {
    return false;
  }

  wg_dump_journal_entry_t *entries = NULL;
  uint32_t session_count = 0;
  bool prepared = false;
  if (!storage_lock(pdMS_TO_TICKS(500))) {
    return false;
  }
  prepared = storage_dump_commit_locked(run_id);
  if (prepared) {
    session_count = (s_wifi_dump_session_count > UINT32_MAX) ? UINT32_MAX : (uint32_t)s_wifi_dump_session_count;
    if (session_count > 0) {
      entries = (wg_dump_journal_entry_t *)calloc(session_count, sizeof(wg_dump_journal_entry_t));
      if (entries == NULL) {
        prepared = false;
      } else {
        for (uint32_t i = 0; i < session_count; ++i) {
          entries[i].session_id = s_wifi_dump_sessions[i].session_id;
          entries[i].ack_seq = s_wifi_dump_sessions[i].written_seq;
        }
      }
    }
  }
  storage_unlock();
  if (!prepared) {
    if (entries != NULL) {
      free(entries);
    }
    return false;
  }

  bool ack_ok = true;
  for (uint32_t i = 0; i < session_count; ++i) {
    if (entries[i].session_id == 0 || entries[i].ack_seq == 0) {
      continue;
    }
    if (!storage_apply_replay_ack(entries[i].session_id, entries[i].ack_seq)) {
      ack_ok = false;
      ESP_LOGW(TAG, "wifi dump commit ack failed sid=%016llX seq=%lu",
               (unsigned long long)entries[i].session_id, (unsigned long)entries[i].ack_seq);
    }
  }
  if (entries != NULL) {
    free(entries);
  }

  if (!storage_lock(pdMS_TO_TICKS(500))) {
    return false;
  }
  if (ack_ok) {
    storage_refresh_backlog_locked(true);
    (void)storage_dump_journal_delete_locked();
  }
  wifi_dump_http_stop_locked();
  wifi_dump_ap_stop_locked();
  storage_dump_stop_locked(true);
  storage_unlock();
  notify_status_frame();

  if (ack_ok) {
    ESP_LOGI(TAG, "wifi dump commit complete run=%016llX", (unsigned long long)run_id);
  }
  return ack_ok;
}

static void storage_recover_wifi_dump_journal(void) {
  wg_dump_journal_header_t header = {0};
  wg_dump_journal_entry_t *entries = NULL;
  bool loaded = false;
  if (!storage_lock(pdMS_TO_TICKS(300))) {
    return;
  }
  loaded = storage_dump_journal_load_locked(&header, &entries);
  if (!loaded) {
    storage_unlock();
    return;
  }
  if (header.committed == 0) {
    ESP_LOGW(TAG, "wifi dump recovery: uncommitted journal retained run=%016llX sessions=%lu",
             (unsigned long long)header.run_id, (unsigned long)header.session_count);
    storage_unlock();
    if (entries != NULL) {
      free(entries);
    }
    return;
  }
  storage_unlock();

  bool ok = true;
  for (uint32_t i = 0; i < header.session_count; ++i) {
    if (entries[i].session_id == 0 || entries[i].ack_seq == 0) {
      continue;
    }
    if (!storage_apply_replay_ack(entries[i].session_id, entries[i].ack_seq)) {
      ok = false;
    }
  }
  if (entries != NULL) {
    free(entries);
  }

  if (!storage_lock(pdMS_TO_TICKS(300))) {
    return;
  }
  if (ok) {
    (void)storage_refresh_backlog_locked(true);
    (void)storage_dump_journal_delete_locked();
    ESP_LOGI(TAG, "wifi dump recovery replayed committed journal run=%016llX",
             (unsigned long long)header.run_id);
  } else {
    ESP_LOGW(TAG, "wifi dump recovery incomplete run=%016llX", (unsigned long long)header.run_id);
  }
  storage_unlock();
}

static uint64_t storage_generate_session_id(void) {
  uint32_t unix_s = s_latest_gps.unix_time_s;
  if (unix_s == 0) {
    unix_s = (uint32_t)(ble_device_ms() / 1000U);
  }
  return ((uint64_t)unix_s << 32) | (uint64_t)esp_random();
}

static void storage_reset_pending_block_locked(void) {
  s_store_block_payload_len = 0;
  s_store_block_record_count = 0;
  s_store_block_first_seq = 0;
  s_store_block_started_ms = 0;
}

static uint32_t storage_reclaim_acked_sessions_locked(void) {
  DIR *dir = opendir(storage_base_path());
  if (dir == NULL) {
    return 0;
  }
  uint32_t reclaimed = 0;
  struct dirent *ent = NULL;
  while ((ent = readdir(dir)) != NULL) {
    uint64_t sid = 0;
    if (!storage_parse_meta_filename(ent->d_name, &sid)) {
      continue;
    }
    wg_session_manifest_t manifest = {0};
    char meta_path[96] = {0};
    char meta_path_b[100] = {0};
    char data_path[96] = {0};
    char gpx_path[WG_STORAGE_PATH_BUF_SIZE] = {0};
    char gpx_tmp_path[WG_STORAGE_PATH_BUF_SIZE] = {0};
    storage_manifest_paths(sid, meta_path, sizeof(meta_path), meta_path_b, sizeof(meta_path_b));
    storage_session_paths(sid, NULL, 0, data_path, sizeof(data_path));
    storage_gpx_paths(sid, gpx_path, sizeof(gpx_path), gpx_tmp_path, sizeof(gpx_tmp_path));
    if (!storage_read_manifest(sid, &manifest)) {
      continue;
    }
    if (manifest.state == WG_SESSION_STATE_OPEN) {
      continue;
    }
    if (manifest.records_written == 0 || manifest.records_acked < manifest.records_written) {
      continue;
    }
    bool removed_any = false;
    if (unlink(meta_path) == 0) {
      removed_any = true;
    }
    if (unlink(meta_path_b) == 0) {
      removed_any = true;
    }
    if (removed_any) {
      reclaimed++;
    }
    (void)unlink(data_path);
    (void)unlink(gpx_path);
    (void)unlink(gpx_tmp_path);
  }
  closedir(dir);
  if (reclaimed > 0) {
    ESP_LOGI(TAG, "storage reclaimed sessions=%u", (unsigned)reclaimed);
  }
  return reclaimed;
}

static bool storage_flush_pending_locked(bool force) {
  if (!s_storage_ready || !s_session_open || s_store_block_record_count == 0) {
    return true;
  }
  if (!force) {
    bool near_capacity = (s_store_block_payload_len + 80U) >= sizeof(s_store_block_payload);
    int64_t age_ms = now_ms() - s_store_block_started_ms;
    if (!near_capacity && age_ms >= 0 && age_ms < WG_STORAGE_BLOCK_FLUSH_MS) {
      return true;
    }
  }

  uint64_t total = 0;
  uint64_t used = 0;
  if (!storage_get_fs_info(&total, &used)) {
    return false;
  }
  uint64_t free_bytes = (total > used) ? (total - used) : 0;
  if (free_bytes < WG_STORAGE_MIN_FREE_BYTES) {
    (void)storage_reclaim_acked_sessions_locked();
    if (storage_get_fs_info(&total, &used)) {
      free_bytes = (total > used) ? (total - used) : 0;
    }
  }
  if (free_bytes < WG_STORAGE_BLOCK_SIZE) {
    s_queue_full = true;
    s_dropped_flash_full += s_store_block_record_count;
    if (s_session_manifest.records_dropped <= UINT32_MAX - s_store_block_record_count) {
      s_session_manifest.records_dropped += s_store_block_record_count;
    } else {
      s_session_manifest.records_dropped = UINT32_MAX;
    }
    s_session_manifest.queue_full = 1;
    (void)storage_write_manifest(&s_session_manifest);
    storage_reset_pending_block_locked();
    storage_refresh_backlog_locked(false);
    return false;
  }

  char data_path[96] = {0};
  storage_session_paths(s_session_id, NULL, 0, data_path, sizeof(data_path));
  FILE *fp = fopen(data_path, "ab");
  if (fp == NULL) {
    ESP_LOGW(TAG, "block flush open failed sid=%016llX errno=%d", (unsigned long long)s_session_id,
             errno);
    return false;
  }

  uint8_t block[WG_STORAGE_BLOCK_SIZE] = {0};
  wg_store_block_header_t header = {
      .magic = WG_STORAGE_BLOCK_MAGIC,
      .version = WG_STORAGE_VERSION,
      .header_size = sizeof(wg_store_block_header_t),
      .session_id = s_session_id,
      .first_seq = s_store_block_first_seq,
      .record_count = s_store_block_record_count,
      .payload_len = s_store_block_payload_len,
      .crc32 = crc32_ieee(s_store_block_payload, s_store_block_payload_len),
  };
  memcpy(block, &header, sizeof(header));
  memcpy(block + sizeof(header), s_store_block_payload, s_store_block_payload_len);
  size_t written = fwrite(block, 1, sizeof(block), fp);
  bool committed = (written == sizeof(block)) && storage_commit_file(fp, "block flush", s_session_id);
  fclose(fp);
  if (!committed) {
    ESP_LOGW(TAG, "block flush write failed sid=%016llX", (unsigned long long)s_session_id);
    return false;
  }

  if (s_session_manifest.records_written <= UINT32_MAX - s_store_block_record_count) {
    s_session_manifest.records_written += s_store_block_record_count;
  } else {
    s_session_manifest.records_written = UINT32_MAX;
  }
  if (s_session_manifest.bytes_written <= UINT32_MAX - s_store_block_payload_len) {
    s_session_manifest.bytes_written += s_store_block_payload_len;
  } else {
    s_session_manifest.bytes_written = UINT32_MAX;
  }
  s_session_manifest.last_seq_written =
      s_store_block_first_seq + (uint32_t)s_store_block_record_count - 1U;
  s_session_manifest.queue_full = 0;
  s_queue_full = false;
  (void)storage_write_manifest(&s_session_manifest);
  storage_reset_pending_block_locked();
  storage_refresh_backlog_locked(false);
  return true;
}

static void storage_note_record_drop_locked(void) {
  if (s_dropped_flash_full < UINT32_MAX) {
    s_dropped_flash_full++;
  }
  if (s_session_open && s_session_manifest.records_dropped < UINT32_MAX) {
    s_session_manifest.records_dropped++;
    s_session_manifest.queue_full = 1;
    (void)storage_write_manifest(&s_session_manifest);
  }
  s_queue_full = true;
}

static bool storage_enqueue_record_locked(const uint8_t *payload, uint16_t payload_len, uint32_t seq) {
  if (!s_storage_ready || !s_session_open || payload == NULL || payload_len == 0 || seq == 0) {
    return false;
  }
  if ((size_t)payload_len > WG_HOST_FRAME_MAX_PAYLOAD) {
    return false;
  }
  size_t needed = WG_STORAGE_RECORD_HDR_SIZE + payload_len;
  if (needed > sizeof(s_store_block_payload)) {
    return false;
  }
  if (s_store_block_record_count > 0 &&
      (size_t)s_store_block_payload_len + needed > sizeof(s_store_block_payload)) {
    if (!storage_flush_pending_locked(true)) {
      storage_note_record_drop_locked();
      return false;
    }
  }

  if (s_store_block_record_count == 0) {
    s_store_block_first_seq = seq;
    s_store_block_started_ms = now_ms();
  }
  uint16_t pos = s_store_block_payload_len;
  s_store_block_payload[pos + 0] = (uint8_t)(payload_len & 0xFF);
  s_store_block_payload[pos + 1] = (uint8_t)((payload_len >> 8) & 0xFF);
  memcpy(&s_store_block_payload[pos + 2], payload, payload_len);
  s_store_block_payload_len = (uint16_t)((size_t)s_store_block_payload_len + needed);
  s_store_block_record_count++;

  if ((size_t)s_store_block_payload_len + 80U >= sizeof(s_store_block_payload)) {
    if (!storage_flush_pending_locked(true)) {
      storage_note_record_drop_locked();
      return false;
    }
  }
  return true;
}

static bool storage_append_gpx_point_locked(const gps_fix_t *gps, bool force_commit) {
  if (gps == NULL || !gps->valid || !s_session_open || s_session_id == 0) {
    return false;
  }
  if (s_gpx_fp == NULL || s_gpx_session_id != s_session_id) {
    if (!storage_open_gpx_session_locked(s_session_id)) {
      return false;
    }
  }

  int64_t ms = now_ms();
  if (ms < 0) {
    ms = 0;
  }
  if (!force_commit && s_gpx_has_last_point) {
    int64_t elapsed_ms = ms - s_gpx_last_point_ms;
    double distance_m =
        gpx_distance_meters_e7(s_gpx_last_lat_e7, s_gpx_last_lon_e7, gps->lat_e7, gps->lon_e7);
    if (elapsed_ms >= 0 && elapsed_ms < WG_STORAGE_GPX_POINT_MIN_INTERVAL_MS &&
        distance_m < WG_STORAGE_GPX_POINT_MIN_DISTANCE_M) {
      return true;
    }
  }

  char iso_time[32] = {0};
  if (!gpx_format_time_utc(gps->unix_time_s, iso_time, sizeof(iso_time))) {
    return false;
  }

  double lat = (double)gps->lat_e7 / 10000000.0;
  double lon = (double)gps->lon_e7 / 10000000.0;
  int n = fprintf(s_gpx_fp, "<trkpt lat=\"%.7f\" lon=\"%.7f\"><time>%s</time>", lat, lon, iso_time);
  if (n <= 0) {
    return false;
  }
  if ((gps->flags & WG_GPS_FLAG_HAS_ALT) != 0) {
    if (fprintf(s_gpx_fp, "<ele>%.1f</ele>", (double)gps->alt_mm / 1000.0) <= 0) {
      return false;
    }
  }
  if (fprintf(s_gpx_fp, "</trkpt>\n") <= 0) {
    return false;
  }

  s_gpx_has_last_point = true;
  s_gpx_last_lat_e7 = gps->lat_e7;
  s_gpx_last_lon_e7 = gps->lon_e7;
  s_gpx_last_point_ms = ms;
  if (s_gpx_points_written < UINT32_MAX) {
    s_gpx_points_written++;
  }

  bool should_commit = force_commit;
  if (!should_commit) {
    int64_t flush_age_ms = ms - s_gpx_last_flush_ms;
    should_commit = (s_gpx_last_flush_ms <= 0 || flush_age_ms >= WG_STORAGE_GPX_FLUSH_INTERVAL_MS ||
                     (s_gpx_points_written % 16U) == 0U);
  }
  if (should_commit) {
    if (!storage_commit_file(s_gpx_fp, "gpx append", s_gpx_session_id)) {
      return false;
    }
    s_gpx_last_flush_ms = ms;
  }
  return true;
}

static void storage_gpx_tick(void) {
  if (!s_storage_ready) {
    return;
  }
  if (!storage_lock(pdMS_TO_TICKS(50))) {
    return;
  }
  if (s_scanning && s_session_open && s_gpx_session_id == s_session_id && s_latest_gps.valid) {
    if (!storage_append_gpx_point_locked(&s_latest_gps, false)) {
      ESP_LOGW(TAG, "gpx tick append failed sid=%016llX errno=%d", (unsigned long long)s_session_id,
               errno);
    }
  }
  storage_unlock();
}

static uint32_t debug_seed_prng_next(uint32_t *state) {
  uint32_t x = *state;
  if (x == 0) {
    x = 0x1F123BB5U;
  }
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

static bool storage_seed_synthetic_locked(uint32_t target_bytes, uint32_t *records_added_out,
                                          uint32_t *bytes_added_out) {
  if (!s_storage_ready) {
    return false;
  }
  if (target_bytes == 0) {
    target_bytes = WG_DEBUG_SEED_DEFAULT_BYTES;
  }
  if (target_bytes < WG_DEBUG_SEED_MIN_BYTES) {
    target_bytes = WG_DEBUG_SEED_MIN_BYTES;
  }
  if (target_bytes > WG_DEBUG_SEED_MAX_BYTES) {
    target_bytes = WG_DEBUG_SEED_MAX_BYTES;
  }

  bool opened_here = false;
  if (!s_session_open) {
    if (!storage_open_session_locked()) {
      return false;
    }
    opened_here = true;
  }

  uint32_t seq = s_session_next_seq;
  if (seq == 0) {
    seq = 1;
  }
  uint32_t records_added = 0;
  uint32_t bytes_added = 0;
  uint32_t rng = (uint32_t)((uint64_t)now_ms() ^ s_session_id ^ (uint64_t)target_bytes);
  if (rng == 0) {
    rng = 0xA55A3C97U;
  }

  uint8_t record_bytes[96] = {0};
  while (bytes_added < target_bytes && seq < UINT32_MAX && records_added < 50000U) {
    wg_sighting_payload_t payload = {0};
    for (int i = 0; i < 6; ++i) {
      payload.bssid[i] = (uint8_t)(debug_seed_prng_next(&rng) >> 24);
    }
    payload.bssid[0] = (uint8_t)((payload.bssid[0] & 0xFEU) | 0x02U);  // locally-administered unicast
    payload.channel = (uint8_t)(1U + (seq % 13U));
    payload.rssi = (int8_t)(-30 - (int8_t)(seq % 55U));
    payload.auth_mode = WG_AUTH_WPA2_WPA3;
    payload.proto_flags = WG_SEC_PROTO_WPA2;
    payload.akm_flags = WG_SEC_AKM_PSK;
    payload.cipher_flags = WG_SEC_CIPHER_CCMP_128;
    char ssid[33] = {0};
    int ssid_len = snprintf(ssid, sizeof(ssid), "SIM_%08lX_%05lu",
                            (unsigned long)debug_seed_prng_next(&rng),
                            (unsigned long)(seq % 100000UL));
    if (ssid_len < 0) {
      ssid_len = 0;
    } else if (ssid_len > 32) {
      ssid_len = 32;
    }
    payload.ssid_len = (uint8_t)ssid_len;
    if (ssid_len > 0) {
      memcpy(payload.ssid, ssid, (size_t)ssid_len);
    }
    payload.flags = WG_SIGHTING_FLAG_NEW;
    payload.session_id = s_session_id;
    payload.record_seq = seq;
    payload.node_id = 0;
    payload.source_flags = WG_SIGHTING_SOURCE_LIVE;

    if (s_latest_gps.valid) {
      payload.gps_valid = 1;
      payload.gps_source = (uint8_t)s_gps_source;
      payload.gps_lat_e7 = s_latest_gps.lat_e7;
      payload.gps_lon_e7 = s_latest_gps.lon_e7;
      payload.gps_alt_mm = s_latest_gps.alt_mm;
      payload.gps_unix_time_s = s_latest_gps.unix_time_s;
      payload.gps_accuracy_cm = s_latest_gps.accuracy_cm;
    } else {
      payload.gps_valid = 1;
      payload.gps_source = WG_GPS_SRC_PHONE;
      payload.gps_lat_e7 = 377749000 + (int32_t)(seq % 8000U);
      payload.gps_lon_e7 = -1224194000 + (int32_t)(seq % 8000U);
      payload.gps_alt_mm = 12000;
      payload.gps_unix_time_s = (uint32_t)(now_ms() / 1000LL);
      payload.gps_accuracy_cm = 500;
    }

    const size_t record_len = wg_build_sighting_payload(&payload, record_bytes, sizeof(record_bytes));
    if (record_len == 0 || record_len > UINT16_MAX) {
      break;
    }
    if (!storage_enqueue_record_locked(record_bytes, (uint16_t)record_len, seq)) {
      break;
    }

    uint32_t written_with_hdr = WG_STORAGE_RECORD_HDR_SIZE + (uint32_t)record_len;
    if (bytes_added <= UINT32_MAX - written_with_hdr) {
      bytes_added += written_with_hdr;
    } else {
      bytes_added = UINT32_MAX;
    }
    records_added++;
    seq++;
  }

  if (!storage_flush_pending_locked(true)) {
    if (opened_here) {
      storage_close_session_locked(WG_SESSION_STATE_ABORTED);
    }
    return false;
  }

  if (opened_here) {
    storage_close_session_locked(WG_SESSION_STATE_CLOSED);
  } else {
    s_session_next_seq = seq;
    storage_refresh_backlog_locked(false);
  }

  if (records_added_out != NULL) {
    *records_added_out = records_added;
  }
  if (bytes_added_out != NULL) {
    *bytes_added_out = bytes_added;
  }
  return records_added > 0;
}

static bool storage_seed_synthetic(uint32_t target_bytes, uint32_t *records_added_out,
                                   uint32_t *bytes_added_out) {
  if (!storage_lock(pdMS_TO_TICKS(500))) {
    return false;
  }
  bool ok = storage_seed_synthetic_locked(target_bytes, records_added_out, bytes_added_out);
  storage_unlock();
  return ok;
}

static bool storage_open_session_locked(void) {
  if (!s_storage_ready) {
    return false;
  }
  if (s_session_open) {
    return true;
  }
  s_session_id = storage_generate_session_id();
  s_last_session_id = s_session_id;
  s_session_next_seq = 1;
  memset(&s_session_manifest, 0, sizeof(s_session_manifest));
  s_session_manifest.session_id = s_session_id;
  s_session_manifest.state = WG_SESSION_STATE_OPEN;
  s_session_manifest.start_unix_time_s = s_latest_gps.unix_time_s;
  s_session_manifest.start_hop_ms = s_hop_ms;
  s_session_manifest.start_channel_mask = s_channel_mask;
  s_session_manifest.queue_full = 0;
  if (!storage_write_manifest(&s_session_manifest)) {
    s_session_id = 0;
    return false;
  }
  storage_reset_pending_block_locked();
  storage_reset_gpx_state_locked(true, false);
  s_session_open = true;
  storage_refresh_backlog_locked(false);
  ESP_LOGI(TAG, "session open sid=%016llX", (unsigned long long)s_session_id);
  return true;
}

static void storage_close_session_locked(wg_session_state_t final_state) {
  if (!s_storage_ready || !s_session_open) {
    return;
  }
  if (s_latest_gps.valid && s_gpx_fp != NULL && s_gpx_session_id == s_session_id) {
    (void)storage_append_gpx_point_locked(&s_latest_gps, true);
  }
  (void)storage_flush_pending_locked(true);
  storage_close_gpx_session_locked(true);
  if (final_state < WG_SESSION_STATE_OPEN || final_state > WG_SESSION_STATE_ABORTED) {
    final_state = WG_SESSION_STATE_ABORTED;
  }
  s_session_manifest.state = (uint8_t)final_state;
  s_session_manifest.end_unix_time_s = s_latest_gps.unix_time_s;
  s_session_manifest.unclean_shutdown = (final_state == WG_SESSION_STATE_ABORTED) ? 1 : 0;
  s_session_manifest.queue_full = s_queue_full ? 1 : 0;
  (void)storage_write_manifest(&s_session_manifest);
  ESP_LOGI(TAG, "session close sid=%016llX state=%u written=%lu acked=%lu dropped=%lu",
           (unsigned long long)s_session_id, (unsigned)s_session_manifest.state,
           (unsigned long)s_session_manifest.records_written,
           (unsigned long)s_session_manifest.records_acked,
           (unsigned long)s_session_manifest.records_dropped);
  s_last_session_id = s_session_id;
  s_session_open = false;
  s_session_id = 0;
  s_session_next_seq = 1;
  memset(&s_session_manifest, 0, sizeof(s_session_manifest));
  storage_reset_pending_block_locked();
  storage_refresh_backlog_locked(true);
}

static bool storage_replay_load_next_block_locked(void) {
  if (!s_replay_active || s_replay_fp == NULL) {
    return false;
  }
  while (true) {
    size_t n = fread(s_replay_block, 1, sizeof(s_replay_block), s_replay_fp);
    if (n != sizeof(s_replay_block)) {
      return false;
    }
    const wg_store_block_header_t *hdr = (const wg_store_block_header_t *)s_replay_block;
    if (hdr->magic != WG_STORAGE_BLOCK_MAGIC || hdr->version != WG_STORAGE_VERSION ||
        hdr->header_size != sizeof(wg_store_block_header_t) || hdr->payload_len == 0 ||
        (size_t)hdr->payload_len > sizeof(s_replay_block) - sizeof(wg_store_block_header_t) ||
        hdr->record_count == 0) {
      continue;
    }
    if (hdr->session_id != s_replay_session_id) {
      continue;
    }
    const uint8_t *payload = s_replay_block + sizeof(wg_store_block_header_t);
    if (crc32_ieee(payload, hdr->payload_len) != hdr->crc32) {
      wg_session_manifest_t manifest = {0};
      if (storage_read_manifest(s_replay_session_id, &manifest) && manifest.crc_failures < UINT32_MAX) {
        manifest.crc_failures++;
        (void)storage_write_manifest(&manifest);
      }
      continue;
    }
    s_replay_block_record_count = hdr->record_count;
    s_replay_block_index = 0;
    s_replay_block_payload_len = hdr->payload_len;
    s_replay_block_payload_pos = 0;
    s_replay_block_first_seq = hdr->first_seq;
    return true;
  }
}

static void storage_clear_replay_pending_message_locked(void) {
  s_replay_pending_valid = false;
  s_replay_pending_msg_type = WG_MSG_SIGHTING;
  s_replay_pending_len = 0;
  s_replay_pending_session_id = 0;
  s_replay_pending_seq = 0;
}

static void storage_reset_replay_locked(void) {
  if (s_replay_fp != NULL) {
    fclose(s_replay_fp);
    s_replay_fp = NULL;
  }
  s_replay_active = false;
  s_replay_session_id = 0;
  s_replay_cursor_seq = 0;
  s_replay_acked_seq = 0;
  s_replay_block_record_count = 0;
  s_replay_block_index = 0;
  s_replay_block_payload_len = 0;
  s_replay_block_payload_pos = 0;
  s_replay_block_first_seq = 0;
  storage_clear_replay_pending_message_locked();
  s_replay_prefetch_valid = false;
  s_replay_prefetch_len = 0;
  s_replay_prefetch_session_id = 0;
  s_replay_prefetch_seq = 0;
}

static bool storage_select_next_replay_session_locked(uint64_t *session_id_out,
                                                      wg_session_manifest_t *manifest_out) {
  uint64_t best_sid = 0;
  wg_session_manifest_t best_manifest = {0};
  DIR *dir = opendir(storage_base_path());
  if (dir == NULL) {
    return false;
  }
  struct dirent *ent = NULL;
  while ((ent = readdir(dir)) != NULL) {
    uint64_t sid = 0;
    if (!storage_parse_meta_filename(ent->d_name, &sid)) {
      continue;
    }
    wg_session_manifest_t manifest = {0};
    if (!storage_read_manifest(sid, &manifest)) {
      continue;
    }
    if (manifest.records_written == 0 || manifest.records_acked >= manifest.records_written) {
      continue;
    }
    if (best_sid == 0 || sid < best_sid) {
      best_sid = sid;
      best_manifest = manifest;
    }
  }
  closedir(dir);
  if (best_sid == 0) {
    return false;
  }
  *session_id_out = best_sid;
  *manifest_out = best_manifest;
  return true;
}

static bool storage_replay_allowed_locked(void) {
  if (!s_storage_ready || !s_replay_requested || s_blob_requested) {
    return false;
  }
  if (s_wifi_dump_requested || s_wifi_dump_active) {
    return false;
  }
  if (s_scanning) {
    return false;
  }
  if (!storage_has_output_link()) {
    return false;
  }
  return true;
}

static void storage_maybe_start_replay_locked(void) {
  if (s_replay_active || !storage_replay_allowed_locked()) {
    return;
  }
  (void)storage_flush_pending_locked(true);
  uint64_t sid = 0;
  wg_session_manifest_t manifest = {0};
  if (!storage_select_next_replay_session_locked(&sid, &manifest)) {
    return;
  }
  char data_path[96] = {0};
  storage_session_paths(sid, NULL, 0, data_path, sizeof(data_path));
  FILE *fp = fopen(data_path, "rb");
  if (fp == NULL) {
    ESP_LOGW(TAG, "replay open failed sid=%016llX errno=%d", (unsigned long long)sid, errno);
    return;
  }
  storage_reset_replay_locked();
  s_replay_fp = fp;
  s_replay_active = true;
  s_replay_session_id = sid;
  s_replay_acked_seq = manifest.last_seq_acked;
  s_replay_cursor_seq = manifest.last_seq_acked;
  ESP_LOGI(TAG, "replay start sid=%016llX acked=%lu written=%lu", (unsigned long long)sid,
           (unsigned long)manifest.last_seq_acked, (unsigned long)manifest.last_seq_written);
}

static bool storage_replay_fetch_next_locked(uint8_t *out_payload, uint16_t *out_len,
                                             uint64_t *out_session_id, uint32_t *out_seq) {
  if (!s_replay_active || s_replay_fp == NULL || out_payload == NULL || out_len == NULL ||
      out_session_id == NULL || out_seq == NULL) {
    return false;
  }
  while (true) {
    if (s_replay_block_record_count == 0 || s_replay_block_index >= s_replay_block_record_count) {
      if (!storage_replay_load_next_block_locked()) {
        storage_reset_replay_locked();
        storage_maybe_start_replay_locked();
        if (!s_replay_active) {
          return false;
        }
        continue;
      }
    }

    while (s_replay_block_index < s_replay_block_record_count) {
      if ((size_t)s_replay_block_payload_pos + WG_STORAGE_RECORD_HDR_SIZE > s_replay_block_payload_len) {
        s_replay_block_index = s_replay_block_record_count;
        break;
      }
      const uint8_t *payload = s_replay_block + sizeof(wg_store_block_header_t);
      uint16_t record_len = (uint16_t)payload[s_replay_block_payload_pos] |
                            ((uint16_t)payload[s_replay_block_payload_pos + 1] << 8);
      s_replay_block_payload_pos += WG_STORAGE_RECORD_HDR_SIZE;
      if ((size_t)s_replay_block_payload_pos + record_len > s_replay_block_payload_len ||
          record_len > WG_HOST_FRAME_MAX_PAYLOAD) {
        s_replay_block_index = s_replay_block_record_count;
        break;
      }
      uint32_t seq = s_replay_block_first_seq + s_replay_block_index;
      const uint8_t *record_ptr = &payload[s_replay_block_payload_pos];
      s_replay_block_payload_pos = (uint16_t)((size_t)s_replay_block_payload_pos + record_len);
      s_replay_block_index++;
      if (seq <= s_replay_acked_seq) {
        continue;
      }
      memcpy(out_payload, record_ptr, record_len);
      *out_len = record_len;
      *out_session_id = s_replay_session_id;
      *out_seq = seq;
      s_replay_cursor_seq = seq;
      return true;
    }
  }
}

static bool storage_patch_sighting_source(uint8_t *payload, uint16_t payload_len, uint8_t source_flags) {
  if (payload == NULL || payload_len < 28) {
    return false;
  }
  uint8_t ssid_len = payload[12];
  if (ssid_len > 32) {
    return false;
  }
  size_t source_idx = (size_t)27 + ssid_len;
  if (source_idx >= payload_len) {
    return false;
  }
  payload[source_idx] = source_flags;
  return true;
}

static void wake_replay_task(void) {
  if (s_replay_task != NULL) {
    xTaskNotifyGive(s_replay_task);
  }
}

static bool storage_prepare_replay_single_pending_locked(void) {
  uint8_t payload[WG_HOST_FRAME_MAX_PAYLOAD] = {0};
  uint16_t payload_len = 0;
  uint64_t session_id = 0;
  uint32_t seq = 0;
  if (s_replay_prefetch_valid) {
    payload_len = s_replay_prefetch_len;
    session_id = s_replay_prefetch_session_id;
    seq = s_replay_prefetch_seq;
    memcpy(payload, s_replay_prefetch_payload, payload_len);
    s_replay_prefetch_valid = false;
    s_replay_prefetch_len = 0;
    s_replay_prefetch_session_id = 0;
    s_replay_prefetch_seq = 0;
  } else if (!storage_replay_fetch_next_locked(payload, &payload_len, &session_id, &seq)) {
    return false;
  }

  if (payload_len == 0 || payload_len > sizeof(s_replay_pending_payload)) {
    return false;
  }
  (void)storage_patch_sighting_source(payload, payload_len, WG_SIGHTING_SOURCE_REPLAY);
  memcpy(s_replay_pending_payload, payload, payload_len);
  s_replay_pending_msg_type = WG_MSG_SIGHTING;
  s_replay_pending_len = payload_len;
  s_replay_pending_session_id = session_id;
  s_replay_pending_seq = seq;
  s_replay_pending_valid = true;
  return true;
}

static bool storage_prepare_replay_batch_pending_locked(void) {
  uint8_t batch_payload[WG_HOST_FRAME_MAX_PAYLOAD] = {0};
  uint16_t max_batch_payload = ble_notify_frame_payload_limit();
  if (max_batch_payload > sizeof(batch_payload)) {
    max_batch_payload = sizeof(batch_payload);
  }
  uint16_t pos = WG_REPLAY_BATCH_HEADER_SIZE;
  uint8_t count = 0;
  uint64_t last_session_id = 0;
  uint32_t last_seq = 0;

  while (count < WG_REPLAY_BATCH_MAX_RECORDS) {
    uint8_t record_payload[WG_HOST_FRAME_MAX_PAYLOAD] = {0};
    uint16_t record_len = 0;
    uint64_t session_id = 0;
    uint32_t seq = 0;

    if (s_replay_prefetch_valid) {
      record_len = s_replay_prefetch_len;
      session_id = s_replay_prefetch_session_id;
      seq = s_replay_prefetch_seq;
      memcpy(record_payload, s_replay_prefetch_payload, record_len);
      s_replay_prefetch_valid = false;
      s_replay_prefetch_len = 0;
      s_replay_prefetch_session_id = 0;
      s_replay_prefetch_seq = 0;
    } else if (!storage_replay_fetch_next_locked(record_payload, &record_len, &session_id, &seq)) {
      break;
    }

    if (record_len == 0 || record_len > sizeof(record_payload)) {
      continue;
    }
    (void)storage_patch_sighting_source(record_payload, record_len, WG_SIGHTING_SOURCE_REPLAY);

    size_t needed = (size_t)WG_REPLAY_BATCH_RECORD_HDR_SIZE + record_len;
    if ((size_t)pos + needed > max_batch_payload) {
      if (count == 0) {
        memcpy(s_replay_pending_payload, record_payload, record_len);
        s_replay_pending_msg_type = WG_MSG_SIGHTING;
        s_replay_pending_len = record_len;
        s_replay_pending_session_id = session_id;
        s_replay_pending_seq = seq;
        s_replay_pending_valid = true;
        return true;
      }
      memcpy(s_replay_prefetch_payload, record_payload, record_len);
      s_replay_prefetch_len = record_len;
      s_replay_prefetch_session_id = session_id;
      s_replay_prefetch_seq = seq;
      s_replay_prefetch_valid = true;
      break;
    }

    batch_payload[pos + 0] = (uint8_t)(record_len & 0xFF);
    batch_payload[pos + 1] = (uint8_t)((record_len >> 8) & 0xFF);
    memcpy(&batch_payload[pos + WG_REPLAY_BATCH_RECORD_HDR_SIZE], record_payload, record_len);
    pos = (uint16_t)((size_t)pos + needed);
    count++;
    last_session_id = session_id;
    last_seq = seq;
  }

  if (count == 0) {
    return false;
  }

  batch_payload[0] = count;
  memcpy(s_replay_pending_payload, batch_payload, pos);
  s_replay_pending_msg_type = WG_MSG_REPLAY_BATCH;
  s_replay_pending_len = pos;
  s_replay_pending_session_id = last_session_id;
  s_replay_pending_seq = last_seq;
  s_replay_pending_valid = true;
  return true;
}

static bool storage_prepare_replay_pending_locked(void) {
  if (s_replay_pending_valid) {
    return true;
  }
  if (s_sighting_notify_enabled && !s_host_serial_active) {
    if (storage_prepare_replay_batch_pending_locked()) {
      return true;
    }
  }
  return storage_prepare_replay_single_pending_locked();
}

static bool storage_blob_allowed_locked(void) {
  if (!s_storage_ready || !s_blob_requested) {
    return false;
  }
  if (s_wifi_dump_requested || s_wifi_dump_active) {
    return false;
  }
  if (s_scanning) {
    return false;
  }
  if (!s_sighting_notify_enabled) {
    return false;
  }
  return true;
}

static void storage_clear_blob_pending_locked(void) {
  s_blob_pending_valid = false;
  s_blob_pending_msg_type = WG_MSG_BACKLOG_BLOB_META;
  s_blob_pending_len = 0;
  s_blob_pending_chunk_len = 0;
}

static bool storage_seek_blob_locked(uint32_t offset) {
  if (s_blob_fp == NULL) {
    return false;
  }
  if (offset > s_blob_file_bytes) {
    offset = s_blob_file_bytes;
  }
  if (fseek(s_blob_fp, (long)offset, SEEK_SET) != 0) {
    ESP_LOGW(TAG, "blob export seek failed sid=%016llX off=%lu errno=%d",
             (unsigned long long)s_blob_session_id, (unsigned long)offset, errno);
    return false;
  }
  s_blob_bytes_sent = offset;
  if (s_blob_bytes_acked > offset) {
    s_blob_bytes_acked = offset;
  }
  s_blob_done_sent = false;
  s_blob_waiting_ack = false;
  s_blob_last_progress_ms = now_ms();
  storage_clear_blob_pending_locked();
  return true;
}

static void storage_reset_blob_locked(void) {
  if (s_blob_fp != NULL) {
    fclose(s_blob_fp);
    s_blob_fp = NULL;
  }
  s_blob_active = false;
  s_blob_meta_sent = false;
  s_blob_done_sent = false;
  s_blob_waiting_ack = false;
  s_blob_session_id = 0;
  s_blob_file_bytes = 0;
  s_blob_bytes_sent = 0;
  s_blob_bytes_acked = 0;
  s_blob_written_seq = 0;
  s_blob_acked_seq = 0;
  s_blob_last_progress_ms = 0;
  storage_clear_blob_pending_locked();
}

static void storage_maybe_start_blob_locked(void) {
  if (s_blob_active || !storage_blob_allowed_locked()) {
    return;
  }
  (void)storage_flush_pending_locked(true);

  uint64_t sid = 0;
  wg_session_manifest_t manifest = {0};
  if (!storage_select_next_replay_session_locked(&sid, &manifest)) {
    return;
  }

  char data_path[96] = {0};
  storage_session_paths(sid, NULL, 0, data_path, sizeof(data_path));
  struct stat st = {0};
  if (stat(data_path, &st) != 0 || st.st_size <= 0 || st.st_size > UINT32_MAX) {
    ESP_LOGW(TAG, "blob export stat failed sid=%016llX errno=%d", (unsigned long long)sid, errno);
    return;
  }

  FILE *fp = fopen(data_path, "rb");
  if (fp == NULL) {
    ESP_LOGW(TAG, "blob export open failed sid=%016llX errno=%d", (unsigned long long)sid, errno);
    return;
  }

  storage_reset_blob_locked();
  s_blob_fp = fp;
  s_blob_active = true;
  s_blob_session_id = sid;
  s_blob_file_bytes = (uint32_t)st.st_size;
  s_blob_bytes_acked = 0;
  s_blob_written_seq = manifest.last_seq_written;
  s_blob_acked_seq = manifest.last_seq_acked;
  s_blob_last_progress_ms = now_ms();
  ESP_LOGI(TAG, "blob export start sid=%016llX bytes=%lu acked=%lu written=%lu",
           (unsigned long long)sid, (unsigned long)s_blob_file_bytes,
           (unsigned long)s_blob_acked_seq, (unsigned long)s_blob_written_seq);
}

static bool storage_prepare_blob_pending_locked(void) {
  if (s_blob_pending_valid) {
    return true;
  }
  if (!s_blob_active) {
    return false;
  }

  if (s_blob_waiting_ack) {
    if (s_blob_acked_seq >= s_blob_written_seq && s_blob_written_seq > 0) {
      ESP_LOGI(TAG, "blob export acked (mem) sid=%016llX seq=%lu",
               (unsigned long long)s_blob_session_id, (unsigned long)s_blob_acked_seq);
      storage_reset_blob_locked();
      storage_maybe_start_blob_locked();
      return false;
    }
    wg_session_manifest_t manifest = {0};
    if (storage_read_manifest(s_blob_session_id, &manifest)) {
      s_blob_acked_seq = manifest.last_seq_acked;
      if (manifest.last_seq_acked >= s_blob_written_seq) {
        ESP_LOGI(TAG, "blob export acked sid=%016llX seq=%lu",
                 (unsigned long long)s_blob_session_id, (unsigned long)manifest.last_seq_acked);
        storage_reset_blob_locked();
        storage_maybe_start_blob_locked();
      }
    }
    return false;
  }

  if (s_blob_bytes_sent > s_blob_bytes_acked) {
    const int64_t ms = now_ms();
    if (s_blob_last_progress_ms > 0 &&
        (ms - s_blob_last_progress_ms) >= WG_BLOB_CHUNK_ACK_TIMEOUT_MS) {
      ESP_LOGW(TAG, "blob export chunk ack timeout sid=%016llX sent=%lu acked=%lu; resuming at acked",
               (unsigned long long)s_blob_session_id, (unsigned long)s_blob_bytes_sent,
               (unsigned long)s_blob_bytes_acked);
      if (!storage_seek_blob_locked(s_blob_bytes_acked)) {
        storage_reset_blob_locked();
        return false;
      }
    }
    if ((s_blob_bytes_sent - s_blob_bytes_acked) >= WG_BLOB_CHUNK_WINDOW_BYTES) {
      // Sliding window: allow multiple chunks in flight for throughput, but
      // cap outstanding bytes to avoid overrunning the BLE TX queue.
      return false;
    }
  }

  if (!s_blob_meta_sent) {
    uint8_t payload[WG_BLOB_META_PAYLOAD_SIZE] = {0};
    wr_u64_le(&payload[0], s_blob_session_id);
    wr_u32_le(&payload[8], s_blob_file_bytes);
    wr_u32_le(&payload[12], s_blob_acked_seq);
    wr_u32_le(&payload[16], s_blob_written_seq);
    memcpy(s_blob_pending_payload, payload, sizeof(payload));
    s_blob_pending_msg_type = WG_MSG_BACKLOG_BLOB_META;
    s_blob_pending_len = sizeof(payload);
    s_blob_pending_chunk_len = 0;
    s_blob_pending_valid = true;
    return true;
  }

  if (s_blob_bytes_sent < s_blob_file_bytes) {
    uint16_t chunk_data_limit = ble_notify_frame_payload_limit();
    if (chunk_data_limit <= WG_BLOB_CHUNK_HEADER_SIZE) {
      return false;
    }
    chunk_data_limit = (uint16_t)(chunk_data_limit - WG_BLOB_CHUNK_HEADER_SIZE);
    if (chunk_data_limit > WG_BLOB_CHUNK_DATA_MAX_MAX) {
      chunk_data_limit = WG_BLOB_CHUNK_DATA_MAX_MAX;
    }
    uint32_t remaining = s_blob_file_bytes - s_blob_bytes_sent;
    uint16_t chunk_len =
        (remaining > chunk_data_limit) ? chunk_data_limit : (uint16_t)remaining;
    size_t n = fread(&s_blob_pending_payload[WG_BLOB_CHUNK_HEADER_SIZE], 1, chunk_len, s_blob_fp);
    if (n != chunk_len) {
      ESP_LOGW(TAG, "blob export read failed sid=%016llX sent=%lu wanted=%u got=%u",
               (unsigned long long)s_blob_session_id, (unsigned long)s_blob_bytes_sent,
               (unsigned)chunk_len, (unsigned)n);
      storage_reset_blob_locked();
      return false;
    }
    wr_u64_le(&s_blob_pending_payload[0], s_blob_session_id);
    wr_u32_le(&s_blob_pending_payload[8], s_blob_bytes_sent);
    wr_u16_le(&s_blob_pending_payload[12], chunk_len);
    s_blob_pending_msg_type = WG_MSG_BACKLOG_BLOB_CHUNK;
    s_blob_pending_len = (uint16_t)(WG_BLOB_CHUNK_HEADER_SIZE + chunk_len);
    s_blob_pending_chunk_len = chunk_len;
    s_blob_pending_valid = true;
    return true;
  }

  if (s_blob_bytes_acked < s_blob_file_bytes) {
    return false;
  }

  if (!s_blob_done_sent) {
    uint8_t payload[WG_BLOB_DONE_PAYLOAD_SIZE] = {0};
    wr_u64_le(&payload[0], s_blob_session_id);
    wr_u32_le(&payload[8], s_blob_file_bytes);
    wr_u32_le(&payload[12], s_blob_written_seq);
    memcpy(s_blob_pending_payload, payload, sizeof(payload));
    s_blob_pending_msg_type = WG_MSG_BACKLOG_BLOB_DONE;
    s_blob_pending_len = sizeof(payload);
    s_blob_pending_chunk_len = 0;
    s_blob_pending_valid = true;
    return true;
  }

  return false;
}

static bool storage_run_blob_tick(void) {
  bool sent_any = false;
  if (!s_storage_ready) {
    return false;
  }
  for (uint32_t budget = 0; budget < WG_STORAGE_BLOB_BUDGET_PER_TICK; ++budget) {
    if (!storage_lock(pdMS_TO_TICKS(20))) {
      return sent_any;
    }
    if (!storage_blob_allowed_locked()) {
      storage_reset_blob_locked();
      storage_unlock();
      break;
    }
    storage_maybe_start_blob_locked();
    if (!storage_prepare_blob_pending_locked()) {
      storage_unlock();
      break;
    }
    uint8_t msg_type = s_blob_pending_msg_type;
    uint16_t payload_len = s_blob_pending_len;
    uint16_t chunk_len = s_blob_pending_chunk_len;
    uint8_t payload[WG_BLE_NOTIFY_FRAME_MAX_PAYLOAD_MAX] = {0};
    if (payload_len > sizeof(payload)) {
      storage_clear_blob_pending_locked();
      storage_unlock();
      continue;
    }
    memcpy(payload, s_blob_pending_payload, payload_len);

    // Commit counters while still holding the lock so that an ACK arriving
    // during ble_notify_framed() sees the correct s_blob_bytes_sent value.
    // Without this, the NimBLE host task can pre-empt the replay task,
    // process the ACK with s_blob_bytes_sent still at 0, clamp ack_end to 0,
    // and silently drop the ACK — causing an infinite timeout loop.
    uint32_t prev_bytes_sent = s_blob_bytes_sent;
    if (msg_type == WG_MSG_BACKLOG_BLOB_META) {
      s_blob_meta_sent = true;
      s_blob_last_progress_ms = now_ms();
    } else if (msg_type == WG_MSG_BACKLOG_BLOB_CHUNK) {
      uint32_t next = s_blob_bytes_sent + (uint32_t)chunk_len;
      if (next < s_blob_bytes_sent || next > s_blob_file_bytes) {
        next = s_blob_file_bytes;
      }
      s_blob_bytes_sent = next;
      s_blob_last_progress_ms = now_ms();
    } else if (msg_type == WG_MSG_BACKLOG_BLOB_DONE) {
      s_blob_done_sent = true;
      s_blob_waiting_ack = true;
      s_blob_last_progress_ms = now_ms();
    }
    storage_clear_blob_pending_locked();
    storage_unlock();

    if (!ble_notify_framed(s_sighting_handle, msg_type, payload, payload_len)) {
      if (s_notify_drops % 50 == 1) {
        ESP_LOGW(TAG, "blob notify backpressure type=0x%02X len=%u drops=%lu rc=%d",
                 (unsigned)msg_type, (unsigned)payload_len,
                 (unsigned long)s_notify_drops, s_notify_drop_last_rc);
      }
      // Notification failed — roll back the counters we just committed.
      if (storage_lock(pdMS_TO_TICKS(20))) {
        if (msg_type == WG_MSG_BACKLOG_BLOB_CHUNK) {
          if (!storage_seek_blob_locked(prev_bytes_sent)) {
            storage_reset_blob_locked();
          }
        } else if (msg_type == WG_MSG_BACKLOG_BLOB_META) {
          s_blob_meta_sent = false;
        } else if (msg_type == WG_MSG_BACKLOG_BLOB_DONE) {
          s_blob_done_sent = false;
          s_blob_waiting_ack = false;
        }
        storage_unlock();
      }
      break;
    }
    sent_any = true;

    // After META or DONE, yield the budget loop so the BLE stack has time to
    // actually transmit the notification before we queue the next one.
    // Back-to-back META + CHUNK queuing causes the large CHUNK notification
    // (514 bytes, requiring LL fragmentation) to be silently lost in transit.
    if (msg_type == WG_MSG_BACKLOG_BLOB_META ||
        msg_type == WG_MSG_BACKLOG_BLOB_DONE) {
      break;
    }
  }
  return sent_any;
}

static bool storage_run_replay_tick(void) {
  bool sent_any = false;
  if (!s_storage_ready) {
    return false;
  }
  if (s_blob_requested) {
    return false;
  }
  for (uint32_t budget = 0; budget < WG_STORAGE_REPLAY_BUDGET_PER_TICK; ++budget) {
    if (!storage_lock(pdMS_TO_TICKS(20))) {
      return sent_any;
    }
    if (!storage_replay_allowed_locked()) {
      storage_reset_replay_locked();
      storage_unlock();
      break;
    }
    storage_maybe_start_replay_locked();
    if (!storage_prepare_replay_pending_locked()) {
      storage_unlock();
      break;
    }

    uint16_t payload_len = s_replay_pending_len;
    uint8_t msg_type = s_replay_pending_msg_type;
    bool send_ble = s_sighting_notify_enabled;
    bool send_host = s_host_serial_active;
    if (send_host && msg_type != WG_MSG_SIGHTING) {
      storage_clear_replay_pending_message_locked();
      storage_unlock();
      continue;
    }
    uint8_t payload[WG_HOST_FRAME_MAX_PAYLOAD] = {0};
    if (payload_len > 0 && payload_len <= sizeof(payload)) {
      memcpy(payload, s_replay_pending_payload, payload_len);
    }
    storage_unlock();

    if (payload_len == 0 || payload_len > sizeof(payload)) {
      if (!storage_lock(pdMS_TO_TICKS(20))) {
        return sent_any;
      }
      storage_clear_replay_pending_message_locked();
      storage_unlock();
      continue;
    }

    bool sent = false;
    if (send_ble) {
      sent = ble_notify_framed(s_sighting_handle, msg_type, payload, payload_len) || sent;
    }
    if (send_host) {
      sent = host_serial_notify_framed(msg_type, payload, payload_len) || sent;
    }
    if (!sent) {
      break;
    }
    sent_any = true;
    if (!storage_lock(pdMS_TO_TICKS(20))) {
      return sent_any;
    }
    storage_clear_replay_pending_message_locked();
    storage_unlock();
  }
  return sent_any;
}

static void storage_set_replay_enabled(bool enabled) {
  if (!storage_lock(pdMS_TO_TICKS(80))) {
    return;
  }
  if (enabled) {
    s_blob_requested = false;
    storage_reset_blob_locked();
  }
  s_replay_requested = enabled;
  if (!enabled) {
    storage_reset_replay_locked();
    storage_unlock();
    return;
  }
  if (!storage_replay_allowed_locked()) {
    storage_reset_replay_locked();
    storage_unlock();
    return;
  }
  storage_maybe_start_replay_locked();
  storage_unlock();
  wake_replay_task();
}

static void storage_set_blob_enabled(bool enabled) {
  if (!storage_lock(pdMS_TO_TICKS(80))) {
    return;
  }
  if (enabled) {
    s_replay_requested = false;
    storage_reset_replay_locked();
  }
  s_blob_requested = enabled;
  if (!enabled) {
    storage_reset_blob_locked();
    storage_unlock();
    return;
  }
  if (!storage_blob_allowed_locked()) {
    storage_reset_blob_locked();
    storage_unlock();
    return;
  }
  storage_maybe_start_blob_locked();
  storage_unlock();
  wake_replay_task();
}

static bool storage_clear_sessions(void) {
  if (!s_storage_ready) {
    return false;
  }
  if (!storage_lock(pdMS_TO_TICKS(250))) {
    return false;
  }

  (void)storage_flush_pending_locked(true);
  s_replay_requested = false;
  storage_reset_replay_locked();
  s_blob_requested = false;
  storage_reset_blob_locked();

  if (s_session_open) {
    storage_close_session_locked(WG_SESSION_STATE_ABORTED);
  }

  DIR *dir = opendir(storage_base_path());
  if (dir == NULL) {
    storage_refresh_backlog_locked(false);
    storage_unlock();
    ESP_LOGW(TAG, "storage clear failed: open dir errno=%d", errno);
    return false;
  }

  bool ok = true;
  uint32_t removed = 0;
  const size_t prefix_len = strlen(WG_STORAGE_FILE_PREFIX);
  struct dirent *ent = NULL;
  while ((ent = readdir(dir)) != NULL) {
    if (strncmp(ent->d_name, WG_STORAGE_FILE_PREFIX, prefix_len) != 0) {
      continue;
    }
    size_t name_len = strlen(ent->d_name);
    bool is_meta = (name_len > 5 && strcmp(ent->d_name + name_len - 5, ".meta") == 0) ||
                   (name_len > 7 && strcmp(ent->d_name + name_len - 7, ".meta.b") == 0);
    bool is_data = name_len > 4 && strcmp(ent->d_name + name_len - 4, ".dat") == 0;
    bool is_gpx = name_len > 4 && strcmp(ent->d_name + name_len - 4, ".gpx") == 0;
    bool is_gpx_tmp = name_len > 8 && strcmp(ent->d_name + name_len - 8, ".gpx.tmp") == 0;
    if (!is_meta && !is_data && !is_gpx && !is_gpx_tmp) {
      continue;
    }
    char path[WG_STORAGE_PATH_BUF_SIZE] = {0};
    int path_len = snprintf(path, sizeof(path), "%s/%s", storage_base_path(), ent->d_name);
    if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
      ok = false;
      ESP_LOGW(TAG, "storage clear path too long for '%s'", ent->d_name);
      continue;
    }
    if (unlink(path) == 0) {
      removed++;
    } else if (errno != ENOENT) {
      ok = false;
      ESP_LOGW(TAG, "storage clear unlink failed path=%s errno=%d", path, errno);
    }
  }
  closedir(dir);

  s_session_open = false;
  s_session_id = 0;
  s_last_session_id = 0;
  s_session_next_seq = 1;
  memset(&s_session_manifest, 0, sizeof(s_session_manifest));
  storage_reset_pending_block_locked();
  s_queue_full = false;
  s_dropped_flash_full = 0;
  storage_refresh_backlog_locked(false);
  storage_unlock();

  if (ok) {
    ESP_LOGI(TAG, "storage clear complete removed=%lu", (unsigned long)removed);
  } else {
    ESP_LOGW(TAG, "storage clear completed with errors removed=%lu", (unsigned long)removed);
  }
  return ok;
}

static void storage_refresh_backlog_locked(bool reclaim) {
  if (!s_storage_ready) {
    s_queue_backlog_records = 0;
    s_queue_backlog_bytes = 0;
    return;
  }
  if (reclaim) {
    (void)storage_reclaim_acked_sessions_locked();
  }
  uint64_t records_total = 0;
  uint64_t bytes_total = 0;
  DIR *dir = opendir(storage_base_path());
  if (dir == NULL) {
    s_queue_backlog_records = 0;
    s_queue_backlog_bytes = 0;
    return;
  }
  struct dirent *ent = NULL;
  while ((ent = readdir(dir)) != NULL) {
    uint64_t sid = 0;
    if (!storage_parse_meta_filename(ent->d_name, &sid)) {
      continue;
    }
    wg_session_manifest_t manifest = {0};
    if (!storage_read_manifest(sid, &manifest)) {
      continue;
    }
    if (manifest.records_written == 0 || manifest.records_acked >= manifest.records_written) {
      continue;
    }
    uint32_t unacked = manifest.records_written - manifest.records_acked;
    records_total += unacked;
    char data_path[96] = {0};
    storage_session_paths(sid, NULL, 0, data_path, sizeof(data_path));
    struct stat st = {0};
    if (stat(data_path, &st) == 0 && st.st_size > 0 && manifest.records_written > 0) {
      uint64_t file_bytes = (uint64_t)st.st_size;
      uint64_t est = (file_bytes * unacked + manifest.records_written - 1U) / manifest.records_written;
      bytes_total += est;
    }
  }
  closedir(dir);
  if (s_session_open && s_store_block_record_count > 0) {
    records_total += s_store_block_record_count;
    bytes_total += s_store_block_payload_len;
  }
  s_queue_backlog_records = (records_total > UINT32_MAX) ? UINT32_MAX : (uint32_t)records_total;
  s_queue_backlog_bytes = (bytes_total > UINT32_MAX) ? UINT32_MAX : (uint32_t)bytes_total;
}

static void storage_refresh_backlog(bool reclaim) {
  if (!storage_lock(pdMS_TO_TICKS(100))) {
    return;
  }
  storage_refresh_backlog_locked(reclaim);
  storage_unlock();
}

static bool storage_apply_replay_ack(uint64_t session_id, uint32_t highest_seq) {
  if (!s_storage_ready || session_id == 0 || highest_seq == 0) {
    return false;
  }
  if (!storage_lock(pdMS_TO_TICKS(120))) {
    return false;
  }
  wg_session_manifest_t manifest = {0};
  bool use_current = s_session_open && session_id == s_session_id;
  bool loaded = use_current ? true : storage_read_manifest(session_id, &manifest);
  if (use_current) {
    manifest = s_session_manifest;
  }
  if (!loaded) {
    storage_unlock();
    return false;
  }
  if (highest_seq > manifest.last_seq_written) {
    highest_seq = manifest.last_seq_written;
  }
  if (highest_seq <= manifest.last_seq_acked) {
    if (s_replay_pending_valid && s_replay_pending_session_id == session_id &&
        manifest.last_seq_acked >= s_replay_pending_seq) {
      storage_clear_replay_pending_message_locked();
    }
    if (s_replay_prefetch_valid && s_replay_prefetch_session_id == session_id &&
        manifest.last_seq_acked >= s_replay_prefetch_seq) {
      s_replay_prefetch_valid = false;
      s_replay_prefetch_len = 0;
      s_replay_prefetch_session_id = 0;
      s_replay_prefetch_seq = 0;
    }
    if (s_blob_active && s_blob_session_id == session_id &&
        manifest.last_seq_acked > s_blob_acked_seq) {
      s_blob_acked_seq = manifest.last_seq_acked;
    }
    storage_unlock();
    return true;
  }
  manifest.last_seq_acked = highest_seq;
  manifest.records_acked = highest_seq;
  if (manifest.records_acked > manifest.records_written) {
    manifest.records_acked = manifest.records_written;
  }
  if (!storage_write_manifest(&manifest)) {
    if (use_current) {
      s_session_manifest = manifest;
    }
    bool transient_applied = false;
    if (s_replay_active && s_replay_session_id == session_id && highest_seq > s_replay_acked_seq) {
      s_replay_acked_seq = highest_seq;
      transient_applied = true;
    }
    if (s_blob_active && s_blob_session_id == session_id && highest_seq > s_blob_acked_seq) {
      s_blob_acked_seq = highest_seq;
      transient_applied = true;
    }
    if (s_replay_pending_valid && s_replay_pending_session_id == session_id &&
        highest_seq >= s_replay_pending_seq) {
      storage_clear_replay_pending_message_locked();
    }
    if (s_replay_prefetch_valid && s_replay_prefetch_session_id == session_id &&
        highest_seq >= s_replay_prefetch_seq) {
      s_replay_prefetch_valid = false;
      s_replay_prefetch_len = 0;
      s_replay_prefetch_session_id = 0;
      s_replay_prefetch_seq = 0;
    }
    if (transient_applied) {
      ESP_LOGW(TAG, "ack persisted in RAM only sid=%016llX seq=%lu", (unsigned long long)session_id,
               (unsigned long)highest_seq);
      storage_unlock();
      return true;
    }
    storage_unlock();
    return false;
  }
  if (use_current) {
    s_session_manifest = manifest;
  }
  if (s_replay_active && s_replay_session_id == session_id && highest_seq > s_replay_acked_seq) {
    s_replay_acked_seq = highest_seq;
  }
  if (s_blob_active && s_blob_session_id == session_id && highest_seq > s_blob_acked_seq) {
    s_blob_acked_seq = highest_seq;
  }
  if (s_replay_pending_valid && s_replay_pending_session_id == session_id &&
      highest_seq >= s_replay_pending_seq) {
    storage_clear_replay_pending_message_locked();
  }
  if (s_replay_prefetch_valid && s_replay_prefetch_session_id == session_id &&
      highest_seq >= s_replay_prefetch_seq) {
    s_replay_prefetch_valid = false;
    s_replay_prefetch_len = 0;
    s_replay_prefetch_session_id = 0;
    s_replay_prefetch_seq = 0;
  }
  storage_refresh_backlog_locked(manifest.records_acked >= manifest.records_written);
  storage_unlock();
  return true;
}

static bool storage_apply_blob_chunk_reply(uint64_t session_id, uint32_t offset, uint16_t chunk_len,
                                           uint8_t reply_code) {
  if (session_id == 0) {
    return false;
  }
  if (reply_code != WG_BACKLOG_BLOB_CHUNK_REPLY_ACK &&
      reply_code != WG_BACKLOG_BLOB_CHUNK_REPLY_NAK) {
    return false;
  }
  if (!s_storage_ready) {
    return false;
  }
  if (!storage_lock(pdMS_TO_TICKS(60))) {
    return false;
  }

  bool ok = true;
  bool changed = false;
  if (!s_blob_active || !s_blob_requested || s_blob_session_id != session_id) {
    ESP_LOGW(TAG,
             "blob chunk reply ignored sid=%016llX off=%lu len=%u reply=%u active=%d req=%d current_sid=%016llX",
             (unsigned long long)session_id, (unsigned long)offset, (unsigned)chunk_len,
             (unsigned)reply_code, s_blob_active ? 1 : 0, s_blob_requested ? 1 : 0,
             (unsigned long long)s_blob_session_id);
    storage_unlock();
    return true;
  }

  if (reply_code == WG_BACKLOG_BLOB_CHUNK_REPLY_ACK) {
    if (chunk_len == 0) {
      ok = false;
    } else {
      uint64_t ack_end64 = (uint64_t)offset + (uint64_t)chunk_len;
      uint32_t ack_end = (ack_end64 > UINT32_MAX) ? UINT32_MAX : (uint32_t)ack_end64;
      if (ack_end > s_blob_file_bytes) {
        ack_end = s_blob_file_bytes;
      }
      if (offset > s_blob_bytes_sent) {
        ESP_LOGW(TAG, "blob chunk ack reject sid=%016llX off=%lu > sent=%lu",
                 (unsigned long long)session_id, (unsigned long)offset,
                 (unsigned long)s_blob_bytes_sent);
        ok = false;
      } else {
        if (ack_end > s_blob_bytes_sent) {
          ack_end = s_blob_bytes_sent;
        }
        if (offset > s_blob_bytes_acked) {
          if (!storage_seek_blob_locked(s_blob_bytes_acked)) {
            ok = false;
          } else {
            changed = true;
          }
        } else if (ack_end > s_blob_bytes_acked) {
          s_blob_bytes_acked = ack_end;
          changed = true;
          ESP_LOGI(TAG, "blob chunk ack sid=%016llX acked=%lu/%lu",
                   (unsigned long long)session_id, (unsigned long)s_blob_bytes_acked,
                   (unsigned long)s_blob_file_bytes);
        }
      }
    }
  } else {
    if (!storage_seek_blob_locked(offset)) {
      ok = false;
    } else {
      changed = true;
      ESP_LOGW(TAG, "blob chunk nak sid=%016llX resume_off=%lu",
               (unsigned long long)session_id, (unsigned long)offset);
    }
  }

  if (changed) {
    s_blob_last_progress_ms = now_ms();
  }
  storage_unlock();
  if (changed) {
    wake_replay_task();
  }
  return ok;
}

static bool storage_open_session(void) {
  bool ok = false;
  if (!storage_lock(pdMS_TO_TICKS(120))) {
    return false;
  }
  ok = storage_open_session_locked();
  storage_unlock();
  return ok;
}

static void storage_close_session(wg_session_state_t final_state) {
  if (!storage_lock(pdMS_TO_TICKS(200))) {
    return;
  }
  storage_close_session_locked(final_state);
  storage_unlock();
}

static bool storage_flush_pending(bool force) {
  bool ok = false;
  if (!storage_lock(pdMS_TO_TICKS(80))) {
    return false;
  }
  ok = storage_flush_pending_locked(force);
  storage_unlock();
  return ok;
}

static void storage_abort_stale_open_sessions(void) {
  DIR *dir = opendir(storage_base_path());
  if (dir == NULL) {
    return;
  }
  uint32_t aborted = 0;
  struct dirent *ent = NULL;
  while ((ent = readdir(dir)) != NULL) {
    uint64_t sid = 0;
    if (!storage_parse_meta_filename(ent->d_name, &sid)) {
      continue;
    }
    wg_session_manifest_t manifest = {0};
    if (!storage_read_manifest(sid, &manifest)) {
      continue;
    }
    if (manifest.state != WG_SESSION_STATE_OPEN) {
      continue;
    }
    manifest.state = WG_SESSION_STATE_ABORTED;
    manifest.unclean_shutdown = 1;
    if (storage_write_manifest(&manifest)) {
      (void)storage_finalize_stale_gpx_session(sid);
      aborted++;
    }
  }
  closedir(dir);
  if (aborted > 0) {
    ESP_LOGW(TAG, "storage marked %u stale sessions as aborted", (unsigned)aborted);
  }
}

static bool storage_mount_sd(void) {
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  spi_bus_config_t bus_cfg = {
      .mosi_io_num = WG_SD_SPI_MOSI_GPIO,
      .miso_io_num = WG_SD_SPI_MISO_GPIO,
      .sclk_io_num = WG_SD_SPI_SCK_GPIO,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 4096,
  };

  esp_err_t rc = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
  if (rc != ESP_OK) {
    ESP_LOGW(TAG, "storage sd spi bus init failed rc=%s", esp_err_to_name(rc));
    return false;
  }

  sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_cfg.host_id = host.slot;
  slot_cfg.gpio_cs = WG_SD_SPI_CS_GPIO;

  esp_vfs_fat_mount_config_t mount_cfg = {
      .format_if_mount_failed = false,
      .max_files = 8,
      .allocation_unit_size = 16 * 1024,
      .disk_status_check_enable = false,
      .use_one_fat = false,
  };

  sdmmc_card_t *card = NULL;
  rc = esp_vfs_fat_sdspi_mount(WG_STORAGE_SD_BASE_PATH, &host, &slot_cfg, &mount_cfg, &card);
  if (rc != ESP_OK) {
    ESP_LOGW(TAG, "storage sd mount failed rc=%s", esp_err_to_name(rc));
    (void)spi_bus_free(host.slot);
    return false;
  }

  s_storage_sd_card = card;
  s_storage_backend = WG_STORAGE_BACKEND_SD;
  return true;
}

static bool storage_mount_spiffs(void) {
  esp_vfs_spiffs_conf_t conf = {
      .base_path = WG_STORAGE_SPIFFS_BASE_PATH,
      .partition_label = "spiffs",
      .max_files = 8,
      .format_if_mount_failed = true,
  };
  esp_err_t rc = esp_vfs_spiffs_register(&conf);
  if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "storage spiffs mount failed rc=%s", esp_err_to_name(rc));
    return false;
  }
  s_storage_backend = WG_STORAGE_BACKEND_SPIFFS;
  return true;
}

static void init_storage(void) {
  s_store_mutex = xSemaphoreCreateMutex();
  if (s_store_mutex == NULL) {
    ESP_LOGW(TAG, "storage disabled: mutex allocation failed");
    return;
  }
  s_storage_backend = WG_STORAGE_BACKEND_NONE;
  s_storage_sd_card = NULL;

  bool mounted = storage_mount_sd();
  if (!mounted) {
    ESP_LOGW(TAG, "storage fallback: using SPIFFS backend");
    mounted = storage_mount_spiffs();
  }
  if (!mounted) {
    ESP_LOGW(TAG, "storage disabled: no backend available");
    return;
  }

  s_storage_ready = true;
  storage_abort_stale_open_sessions();
  storage_recover_wifi_dump_journal();
  storage_refresh_backlog(true);
  uint64_t total = 0;
  uint64_t used = 0;
  if (storage_get_fs_info(&total, &used)) {
    uint64_t free = (total > used) ? (total - used) : 0;
    ESP_LOGI(TAG, "storage ready backend=%s base=%s total=%llu used=%llu free=%llu",
             storage_backend_name(s_storage_backend), storage_base_path(),
             (unsigned long long)total, (unsigned long long)used, (unsigned long long)free);
  } else {
    ESP_LOGI(TAG, "storage ready backend=%s base=%s",
             storage_backend_name(s_storage_backend), storage_base_path());
  }
}

static void init_die_temp_sensor(void) {
#if SOC_TEMP_SENSOR_SUPPORTED
  temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
  temperature_sensor_handle_t handle = NULL;
  esp_err_t rc = temperature_sensor_install(&cfg, &handle);
  if (rc != ESP_OK) {
    ESP_LOGW(TAG, "die temp install failed rc=%s", esp_err_to_name(rc));
    s_die_temp_handle = NULL;
    s_die_temp_valid = false;
    s_die_temp_centi = INT16_MIN;
    return;
  }
  rc = temperature_sensor_enable(handle);
  if (rc != ESP_OK) {
    ESP_LOGW(TAG, "die temp enable failed rc=%s", esp_err_to_name(rc));
    (void)temperature_sensor_uninstall(handle);
    s_die_temp_handle = NULL;
    s_die_temp_valid = false;
    s_die_temp_centi = INT16_MIN;
    return;
  }
  s_die_temp_handle = handle;
  s_die_temp_valid = false;
  s_die_temp_centi = INT16_MIN;
  ESP_LOGI(TAG, "die temp sensor ready");
#endif
}

static void update_die_temp_sample(void) {
#if SOC_TEMP_SENSOR_SUPPORTED
  if (s_die_temp_handle == NULL) {
    return;
  }
  float celsius = 0.0f;
  esp_err_t rc = temperature_sensor_get_celsius(s_die_temp_handle, &celsius);
  if (rc != ESP_OK || !isfinite(celsius)) {
    s_die_temp_valid = false;
    s_die_temp_centi = INT16_MIN;
    return;
  }
  double bounded = (double)celsius;
  if (bounded > 327.67) {
    bounded = 327.67;
  } else if (bounded < -327.67) {
    bounded = -327.67;
  }
  long centi = lround(bounded * 100.0);
  if (centi > INT16_MAX) {
    centi = INT16_MAX;
  } else if (centi < (long)INT16_MIN + 1L) {
    centi = (long)INT16_MIN + 1L;
  }
  s_die_temp_centi = (int16_t)centi;
  s_die_temp_valid = true;
#endif
}

static void split_channel_mask(uint16_t global_mask, uint16_t *local_mask, uint16_t *node_mask) {
  uint16_t local = 0;
  uint16_t node = 0;
  for (uint8_t ch = 1; ch <= 13; ++ch) {
    uint16_t bit = (uint16_t)(1U << (ch - 1));
    if ((global_mask & bit) == 0) {
      continue;
    }
    if ((ch & 1U) != 0) {
      local |= bit;
    } else {
      node |= bit;
    }
  }
  if (local == 0) {
    local = global_mask;
  }
  *local_mask = local;
  *node_mask = node;
}

static bool node_channel_mask_24_is_valid(uint16_t mask) {
  return (mask & (uint16_t)~0x1FFFU) == 0U;
}

static bool node_channel_mask_5ghz_is_valid(uint64_t mask) {
  return (mask & ~WG_NODE_5GHZ_MASK_ALL) == 0ULL;
}

static bool node_channel_plan_enabled(uint16_t mask24, uint64_t mask5ghz) {
  return mask24 != 0U || mask5ghz != 0ULL;
}

static void apply_channel_plan(uint16_t local_mask, uint16_t node_mask_24, uint64_t node_mask_5ghz) {
  s_local_channel_mask = local_mask;
  s_node_channel_mask = node_mask_24;
  s_node_channel_mask_5ghz = node_mask_5ghz;
  // Keep legacy field aligned with the master's 2.4 GHz mask.
  s_channel_mask = local_mask;
}

static const char *gps_source_name(wg_gps_source_t source) {
  switch (source) {
    case WG_GPS_SRC_UART:
      return "uart";
    case WG_GPS_SRC_PHONE:
      return "phone";
    case WG_GPS_SRC_NONE:
    default:
      return "none";
  }
}

static bool gps_nav_mode_is_valid(uint8_t mode) {
  return mode == WG_GPS_NAV_MODE_AUTO || mode == WG_GPS_NAV_MODE_1HZ ||
         mode == WG_GPS_NAV_MODE_2HZ || mode == WG_GPS_NAV_MODE_4HZ;
}

static const char *gps_nav_mode_name(uint8_t mode) {
  switch (mode) {
    case WG_GPS_NAV_MODE_1HZ:
      return "force-1hz";
    case WG_GPS_NAV_MODE_2HZ:
      return "force-2hz";
    case WG_GPS_NAV_MODE_4HZ:
      return "force-4hz";
    case WG_GPS_NAV_MODE_AUTO:
    default:
      return "auto";
  }
}

static const char *node_type_name(uint8_t type) {
  switch (type) {
    case WG_NODE_MSG_HELLO:
      return "HELLO";
    case WG_NODE_MSG_HELLO_ACK:
      return "HELLO_ACK";
    case WG_NODE_MSG_CONFIG:
      return "CONFIG";
    case WG_NODE_MSG_PING:
      return "PING";
    case WG_NODE_MSG_PONG:
      return "PONG";
    case WG_NODE_MSG_STATUS:
      return "STATUS";
    case WG_NODE_MSG_SIGHTING:
      return "SIGHTING";
    default:
      return "UNKNOWN";
  }
}

static void log_bridge_diagnostics(bool force) {
  const int64_t ms = now_ms();
  if (!force && s_diag_last_log_ms > 0 && (ms - s_diag_last_log_ms) < 5000) {
    return;
  }
  s_diag_last_log_ms = ms;

  const int64_t gps_rx_age_ms = (s_gps_last_rx_ms > 0) ? (ms - s_gps_last_rx_ms) : -1;
  const int64_t gps_fix_age_ms = (s_latest_gps.received_ms > 0) ? (ms - s_latest_gps.received_ms) : -1;
  const int64_t gps_rate_age_ms =
      (s_gps_last_rate_change_ms > 0) ? (ms - s_gps_last_rate_change_ms) : -1;
  ESP_LOGI(TAG,
           "diag gps src=%s valid=%d baud=%d nav_hz=%u nav_mode=%s uart_rx_bytes=%lu rx_age_ms=%lld "
           "fix_age_ms=%lld rate_age_ms=%lld "
           "lines=%lu gga=%lu rmc=%lu gsa=%lu gsv=%lu other=%lu fix_updates=%lu "
           "unique_est=%lu lat=%ld lon=%ld acc_cm=%u sats=%u/%u hdop=%.2f vdop=%.2f pdop=%.2f "
           "gga_rej_latlon=%lu gga_rej_fix=%lu rmc_rej_status=%lu rmc_rej_latlon=%lu "
           "last_fixq=%d last_sats_use=%d last_sats_view=%d last_rmc=%c",
           gps_source_name(s_gps_source), s_latest_gps.valid ? 1 : 0, s_gps_uart_baud_active,
           (unsigned)s_gps_nav_rate_hz, gps_nav_mode_name(s_gps_nav_mode),
           (unsigned long)s_gps_uart_rx_bytes,
           (long long)gps_rx_age_ms, (long long)gps_fix_age_ms, (long long)gps_rate_age_ms,
           (unsigned long)s_gps_nmea_lines_total,
           (unsigned long)s_gps_nmea_gga, (unsigned long)s_gps_nmea_rmc,
           (unsigned long)s_gps_nmea_gsa, (unsigned long)s_gps_nmea_gsv,
           (unsigned long)s_gps_nmea_other,
           (unsigned long)s_gps_fix_updates, (unsigned long)s_unique_bssid_estimate,
           (long)s_latest_gps.lat_e7,
           (long)s_latest_gps.lon_e7, s_latest_gps.accuracy_cm, (unsigned)s_latest_gps.sat_in_use,
           (unsigned)s_latest_gps.sat_in_view, (double)s_latest_gps.hdop_centi / 100.0,
           (double)s_latest_gps.vdop_centi / 100.0, (double)s_latest_gps.pdop_centi / 100.0,
           (unsigned long)s_gps_gga_reject_latlon, (unsigned long)s_gps_gga_reject_fix,
           (unsigned long)s_gps_rmc_reject_status, (unsigned long)s_gps_rmc_reject_latlon,
           s_gps_last_fix_quality, s_gps_last_sats, s_gps_last_sats_in_view, s_gps_last_rmc_status);

  const int64_t node_rx_age_ms =
      (s_node_status.last_rx_ms > 0) ? (ms - s_node_status.last_rx_ms) : -1;
  ESP_LOGI(TAG,
           "diag node link=%d hello=%d report=%d mask24=0x%04X mask5=0x%011llX rx_bytes=%lu rx_frames=%lu "
           "crc_fail=%u rx_last=%s(%u) age_ms=%lld tx_frames=%lu tx_fail=%lu tx_last=%s(%u)",
           s_node_status.link_up ? 1 : 0, s_node_seen_hello ? 1 : 0,
           s_node_report_enable ? 1 : 0, s_node_channel_mask,
           (unsigned long long)s_node_channel_mask_5ghz, (unsigned long)s_node_uart_rx_bytes,
           (unsigned long)s_node_rx_frames, s_node_rx_crc_failures,
           node_type_name(s_node_last_rx_type), (unsigned)s_node_last_rx_payload_len,
           (long long)node_rx_age_ms, (unsigned long)s_node_tx_frames,
           (unsigned long)s_node_tx_failures, node_type_name(s_node_last_tx_type),
           (unsigned)s_node_last_tx_payload_len);

  ESP_LOGI(TAG,
           "diag host enabled=%d pref=%s tx=%s avail(uart=%d usb=%d) active=%d frame_only=%d "
           "rx_frames=%lu tx_frames=%lu rx_errors=%lu",
           s_host_serial_enabled ? 1 : 0, host_backend_name(s_host_backend),
           host_backend_name(s_host_tx_backend), s_host_uart_enabled ? 1 : 0, s_host_usb_enabled ? 1 : 0,
           s_host_serial_active ? 1 : 0,
           s_host_frame_only_logs ? 1 : 0, (unsigned long)s_host_rx_frames,
           (unsigned long)s_host_tx_frames, (unsigned long)s_host_rx_errors);
  ESP_LOGI(TAG,
           "diag ble drops=%u rc_last=%d enomem=%lu ebusy=%lu estalled=%lu att=%lu hci=%lu l2c=%lu "
           "other=%lu link itvl=%u(%.2fms) mtu=%u phy=%s/%s dle tx=%u/%uus rx=%u/%uus",
           (unsigned)s_notify_drops, s_notify_drop_last_rc,
           (unsigned long)s_notify_drop_by_hs_rc[BLE_HS_ENOMEM],
           (unsigned long)s_notify_drop_by_hs_rc[BLE_HS_EBUSY],
           (unsigned long)s_notify_drop_by_hs_rc[BLE_HS_ESTALLED],
           (unsigned long)s_notify_drop_att, (unsigned long)s_notify_drop_hci,
           (unsigned long)s_notify_drop_l2c, (unsigned long)s_notify_drop_other,
           (unsigned)s_ble_conn_itvl_1p25ms, (double)s_ble_conn_itvl_1p25ms * 1.25,
           (unsigned)s_ble_att_mtu, ble_phy_name(s_ble_tx_phy), ble_phy_name(s_ble_rx_phy),
           (unsigned)s_ble_max_tx_octets, (unsigned)s_ble_max_tx_time_us,
           (unsigned)s_ble_max_rx_octets, (unsigned)s_ble_max_rx_time_us);

  ESP_LOGI(TAG,
           "diag store ready=%d backend=%s open=%d sid=%016llX backlog_records=%lu backlog_bytes=%lu "
           "replay_req=%d replay=%d replay_sid=%016llX replay_cursor=%lu "
           "blob_req=%d blob=%d blob_sid=%016llX blob_bytes_ack=%lu blob_bytes_tx=%lu/%lu "
           "queue_full=%d dropped_full=%lu gpx_sid=%016llX gpx_points=%lu gpx_active=%d",
           s_storage_ready ? 1 : 0, storage_backend_name(s_storage_backend),
           s_session_open ? 1 : 0, (unsigned long long)s_session_id,
           (unsigned long)s_queue_backlog_records, (unsigned long)s_queue_backlog_bytes,
           s_replay_requested ? 1 : 0, s_replay_active ? 1 : 0,
           (unsigned long long)s_replay_session_id,
           (unsigned long)s_replay_cursor_seq,
           s_blob_requested ? 1 : 0, s_blob_active ? 1 : 0,
           (unsigned long long)s_blob_session_id,
           (unsigned long)s_blob_bytes_acked, (unsigned long)s_blob_bytes_sent,
           (unsigned long)s_blob_file_bytes,
           s_queue_full ? 1 : 0,
           (unsigned long)s_dropped_flash_full, (unsigned long long)s_gpx_session_id,
           (unsigned long)s_gpx_points_written, s_gpx_fp != NULL ? 1 : 0);

  if (!s_node_seen_hello && s_node_uart_rx_bytes == 0 && s_node_tx_frames >= 20 &&
      !s_node_no_rx_warned) {
    s_node_no_rx_warned = true;
    ESP_LOGW(TAG,
             "node RX is silent while TX is active. Check C6 GPIO%d->node RX "
             "(ESP8266 GPIO3 or BW16 PB2), C6 GPIO%d<-node TX (ESP8266 GPIO1 or BW16 PB1), "
             "common GND, and UART baud=%d on both sides.",
             WG_NODE_UART_TX_GPIO, WG_NODE_UART_RX_GPIO, WG_NODE_UART_BAUD);
  }
}

static uint16_t current_node_age_s(void) {
  if (!s_node_status.link_up || s_node_status.last_rx_ms <= 0) {
    return 0;
  }
  int64_t elapsed_ms = now_ms() - s_node_status.last_rx_ms;
  if (elapsed_ms <= 0) {
    return 0;
  }
  return clamp_u16_u32((uint32_t)(elapsed_ms / 1000));
}

static uint16_t current_gps_age_s(void) {
  if (!s_latest_gps.valid || s_latest_gps.received_ms <= 0) {
    return 0;
  }
  const int64_t elapsed_ms = now_ms() - s_latest_gps.received_ms;
  if (elapsed_ms <= 0) {
    return 0;
  }
  return clamp_u16_u32((uint32_t)(elapsed_ms / 1000));
}

static uint16_t current_gps_accuracy_dm(void) {
  if (!s_latest_gps.valid) {
    return 0;
  }
  return clamp_u16_u32((uint32_t)((s_latest_gps.accuracy_cm + 5U) / 10U));
}

static void build_status_payload(wg_status_payload_t *payload) {
  unique_bssid_refresh_estimate();
  uint64_t storage_total = 0;
  uint64_t storage_used = 0;
  if (!storage_get_fs_info(&storage_total, &storage_used)) {
    storage_total = 0;
    storage_used = 0;
  }
  uint32_t spiffs_total_u32 = (storage_total > UINT32_MAX) ? UINT32_MAX : (uint32_t)storage_total;
  uint32_t spiffs_used_u32 = (storage_used > UINT32_MAX) ? UINT32_MAX : (uint32_t)storage_used;
  uint32_t spiffs_free_u32 =
      (spiffs_total_u32 > spiffs_used_u32) ? (spiffs_total_u32 - spiffs_used_u32) : 0;
  *payload = (wg_status_payload_t){
      .scanning = s_scanning,
      .ble_encrypted = s_ble_encrypted,
      .current_channel = s_current_channel,
      .hop_ms = s_hop_ms,
      .channel_mask = s_local_channel_mask,
      .local_channel_mask = s_local_channel_mask,
      .node_channel_mask_24 = s_node_channel_mask,
      .node_channel_mask_5ghz = s_node_channel_mask_5ghz,
      .unique_bssids_estimate = s_unique_bssid_estimate,
      .packets_per_sec = s_packets_per_sec,
      .dropped_notifies = s_notify_drops,
      .boot_mode = s_boot_mode,
      .gps_valid = s_latest_gps.valid,
      .gps_age_s = current_gps_age_s(),
      .gps_accuracy_dm = current_gps_accuracy_dm(),
      .node_link_up = s_node_status.link_up,
      .node_last_seen_s = current_node_age_s(),
      .node_packets_per_sec = s_node_status.packets_per_sec,
      .node_forwarded_sightings = s_node_status.forwarded_sightings,
      .node_channel = s_node_status.channel,
      .node_channel_mask = s_node_channel_mask,
      .session_open = s_session_open,
      .session_id = s_session_open ? s_session_id : s_last_session_id,
      .queued_records = s_queue_backlog_records,
      .queued_bytes = s_queue_backlog_bytes,
      .replay_active = s_replay_active || s_blob_active,
      .replay_cursor = s_blob_active ? s_blob_bytes_acked : s_replay_cursor_seq,
      .queue_full = s_queue_full,
      .dropped_flash_full = s_dropped_flash_full,
      .node_count = 2,
      .gps_nav_applied_hz = s_gps_nav_rate_hz,
      .spiffs_total_bytes = spiffs_total_u32,
      .spiffs_used_bytes = spiffs_used_u32,
      .spiffs_free_bytes = spiffs_free_u32,
      .storage_total_bytes = storage_total,
      .storage_used_bytes = storage_used,
      .storage_free_bytes = (storage_total >= storage_used) ? (storage_total - storage_used) : 0,
      .blob_active = s_blob_active,
      .blob_session_id = s_blob_session_id,
      .blob_bytes_sent = s_blob_bytes_acked,
      .blob_bytes_total = s_blob_file_bytes,
#if SOC_TEMP_SENSOR_SUPPORTED
      .die_temp_centi = s_die_temp_valid ? s_die_temp_centi : INT16_MIN,
#else
      .die_temp_centi = INT16_MIN,
#endif
  };
}

static bool gps_fix_is_fresh(const gps_fix_t *fix, uint32_t max_age_ms) {
  if (!fix->valid || fix->received_ms <= 0) {
    return false;
  }
  int64_t age_ms = now_ms() - fix->received_ms;
  return age_ms >= 0 && age_ms <= (int64_t)max_age_ms;
}

static void apply_effective_gps_fix(const gps_fix_t *src, wg_gps_source_t source) {
  wg_gps_source_t prev_source = s_gps_source;
  bool prev_valid = s_latest_gps.valid;
  bool changed = false;
  if (src == NULL) {
    changed = s_latest_gps.valid || s_gps_source != WG_GPS_SRC_NONE;
    memset(&s_latest_gps, 0, sizeof(s_latest_gps));
    s_gps_source = WG_GPS_SRC_NONE;
  } else {
    changed = !s_latest_gps.valid || s_gps_source != source ||
              s_latest_gps.lat_e7 != src->lat_e7 || s_latest_gps.lon_e7 != src->lon_e7 ||
              s_latest_gps.accuracy_cm != src->accuracy_cm ||
              s_latest_gps.sat_in_use != src->sat_in_use ||
              s_latest_gps.sat_in_view != src->sat_in_view ||
              s_latest_gps.sat_count != src->sat_count ||
              s_latest_gps.hdop_centi != src->hdop_centi ||
              s_latest_gps.pdop_centi != src->pdop_centi ||
              s_latest_gps.vdop_centi != src->vdop_centi;
    s_latest_gps = *src;
    s_gps_source = source;
  }
  if (changed) {
    if (s_latest_gps.valid) {
      ESP_LOGI(TAG,
               "GPS selected src=%s lat=%ld lon=%ld acc_cm=%u sats=%u/%u hdop=%.2f vdop=%.2f pdop=%.2f age_ms=%lld",
               gps_source_name(s_gps_source), (long)s_latest_gps.lat_e7, (long)s_latest_gps.lon_e7,
               s_latest_gps.accuracy_cm, (unsigned)s_latest_gps.sat_in_use,
               (unsigned)s_latest_gps.sat_in_view, (double)s_latest_gps.hdop_centi / 100.0,
               (double)s_latest_gps.vdop_centi / 100.0, (double)s_latest_gps.pdop_centi / 100.0,
               (long long)(now_ms() - s_latest_gps.received_ms));
    } else if (prev_valid || prev_source != WG_GPS_SRC_NONE) {
      ESP_LOGW(TAG, "GPS lost (prev_src=%s)", gps_source_name(prev_source));
    }
    led_sync_scan_gps_state();
    notify_status_frame();
    notify_gps_frame();
    if (wg_scan_policy_should_auto_start(s_boot_mode, s_auto_scan_paused, s_scanning, prev_valid,
                                         s_latest_gps.valid)) {
      esp_err_t rc = start_scan();
      if (rc == ESP_OK) {
        ESP_LOGI(TAG, "Boot auto: scan started after GPS fix became valid");
      } else {
        ESP_LOGW(TAG, "Boot auto: failed to start scan after GPS fix (%s)", esp_err_to_name(rc));
      }
    }
  }
}

static void recompute_effective_gps_fix(void) {
  bool uart_fresh = gps_fix_is_fresh(&s_uart_gps, WG_GPS_MAX_AGE_MS);
  bool phone_fresh = gps_fix_is_fresh(&s_phone_gps, WG_GPS_PHONE_MAX_AGE_MS);

  if (uart_fresh &&
      (s_uart_gps.accuracy_cm <= WG_GPS_UART_ACCURACY_FALLBACK_CM || !phone_fresh)) {
    apply_effective_gps_fix(&s_uart_gps, WG_GPS_SRC_UART);
    return;
  }
  if (phone_fresh) {
    apply_effective_gps_fix(&s_phone_gps, WG_GPS_SRC_PHONE);
    return;
  }
  if (uart_fresh) {
    apply_effective_gps_fix(&s_uart_gps, WG_GPS_SRC_UART);
    return;
  }
  apply_effective_gps_fix(NULL, WG_GPS_SRC_NONE);
}

static bool parse_nmea_latlon(const char *value, const char *hemi, int32_t *out_e7) {
  if (value == NULL || hemi == NULL || out_e7 == NULL || value[0] == '\0' || hemi[0] == '\0') {
    return false;
  }
  char *end = NULL;
  double raw = strtod(value, &end);
  if (end == value || raw <= 0.0) {
    return false;
  }
  int degrees = (int)(raw / 100.0);
  double minutes = raw - (double)degrees * 100.0;
  double dec = (double)degrees + minutes / 60.0;
  if (hemi[0] == 'S' || hemi[0] == 'W') {
    dec = -dec;
  } else if (hemi[0] != 'N' && hemi[0] != 'E') {
    return false;
  }
  *out_e7 = (int32_t)llround(dec * 10000000.0);
  return true;
}

static uint16_t parse_nmea_dop_centi(const char *token) {
  if (token == NULL || token[0] == '\0') {
    return 0;
  }
  char *end = NULL;
  double dop = strtod(token, &end);
  if (end == token || !isfinite(dop) || dop <= 0.0) {
    return 0;
  }
  if (dop > 999.99) {
    dop = 999.99;
  }
  return clamp_u16_u32((uint32_t)llround(dop * 100.0));
}

static bool nmea_sentence_has_type(const char *line, const char *type3) {
  if (line == NULL || type3 == NULL) {
    return false;
  }
  size_t len = strnlen(line, 16);
  if (len < 6) {
    return false;
  }
  return line[0] == '$' && strncmp(&line[3], type3, 3) == 0;
}

static uint16_t nmea_sentence_talker_key(const char *line) {
  if (line == NULL) {
    return 0;
  }
  size_t len = strnlen(line, 8);
  if (len < 3 || line[0] != '$') {
    return 0;
  }
  return (((uint16_t)(uint8_t)line[1]) << 8) | (uint16_t)(uint8_t)line[2];
}

static bool talker_key_is_gn(uint16_t key) {
  return key == ((((uint16_t)'G') << 8) | (uint16_t)'N');
}

static void gps_update_talker_sat(gps_talker_sat_t *buckets, size_t bucket_count, uint16_t talker_key,
                                  uint8_t sat_count, int64_t now) {
  if (buckets == NULL || bucket_count == 0 || talker_key == 0 || now <= 0) {
    return;
  }
  size_t slot = bucket_count;
  size_t oldest_slot = 0;
  int64_t oldest_ms = INT64_MAX;
  for (size_t i = 0; i < bucket_count; ++i) {
    if (buckets[i].key == talker_key || buckets[i].key == 0) {
      slot = i;
      break;
    }
    if (buckets[i].updated_ms < oldest_ms) {
      oldest_ms = buckets[i].updated_ms;
      oldest_slot = i;
    }
  }
  if (slot >= bucket_count) {
    slot = oldest_slot;
  }
  buckets[slot].key = talker_key;
  buckets[slot].sat_count = sat_count;
  buckets[slot].updated_ms = now;
}

// gn_filter: -1 = any, 0 = non-GN only, 1 = GN only.
static uint8_t gps_sum_recent_talker_sat(const gps_talker_sat_t *buckets, size_t bucket_count,
                                         int64_t now, int64_t fresh_ms, int gn_filter,
                                         bool *has_any) {
  bool any = false;
  uint32_t total = 0;
  if (buckets != NULL && now > 0 && fresh_ms > 0) {
    for (size_t i = 0; i < bucket_count; ++i) {
      if (buckets[i].key == 0 || buckets[i].sat_count == 0 || buckets[i].updated_ms <= 0) {
        continue;
      }
      int64_t age = now - buckets[i].updated_ms;
      if (age < 0 || age > fresh_ms) {
        continue;
      }
      bool is_gn = talker_key_is_gn(buckets[i].key);
      if ((gn_filter == 0 && is_gn) || (gn_filter == 1 && !is_gn)) {
        continue;
      }
      any = true;
      total += buckets[i].sat_count;
    }
  }
  if (total > UINT8_MAX) {
    total = UINT8_MAX;
  }
  if (has_any != NULL) {
    *has_any = any;
  }
  return (uint8_t)total;
}

static bool gps_send_ubx(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t payload_len) {
  if (payload_len > 64) {
    return false;
  }
  uint8_t frame[8 + 64] = {0};
  size_t pos = 0;
  frame[pos++] = 0xB5;
  frame[pos++] = 0x62;
  frame[pos++] = cls;
  frame[pos++] = id;
  frame[pos++] = (uint8_t)(payload_len & 0xFF);
  frame[pos++] = (uint8_t)((payload_len >> 8) & 0xFF);
  if (payload_len > 0 && payload != NULL) {
    memcpy(&frame[pos], payload, payload_len);
    pos += payload_len;
  }
  uint8_t ck_a = 0;
  uint8_t ck_b = 0;
  for (size_t i = 2; i < pos; ++i) {
    ck_a = (uint8_t)(ck_a + frame[i]);
    ck_b = (uint8_t)(ck_b + ck_a);
  }
  frame[pos++] = ck_a;
  frame[pos++] = ck_b;

  int written = uart_write_bytes(WG_GPS_UART_NUM, (const char *)frame, pos);
  if (written != (int)pos) {
    return false;
  }
  (void)uart_wait_tx_done(WG_GPS_UART_NUM, pdMS_TO_TICKS(120));
  return true;
}

static bool gps_send_cfg_prt_uart1_baud(uint32_t baud) {
  uint8_t payload[20] = {
      0x01, 0x00, 0x00, 0x00,  // portID=UART1
      0xD0, 0x08, 0x00, 0x00,  // mode=8N1
      (uint8_t)(baud & 0xFF), (uint8_t)((baud >> 8) & 0xFF), (uint8_t)((baud >> 16) & 0xFF),
      (uint8_t)((baud >> 24) & 0xFF),
      0x07, 0x00, 0x03, 0x00,  // inProtoMask UBX|NMEA|RTCM, outProtoMask UBX|NMEA
      0x00, 0x00, 0x00, 0x00,  // flags/reserved
  };
  return gps_send_ubx(0x06, 0x00, payload, sizeof(payload));
}

static bool gps_send_cfg_rate_hz(uint8_t nav_rate_hz) {
  uint16_t meas_rate_ms = 1000;
  if (nav_rate_hz == 2) {
    meas_rate_ms = 500;
  } else if (nav_rate_hz >= 4) {
    nav_rate_hz = 4;
    meas_rate_ms = 250;
  } else {
    nav_rate_hz = 1;
  }
  uint8_t payload[6] = {
      (uint8_t)(meas_rate_ms & 0xFF), (uint8_t)((meas_rate_ms >> 8) & 0xFF),
      0x01, 0x00,  // navRate (cycles)
      0x01, 0x00,  // timeRef = GPS time
  };
  return gps_send_ubx(0x06, 0x08, payload, sizeof(payload));
}

static bool gps_send_cfg_msg_nmea(uint8_t nmea_msg_id, uint8_t uart1_rate) {
  uint8_t payload[8] = {
      0xF0, nmea_msg_id,  // NMEA class + msg id
      0x00, uart1_rate,   // I2C rate, UART1 rate
      0x00, 0x00,         // UART2 rate, USB rate
      0x00, 0x00,         // SPI rate, reserved
  };
  return gps_send_ubx(0x06, 0x01, payload, sizeof(payload));
}

static bool gps_send_cfg_valset_u1(uint32_t key, uint8_t value) {
  uint8_t payload[9] = {0};
  payload[0] = 0x00;                  // version
  payload[1] = WG_UBX_CFG_LAYER_RAM;  // layers (RAM)
  payload[2] = 0x00;                  // transaction
  payload[3] = 0x00;                  // reserved
  wr_u32_le(&payload[4], key);
  payload[8] = value;
  return gps_send_ubx(0x06, 0x8A, payload, sizeof(payload));
}

static bool gps_apply_m9_profile(void) {
  bool ok = true;
  ok = gps_send_cfg_valset_u1(WG_UBX_CFG_KEY_PM_OPERATEMODE, WG_UBX_CFG_PM_OPERATEMODE_FULL) &&
       ok;
  vTaskDelay(pdMS_TO_TICKS(20));
  ok = gps_send_cfg_valset_u1(WG_UBX_CFG_KEY_NMEA_PROTVER, WG_UBX_CFG_NMEA_PROTVER_41) && ok;
  vTaskDelay(pdMS_TO_TICKS(20));
  ok = gps_send_cfg_valset_u1(WG_UBX_CFG_KEY_SIGNAL_GPS_ENA, 1) && ok;
  vTaskDelay(pdMS_TO_TICKS(20));
  ok = gps_send_cfg_valset_u1(WG_UBX_CFG_KEY_SIGNAL_GLO_ENA, 1) && ok;
  vTaskDelay(pdMS_TO_TICKS(20));
  ok = gps_send_cfg_valset_u1(WG_UBX_CFG_KEY_SIGNAL_GAL_ENA, 1) && ok;
  vTaskDelay(pdMS_TO_TICKS(20));
  ok = gps_send_cfg_valset_u1(WG_UBX_CFG_KEY_SIGNAL_BDS_ENA, 1) && ok;
  vTaskDelay(pdMS_TO_TICKS(20));
  ok = gps_send_cfg_valset_u1(WG_UBX_CFG_KEY_SIGNAL_QZSS_ENA, 1) && ok;
  vTaskDelay(pdMS_TO_TICKS(20));
  ok = gps_send_cfg_valset_u1(WG_UBX_CFG_KEY_SIGNAL_SBAS_ENA, 1) && ok;
  return ok;
}

static bool gps_apply_nav_profile(uint8_t nav_rate_hz, bool include_static_msgs) {
  uint8_t clamped = 1;
  if (nav_rate_hz >= 4) {
    clamped = 4;
  } else if (nav_rate_hz >= 2) {
    clamped = 2;
  }
  bool ok = true;
  ok = gps_send_cfg_rate_hz(clamped) && ok;
  vTaskDelay(pdMS_TO_TICKS(30));
  ok = gps_send_cfg_msg_nmea(0x02, clamped) && ok;  // GSA
  vTaskDelay(pdMS_TO_TICKS(20));
  ok = gps_send_cfg_msg_nmea(0x03, clamped) && ok;  // GSV
  if (include_static_msgs) {
    vTaskDelay(pdMS_TO_TICKS(20));
    ok = gps_send_cfg_msg_nmea(0x00, 1) && ok;  // GGA
    vTaskDelay(pdMS_TO_TICKS(20));
    ok = gps_send_cfg_msg_nmea(0x04, 1) && ok;  // RMC
    vTaskDelay(pdMS_TO_TICKS(20));
    ok = gps_send_cfg_msg_nmea(0x01, 0) && ok;  // GLL off
    vTaskDelay(pdMS_TO_TICKS(20));
    ok = gps_send_cfg_msg_nmea(0x05, 0) && ok;  // VTG off
  }
  return ok;
}

static bool gps_detect_nmea_at_baud(int baud, uint32_t timeout_ms) {
  if (baud <= 0) {
    return false;
  }
  if (uart_set_baudrate(WG_GPS_UART_NUM, baud) != ESP_OK) {
    return false;
  }
  (void)uart_flush_input(WG_GPS_UART_NUM);
  int64_t start_ms = now_ms();
  bool saw_dollar = false;
  uint8_t buf[64];
  while ((now_ms() - start_ms) < (int64_t)timeout_ms) {
    int n = uart_read_bytes(WG_GPS_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(80));
    if (n <= 0) {
      continue;
    }
    for (int i = 0; i < n; ++i) {
      if (buf[i] == '$') {
        saw_dollar = true;
      }
      if ((buf[i] == '\n' || buf[i] == '\r') && saw_dollar) {
        return true;
      }
    }
  }
  return false;
}

static int gps_probe_baud(void) {
  static const int k_probe_bauds[] = {WG_GPS_UART_BAUD, WG_GPS_UART_TARGET_BAUD, 115200};
  for (size_t i = 0; i < sizeof(k_probe_bauds) / sizeof(k_probe_bauds[0]); ++i) {
    if (gps_detect_nmea_at_baud(k_probe_bauds[i], 1200)) {
      return k_probe_bauds[i];
    }
  }
  return WG_GPS_UART_BAUD;
}

static uint8_t gps_choose_nav_rate_hz(uint8_t current_hz, const gps_fix_t *fix) {
  if (fix == NULL || !fix->valid || (fix->flags & WG_GPS_FLAG_HAS_SPEED) == 0) {
    return 1;
  }
  uint32_t speed = fix->speed_mmps;
  switch (current_hz) {
    case 4:
      return (speed < WG_GPS_RATE_SPEED_4HZ_DOWN_MMPS) ? 2 : 4;
    case 2:
      if (speed >= WG_GPS_RATE_SPEED_4HZ_UP_MMPS) {
        return 4;
      }
      return (speed < WG_GPS_RATE_SPEED_2HZ_DOWN_MMPS) ? 1 : 2;
    default:
      if (speed >= WG_GPS_RATE_SPEED_4HZ_UP_MMPS) {
        return 4;
      }
      return (speed >= WG_GPS_RATE_SPEED_2HZ_UP_MMPS) ? 2 : 1;
  }
}

static uint8_t gps_nav_mode_forced_hz(uint8_t nav_mode) {
  switch (nav_mode) {
    case WG_GPS_NAV_MODE_1HZ:
      return 1;
    case WG_GPS_NAV_MODE_2HZ:
      return 2;
    case WG_GPS_NAV_MODE_4HZ:
      return 4;
    default:
      return 0;
  }
}

static uint8_t gps_target_nav_rate_hz(uint8_t current_hz, const gps_fix_t *fix, uint8_t nav_mode) {
  uint8_t forced_hz = gps_nav_mode_forced_hz(nav_mode);
  if (forced_hz > 0) {
    return forced_hz;
  }
  return gps_choose_nav_rate_hz(current_hz, fix);
}

static void parse_nmea_gga(char *line) {
  char *save = NULL;
  char *token = strtok_r(line, ",", &save);
  int idx = 0;
  const char *lat = NULL;
  const char *lat_h = NULL;
  const char *lon = NULL;
  const char *lon_h = NULL;
  int fix_quality = 0;
  int sats = 0;
  double hdop = 99.0;
  double alt_m = 0.0;

  while (token != NULL) {
    switch (idx) {
      case 2:
        lat = token;
        break;
      case 3:
        lat_h = token;
        break;
      case 4:
        lon = token;
        break;
      case 5:
        lon_h = token;
        break;
      case 6:
        fix_quality = atoi(token);
        break;
      case 7:
        sats = atoi(token);
        break;
      case 8:
        hdop = strtod(token, NULL);
        break;
      case 9:
        alt_m = strtod(token, NULL);
        break;
      default:
        break;
    }
    token = strtok_r(NULL, ",", &save);
    idx++;
  }

  int32_t lat_e7 = 0;
  int32_t lon_e7 = 0;
  s_gps_last_fix_quality = fix_quality;
  if (!parse_nmea_latlon(lat, lat_h, &lat_e7) || !parse_nmea_latlon(lon, lon_h, &lon_e7)) {
    s_gps_gga_reject_latlon++;
    return;
  }
  if (fix_quality <= 0 || sats < 3) {
    s_gps_gga_reject_fix++;
    s_uart_gps.valid = false;
    s_uart_gps.flags &= (uint8_t)~(WG_GPS_FLAG_VALID | WG_GPS_FLAG_HAS_ALT);
    recompute_effective_gps_fix();
    return;
  }

  s_uart_gps.valid = true;
  s_uart_gps.flags &= (uint8_t)~(WG_GPS_FLAG_VALID | WG_GPS_FLAG_HAS_ALT);
  s_uart_gps.flags |= (WG_GPS_FLAG_VALID | WG_GPS_FLAG_HAS_ALT);
  s_uart_gps.lat_e7 = lat_e7;
  s_uart_gps.lon_e7 = lon_e7;
  s_uart_gps.alt_mm = (int32_t)llround(alt_m * 1000.0);
  uint32_t sat_u32 = sats > 0 ? (uint32_t)sats : 0U;
  if (sat_u32 > UINT8_MAX) {
    sat_u32 = UINT8_MAX;
  }
  uint8_t sat_fallback = (uint8_t)sat_u32;
  if (hdop < 0.3) {
    hdop = 0.3;
  }
  s_uart_gps.accuracy_cm = (uint16_t)clamp_u16_u32((uint32_t)llround(hdop * 500.0));
  int64_t now = now_ms();
  bool gsa_fresh =
      s_gps_last_gsa_ms > 0 && (now - s_gps_last_gsa_ms) >= 0 &&
      (now - s_gps_last_gsa_ms) <= WG_GPS_GSV_FRESH_MS;
  if (!gsa_fresh || s_uart_gps.sat_in_use == 0) {
    s_uart_gps.sat_in_use = sat_fallback;
  }
  bool gsv_fresh =
      s_gps_last_gsv_ms > 0 && (now - s_gps_last_gsv_ms) >= 0 &&
      (now - s_gps_last_gsv_ms) <= WG_GPS_GSV_FRESH_MS;
  if (!gsv_fresh || s_uart_gps.sat_in_view == 0) {
    s_uart_gps.sat_in_view = sat_fallback;
  }
  s_uart_gps.sat_count = s_uart_gps.sat_in_use;
  s_gps_last_sats = s_uart_gps.sat_in_use;
  s_gps_last_sats_in_view = s_uart_gps.sat_in_view;
  s_uart_gps.hdop_centi = clamp_u16_u32((uint32_t)llround(hdop * 100.0));
  s_uart_gps.received_ms = now;
  s_gps_fix_updates++;
  recompute_effective_gps_fix();
}

static void parse_nmea_gsa(char *line) {
  const uint16_t talker_key = nmea_sentence_talker_key(line);
  char *save = NULL;
  char *token = strtok_r(line, ",", &save);
  int idx = 0;
  int sats_in_use = 0;
  uint16_t pdop_centi = 0;
  uint16_t hdop_centi = 0;
  uint16_t vdop_centi = 0;

  while (token != NULL) {
    switch (idx) {
      case 3:
      case 4:
      case 5:
      case 6:
      case 7:
      case 8:
      case 9:
      case 10:
      case 11:
      case 12:
      case 13:
      case 14:
        if (token[0] != '\0') {
          sats_in_use++;
        }
        break;
      case 15:
        pdop_centi = parse_nmea_dop_centi(token);
        break;
      case 16:
        hdop_centi = parse_nmea_dop_centi(token);
        break;
      case 17:
        vdop_centi = parse_nmea_dop_centi(token);
        break;
      default:
        break;
    }
    token = strtok_r(NULL, ",", &save);
    idx++;
  }

  if (hdop_centi > 0) {
    s_uart_gps.hdop_centi = hdop_centi;
  }
  if (pdop_centi > 0) {
    s_uart_gps.pdop_centi = pdop_centi;
  }
  if (vdop_centi > 0) {
    s_uart_gps.vdop_centi = vdop_centi;
  }

  if (sats_in_use > 0) {
    uint32_t sat_u32 = (uint32_t)sats_in_use;
    if (sat_u32 > UINT8_MAX) {
      sat_u32 = UINT8_MAX;
    }
    int64_t now = now_ms();
    gps_update_talker_sat(s_gps_gsa_sat_by_talker, WG_GPS_TALKER_BUCKETS, talker_key,
                          (uint8_t)sat_u32, now);
    bool have_non_gn = false;
    bool have_gn = false;
    uint8_t sats_non_gn = gps_sum_recent_talker_sat(
        s_gps_gsa_sat_by_talker, WG_GPS_TALKER_BUCKETS, now, WG_GPS_GSV_FRESH_MS, 0, &have_non_gn);
    uint8_t sats_gn = gps_sum_recent_talker_sat(s_gps_gsa_sat_by_talker, WG_GPS_TALKER_BUCKETS,
                                                now, WG_GPS_GSV_FRESH_MS, 1, &have_gn);
    s_uart_gps.sat_in_use = have_non_gn ? sats_non_gn : (have_gn ? sats_gn : (uint8_t)sat_u32);
    s_uart_gps.sat_count = s_uart_gps.sat_in_use;
    s_gps_last_sats = s_uart_gps.sat_in_use;
    s_gps_last_gsa_ms = now;
  }
}

static void parse_nmea_gsv(char *line) {
  const uint16_t talker_key = nmea_sentence_talker_key(line);
  char *save = NULL;
  char *token = strtok_r(line, ",", &save);
  int idx = 0;
  int sats_visible = -1;

  while (token != NULL) {
    if (idx == 3) {
      sats_visible = atoi(token);
      break;
    }
    token = strtok_r(NULL, ",", &save);
    idx++;
  }

  if (sats_visible < 0) {
    return;
  }

  uint32_t sat_u32 = (uint32_t)sats_visible;
  if (sat_u32 > UINT8_MAX) {
    sat_u32 = UINT8_MAX;
  }
  int64_t now = now_ms();
  gps_update_talker_sat(s_gps_gsv_sat_by_talker, WG_GPS_TALKER_BUCKETS, talker_key,
                        (uint8_t)sat_u32, now);
  bool have_non_gn = false;
  bool have_gn = false;
  uint8_t sats_non_gn = gps_sum_recent_talker_sat(
      s_gps_gsv_sat_by_talker, WG_GPS_TALKER_BUCKETS, now, WG_GPS_GSV_FRESH_MS, 0, &have_non_gn);
  uint8_t sats_gn = gps_sum_recent_talker_sat(s_gps_gsv_sat_by_talker, WG_GPS_TALKER_BUCKETS, now,
                                              WG_GPS_GSV_FRESH_MS, 1, &have_gn);
  s_uart_gps.sat_in_view = have_non_gn ? sats_non_gn : (have_gn ? sats_gn : (uint8_t)sat_u32);
  s_gps_last_sats_in_view = s_uart_gps.sat_in_view;
  s_gps_last_gsv_ms = now;
}

static void parse_nmea_rmc(char *line) {
  char *save = NULL;
  char *token = strtok_r(line, ",", &save);
  int idx = 0;
  const char *status = NULL;
  const char *lat = NULL;
  const char *lat_h = NULL;
  const char *lon = NULL;
  const char *lon_h = NULL;
  const char *speed_token = NULL;
  const char *course_token = NULL;
  double speed_knots = 0.0;
  double course_deg = 0.0;

  while (token != NULL) {
    switch (idx) {
      case 2:
        status = token;
        break;
      case 3:
        lat = token;
        break;
      case 4:
        lat_h = token;
        break;
      case 5:
        lon = token;
        break;
      case 6:
        lon_h = token;
        break;
      case 7:
        speed_token = token;
        speed_knots = strtod(token, NULL);
        break;
      case 8:
        course_token = token;
        course_deg = strtod(token, NULL);
        break;
      default:
        break;
    }
    token = strtok_r(NULL, ",", &save);
    idx++;
  }

  s_gps_last_rmc_status = (status != NULL && status[0] != '\0') ? status[0] : '?';
  if (status == NULL || status[0] != 'A') {
    s_gps_rmc_reject_status++;
    return;
  }
  int32_t lat_e7 = 0;
  int32_t lon_e7 = 0;
  if (!parse_nmea_latlon(lat, lat_h, &lat_e7) || !parse_nmea_latlon(lon, lon_h, &lon_e7)) {
    s_gps_rmc_reject_latlon++;
    return;
  }

  s_uart_gps.lat_e7 = lat_e7;
  s_uart_gps.lon_e7 = lon_e7;
  if (speed_token != NULL && speed_token[0] != '\0' && isfinite(speed_knots) && speed_knots >= 0.0) {
    s_uart_gps.flags |= WG_GPS_FLAG_HAS_SPEED;
    s_uart_gps.speed_mmps = (uint32_t)llround(fmax(speed_knots, 0.0) * 514.444);
  } else {
    s_uart_gps.flags &= (uint8_t)~WG_GPS_FLAG_HAS_SPEED;
    s_uart_gps.speed_mmps = 0;
  }
  if (course_token != NULL && course_token[0] != '\0' && isfinite(course_deg) && course_deg >= 0.0) {
    s_uart_gps.flags |= WG_GPS_FLAG_HAS_BEARING;
    s_uart_gps.bearing_mdeg = (uint32_t)llround(course_deg * 1000.0);
  } else {
    s_uart_gps.flags &= (uint8_t)~WG_GPS_FLAG_HAS_BEARING;
    s_uart_gps.bearing_mdeg = 0;
  }
  s_uart_gps.received_ms = now_ms();
  s_gps_fix_updates++;
  recompute_effective_gps_fix();
}

static void process_nmea_line(const char *line) {
  if (line == NULL || line[0] != '$') {
    return;
  }
#if WG_GPS_NMEA_LOG_RAW
  ESP_LOGI(TAG, "GPS NMEA %s", line);
#endif
  s_gps_nmea_lines_total++;
  char copy[128] = {0};
  size_t n = strnlen(line, sizeof(copy) - 1);
  memcpy(copy, line, n);
  copy[n] = '\0';
  char *star = strchr(copy, '*');
  if (star != NULL) {
    *star = '\0';
  }
  if (nmea_sentence_has_type(copy, "GGA")) {
    s_gps_nmea_gga++;
    parse_nmea_gga(copy);
  } else if (nmea_sentence_has_type(copy, "RMC")) {
    s_gps_nmea_rmc++;
    parse_nmea_rmc(copy);
  } else if (nmea_sentence_has_type(copy, "GSA")) {
    s_gps_nmea_gsa++;
    parse_nmea_gsa(copy);
  } else if (nmea_sentence_has_type(copy, "GSV")) {
    s_gps_nmea_gsv++;
    parse_nmea_gsv(copy);
  } else {
    s_gps_nmea_other++;
  }
}

static void gps_uart_task(void *arg) {
  (void)arg;
  uint8_t buf[64];
  int detected_baud = gps_probe_baud();
  if (detected_baud <= 0) {
    detected_baud = WG_GPS_UART_BAUD;
  }
  s_gps_uart_baud_active = detected_baud;
  ESP_LOGI(TAG,
           "GPS UART init uart=%d tx=%d rx=%d boot_baud=%d detected=%d target=%d",
           WG_GPS_UART_NUM, WG_GPS_UART_TX_GPIO, WG_GPS_UART_RX_GPIO, WG_GPS_UART_BAUD,
           detected_baud, WG_GPS_UART_TARGET_BAUD);

  if (detected_baud != WG_GPS_UART_TARGET_BAUD) {
    if (gps_send_cfg_prt_uart1_baud(WG_GPS_UART_TARGET_BAUD)) {
      vTaskDelay(pdMS_TO_TICKS(180));
      if (uart_set_baudrate(WG_GPS_UART_NUM, WG_GPS_UART_TARGET_BAUD) == ESP_OK) {
        s_gps_uart_baud_active = WG_GPS_UART_TARGET_BAUD;
        if (!gps_detect_nmea_at_baud(WG_GPS_UART_TARGET_BAUD, 1400)) {
          ESP_LOGW(TAG, "GPS baud switch verify failed; reverting to %d", detected_baud);
          if (uart_set_baudrate(WG_GPS_UART_NUM, detected_baud) == ESP_OK) {
            s_gps_uart_baud_active = detected_baud;
          }
        } else {
          ESP_LOGI(TAG, "GPS baud switched to %d", WG_GPS_UART_TARGET_BAUD);
        }
      } else {
        ESP_LOGW(TAG, "GPS baud switch set failed; staying on %d", detected_baud);
      }
    } else {
      ESP_LOGW(TAG, "GPS baud switch command failed; staying on %d", detected_baud);
    }
  }

  s_gps_nav_rate_hz = 1;
  if (gps_apply_nav_profile(s_gps_nav_rate_hz, true)) {
    s_gps_last_rate_change_ms = now_ms();
    ESP_LOGI(TAG, "GPS nav profile configured: %uHz mode=%s", (unsigned)s_gps_nav_rate_hz,
             gps_nav_mode_name(s_gps_nav_mode));
  } else {
    s_gps_last_rate_change_ms = now_ms();
    ESP_LOGW(TAG, "GPS nav profile setup incomplete");
  }
  if (gps_apply_m9_profile()) {
    ESP_LOGI(TAG, "GPS M9 profile configured: PSM off + multi-GNSS enabled");
  } else {
    ESP_LOGW(TAG, "GPS M9 profile setup incomplete");
  }

  int64_t last_rate_eval_ms = now_ms();
  while (true) {
    int n = uart_read_bytes(WG_GPS_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(50));
    if (n <= 0) {
      recompute_effective_gps_fix();
    } else {
      s_gps_uart_rx_bytes += (uint32_t)n;
      s_gps_last_rx_ms = now_ms();
      for (int i = 0; i < n; ++i) {
        char c = (char)buf[i];
        if (c == '\r') {
          continue;
        }
        if (c == '\n') {
          if (s_gps_line_len > 0) {
            s_gps_line[s_gps_line_len] = '\0';
            process_nmea_line(s_gps_line);
            s_gps_line_len = 0;
          }
          continue;
        }
        if (s_gps_line_len < sizeof(s_gps_line) - 1) {
          s_gps_line[s_gps_line_len++] = c;
        } else {
          s_gps_line_len = 0;
        }
      }
    }

    int64_t ms_now = now_ms();
    if ((ms_now - last_rate_eval_ms) >= WG_GPS_RATE_EVAL_MS) {
      last_rate_eval_ms = ms_now;
      uint8_t current_hz = s_gps_nav_rate_hz > 0 ? s_gps_nav_rate_hz : 1;
      uint8_t desired_hz = gps_target_nav_rate_hz(current_hz, &s_uart_gps, s_gps_nav_mode);
      if (desired_hz != current_hz &&
          (s_gps_last_rate_change_ms <= 0 ||
           (ms_now - s_gps_last_rate_change_ms) >= WG_GPS_RATE_SWITCH_MIN_MS)) {
        if (gps_apply_nav_profile(desired_hz, false)) {
          ESP_LOGI(TAG, "GPS nav rate %uHz -> %uHz speed_mmps=%lu", (unsigned)current_hz,
                   (unsigned)desired_hz, (unsigned long)s_uart_gps.speed_mmps);
          s_gps_nav_rate_hz = desired_hz;
          s_gps_last_rate_change_ms = ms_now;
        } else {
          ESP_LOGW(TAG, "GPS nav rate switch failed current=%u desired=%u", (unsigned)current_hz,
                   (unsigned)desired_hz);
        }
      }
    }
  }
}

static bool node_write_frame(uint8_t type, const uint8_t *payload, uint16_t payload_len) {
  if (payload_len > WG_NODE_FRAME_MAX_PAYLOAD) {
    ESP_LOGW(TAG, "node tx drop: payload too large type=%s len=%u", node_type_name(type),
             (unsigned)payload_len);
    s_node_tx_failures++;
    return false;
  }
  uint8_t header[2 + WG_NODE_FRAME_HEADER_SIZE] = {0};
  header[0] = WG_NODE_FRAME_SYNC0;
  header[1] = WG_NODE_FRAME_SYNC1;
  header[2] = WG_NODE_FRAME_VERSION;
  header[3] = type;
  header[4] = (uint8_t)(s_node_seq & 0xFF);
  header[5] = (uint8_t)((s_node_seq >> 8) & 0xFF);
  header[6] = (uint8_t)(payload_len & 0xFF);
  header[7] = (uint8_t)((payload_len >> 8) & 0xFF);
  uint32_t dms = ble_device_ms();
  header[8] = (uint8_t)(dms & 0xFF);
  header[9] = (uint8_t)((dms >> 8) & 0xFF);
  header[10] = (uint8_t)((dms >> 16) & 0xFF);
  header[11] = (uint8_t)((dms >> 24) & 0xFF);
  s_node_seq++;

  uint16_t crc = crc16_ccitt(&header[2], WG_NODE_FRAME_HEADER_SIZE);
  if (payload_len > 0 && payload != NULL) {
    crc = crc16_ccitt_seed(crc, payload, payload_len);
  }
  uint8_t crc_bytes[2] = {(uint8_t)(crc & 0xFF), (uint8_t)((crc >> 8) & 0xFF)};

  if (uart_write_bytes(WG_NODE_UART_NUM, (const char *)header, sizeof(header)) < 0) {
    ESP_LOGW(TAG, "node tx failed: header type=%s len=%u", node_type_name(type),
             (unsigned)payload_len);
    s_node_tx_failures++;
    return false;
  }
  if (payload_len > 0 &&
      uart_write_bytes(WG_NODE_UART_NUM, (const char *)payload, payload_len) < 0) {
    ESP_LOGW(TAG, "node tx failed: payload type=%s len=%u", node_type_name(type),
             (unsigned)payload_len);
    s_node_tx_failures++;
    return false;
  }
  if (uart_write_bytes(WG_NODE_UART_NUM, (const char *)crc_bytes, sizeof(crc_bytes)) < 0) {
    ESP_LOGW(TAG, "node tx failed: crc type=%s len=%u", node_type_name(type), (unsigned)payload_len);
    s_node_tx_failures++;
    return false;
  }

  s_node_tx_frames++;
  s_node_last_tx_type = type;
  s_node_last_tx_payload_len = payload_len;
  s_node_last_tx_ms = now_ms();
  return true;
}

static bool node_send_config(void) {
  uint8_t payload[13] = {0};
  bool scan_enable = s_scanning && node_channel_plan_enabled(s_node_channel_mask, s_node_channel_mask_5ghz);
  bool report_enable = scan_enable && s_latest_gps.valid;
  payload[0] = (scan_enable ? 1U : 0U) | (report_enable ? 2U : 0U);
  payload[1] = (uint8_t)(s_hop_ms & 0xFF);
  payload[2] = (uint8_t)((s_hop_ms >> 8) & 0xFF);
  payload[3] = (uint8_t)(s_node_channel_mask & 0xFF);
  payload[4] = (uint8_t)((s_node_channel_mask >> 8) & 0xFF);
  for (size_t i = 0; i < 8; ++i) {
    payload[5 + i] = (uint8_t)((s_node_channel_mask_5ghz >> (8U * i)) & 0xFFU);
  }
  s_node_report_enable = report_enable;
  return node_write_frame(WG_NODE_MSG_CONFIG, payload, sizeof(payload));
}

static void node_send_hello(void) {
  uint8_t payload[1] = {1};
  (void)node_write_frame(WG_NODE_MSG_HELLO, payload, sizeof(payload));
}

static void node_send_ping(void) { (void)node_write_frame(WG_NODE_MSG_PING, NULL, 0); }

static void node_ingest_sighting(const uint8_t *payload, uint16_t payload_len) {
  if (!s_latest_gps.valid || payload_len < 14) {
    return;
  }
  uint8_t ssid_len = payload[12];
  if (ssid_len > 32 || payload_len < (uint16_t)(14 + ssid_len)) {
    return;
  }
  char ssid[33] = {0};
  if (ssid_len > 0) {
    memcpy(ssid, &payload[13], ssid_len);
    ssid[ssid_len] = '\0';
  }
  int64_t ts_ms = now_ms();
  bool is_new = false;
  bool is_updated = false;
  ap_entry_t *entry =
      upsert_ap_entry(payload, ssid, ssid_len, payload[8], payload[9], payload[10], payload[11],
                      payload[6], (int8_t)payload[7], ts_ms, false, &is_new, &is_updated);
  if (entry == NULL || !should_notify_entry(entry, is_new, is_updated, ts_ms)) {
    return;
  }
  uint8_t flags = payload[13 + ssid_len];
  publish_sighting_event(entry, flags, 1);
  if (s_node_status.forwarded_sightings < UINT16_MAX) {
    s_node_status.forwarded_sightings++;
  }
}

static void node_handle_frame(const uint8_t *header, const uint8_t *payload, uint16_t payload_len) {
  uint8_t type = header[1];
  s_node_rx_frames++;
  s_node_last_rx_type = type;
  s_node_last_rx_payload_len = payload_len;
  s_node_status.last_rx_ms = now_ms();
  s_node_status.link_up = true;

  if (type == WG_NODE_MSG_HELLO_ACK) {
    s_node_seen_hello = true;
    ESP_LOGI(TAG, "node rx HELLO_ACK");
    return;
  }
  if (type == WG_NODE_MSG_STATUS && payload_len >= 10) {
    bool remote_scan = (payload[0] & 0x01U) != 0U;
    bool remote_report = (payload[0] & 0x02U) != 0U;
    s_node_status.channel = payload[1];
    s_node_status.channel_mask = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
    s_node_status.channel_mask_5ghz = 0;
    if (payload_len >= 18) {
      for (size_t i = 0; i < 8; ++i) {
        s_node_status.channel_mask_5ghz |= ((uint64_t)payload[10 + i] << (8U * i));
      }
    }
    s_node_status.packets_per_sec = (uint16_t)payload[6] | ((uint16_t)payload[7] << 8);
    (void)remote_scan;
    (void)remote_report;
    return;
  }
  if (type == WG_NODE_MSG_SIGHTING) {
    node_ingest_sighting(payload, payload_len);
    return;
  }
  if (type == WG_NODE_MSG_PONG) {
    return;
  }
}

static void node_consume_byte(uint8_t b) {
  switch (s_node_rx_state) {
    case 0:
      if (b == WG_NODE_FRAME_SYNC0) {
        s_node_rx_state = 1;
      }
      break;
    case 1:
      if (b == WG_NODE_FRAME_SYNC1) {
        s_node_rx_state = 2;
        s_node_hdr_pos = 0;
      } else {
        s_node_rx_state = 0;
      }
      break;
    case 2:
      s_node_hdr[s_node_hdr_pos++] = b;
      if (s_node_hdr_pos >= WG_NODE_FRAME_HEADER_SIZE) {
        if (s_node_hdr[0] != WG_NODE_FRAME_VERSION) {
          s_node_rx_state = 0;
          break;
        }
        s_node_payload_len = (uint16_t)s_node_hdr[4] | ((uint16_t)s_node_hdr[5] << 8);
        if (s_node_payload_len > WG_NODE_FRAME_MAX_PAYLOAD) {
          s_node_rx_state = 0;
          break;
        }
        s_node_payload_pos = 0;
        s_node_crc_pos = 0;
        s_node_rx_state = (s_node_payload_len == 0) ? 4 : 3;
      }
      break;
    case 3:
      s_node_payload[s_node_payload_pos++] = b;
      if (s_node_payload_pos >= s_node_payload_len) {
        s_node_rx_state = 4;
      }
      break;
    case 4:
      s_node_crc_bytes[s_node_crc_pos++] = b;
      if (s_node_crc_pos >= 2) {
        uint16_t rx_crc = (uint16_t)s_node_crc_bytes[0] | ((uint16_t)s_node_crc_bytes[1] << 8);
        uint16_t calc = crc16_ccitt(s_node_hdr, WG_NODE_FRAME_HEADER_SIZE);
        if (s_node_payload_len > 0) {
          calc = crc16_ccitt_seed(calc, s_node_payload, s_node_payload_len);
        }
        if (calc == rx_crc) {
          node_handle_frame(s_node_hdr, s_node_payload, s_node_payload_len);
        } else {
          if (s_node_rx_crc_failures < UINT16_MAX) {
            s_node_rx_crc_failures++;
          }
        }
        s_node_rx_state = 0;
      }
      break;
    default:
      s_node_rx_state = 0;
      break;
  }
}

static void node_uart_task(void *arg) {
  (void)arg;
  uint8_t buf[64];
  int64_t last_ping_ms = 0;
  int64_t last_cfg_ms = 0;
  int64_t last_hello_ms = 0;
  ESP_LOGI(TAG, "Node UART init uart=%d tx=%d rx=%d baud=%d", WG_NODE_UART_NUM,
           WG_NODE_UART_TX_GPIO, WG_NODE_UART_RX_GPIO, WG_NODE_UART_BAUD);
  while (true) {
    int n = uart_read_bytes(WG_NODE_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(40));
    if (n > 0) {
      s_node_uart_rx_bytes += (uint32_t)n;
    }
    for (int i = 0; i < n; ++i) {
      node_consume_byte(buf[i]);
    }
    int64_t ms = now_ms();
    if (!s_node_seen_hello && (ms - last_hello_ms > 2000)) {
      node_send_hello();
      last_hello_ms = ms;
    }
    if (ms - last_cfg_ms > 1000) {
      node_send_config();
      last_cfg_ms = ms;
    }
    if (ms - last_ping_ms > 1000) {
      node_send_ping();
      last_ping_ms = ms;
    }
    if (s_node_status.link_up && (ms - s_node_status.last_rx_ms) > WG_NODE_STALE_MS) {
      s_node_status.link_up = false;
      ESP_LOGW(TAG, "node link stale");
    }
  }
}

static void host_serial_consume_byte(wg_host_backend_t backend, uint8_t b) {
  wg_host_rx_parser_t *parser = host_parser_for_backend(backend);
  if (parser == NULL) {
    return;
  }

  if (parser->pos == 0) {
    if (b != WG_FRAME_MAGIC0) {
      return;
    }
    parser->expected = 0;
    parser->buf[parser->pos++] = b;
    return;
  }

  if (parser->pos == 1) {
    if (b == WG_FRAME_MAGIC1) {
      parser->buf[parser->pos++] = b;
      return;
    }
    parser->pos = (b == WG_FRAME_MAGIC0) ? 1 : 0;
    parser->expected = 0;
    if (parser->pos == 1) {
      parser->buf[0] = WG_FRAME_MAGIC0;
    }
    return;
  }

  if (parser->pos >= sizeof(parser->buf)) {
    parser->errors++;
    s_host_rx_errors++;
    host_serial_reset_rx(parser);
    return;
  }

  parser->buf[parser->pos++] = b;
  if (parser->pos == WG_FRAME_HEADER_SIZE) {
    uint16_t payload_len = (uint16_t)parser->buf[6] | ((uint16_t)parser->buf[7] << 8);
    if (payload_len > WG_HOST_FRAME_MAX_PAYLOAD) {
      parser->errors++;
      s_host_rx_errors++;
      host_serial_reset_rx(parser);
      return;
    }
    parser->expected = (size_t)WG_FRAME_HEADER_SIZE + (size_t)payload_len;
    if (parser->expected > sizeof(parser->buf)) {
      parser->errors++;
      s_host_rx_errors++;
      host_serial_reset_rx(parser);
      return;
    }
  }

  if (parser->expected > 0 && parser->pos >= parser->expected) {
    wg_frame_t preview = {0};
    if (!s_host_serial_active &&
        wg_frame_decode(parser->buf, parser->expected, &preview) == WG_OK &&
        preview.version == WG_PROTOCOL_VERSION && preview.type == WG_MSG_COMMAND) {
      host_serial_mark_active();
    }
    uint8_t cmd_id = 0;
    uint8_t result = process_control_frame(parser->buf, parser->expected, &cmd_id);
    if (cmd_id != 0) {
      host_serial_mark_active();
      s_host_tx_backend = backend;
    }
    if (s_host_serial_active) {
      (void)send_ack_host(cmd_id, result);
    }
    parser->frames++;
    s_host_rx_frames++;
    host_serial_reset_rx(parser);
  }
}

static void host_serial_task(void *arg) {
  (void)arg;
  uint8_t buf[96];
  while (true) {
    bool got_data = false;
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    if (s_host_usb_enabled) {
      int n = host_serial_read_bytes(WG_HOST_BACKEND_USB, buf, sizeof(buf), 0);
      if (n > 0) {
        got_data = true;
        for (int i = 0; i < n; ++i) {
          host_serial_consume_byte(WG_HOST_BACKEND_USB, buf[i]);
        }
      }
    }
#endif
    if (!got_data) {
      vTaskDelay(pdMS_TO_TICKS(WG_HOST_SERIAL_READ_MS));
    }
  }
}

static void init_host_serial_bridge(void) {
  s_host_serial_enabled = false;
  s_host_uart_enabled = false;
  s_host_usb_enabled = false;
  s_host_backend = WG_HOST_BACKEND_NONE;
  s_host_tx_backend = WG_HOST_BACKEND_NONE;

#if SOC_USB_SERIAL_JTAG_SUPPORTED
  esp_err_t rc = ESP_FAIL;
  usb_serial_jtag_driver_config_t cfg = {
      .tx_buffer_size = 4096,
      .rx_buffer_size = 1024,
  };
  rc = usb_serial_jtag_driver_install(&cfg);
  if (rc == ESP_OK || rc == ESP_ERR_INVALID_STATE) {
    s_host_usb_enabled = true;
  } else {
    ESP_LOGW(TAG, "host serial usb init failed rc=%s", esp_err_to_name(rc));
  }
#endif

  s_host_serial_enabled = s_host_usb_enabled;
  if (s_host_usb_enabled) {
    s_host_backend = WG_HOST_BACKEND_USB;
  }
  if (!s_host_serial_enabled) {
    ESP_LOGW(TAG, "host serial bridge disabled: no backend");
    return;
  }

  s_host_tx_mutex = xSemaphoreCreateMutex();
  if (s_host_tx_mutex == NULL) {
    ESP_LOGW(TAG, "host serial disabled: mutex allocation failed");
    s_host_serial_enabled = false;
    s_host_uart_enabled = false;
    s_host_usb_enabled = false;
    s_host_backend = WG_HOST_BACKEND_NONE;
    s_host_tx_backend = WG_HOST_BACKEND_NONE;
    return;
  }
  host_serial_reset_rx(&s_host_uart_rx);
  host_serial_reset_rx(&s_host_usb_rx);
  if (xTaskCreate(host_serial_task, "wg_host_serial", 4096, NULL, 5, NULL) != pdPASS) {
    ESP_LOGW(TAG, "host serial disabled: task create failed");
    s_host_serial_enabled = false;
    s_host_uart_enabled = false;
    s_host_usb_enabled = false;
    s_host_backend = WG_HOST_BACKEND_NONE;
    s_host_tx_backend = WG_HOST_BACKEND_NONE;
    return;
  }
  ESP_LOGI(TAG, "host serial bridge ready over USB");
}

static void init_uart_bridges(void) {
  const uart_config_t gps_cfg = {
      .baud_rate = WG_GPS_UART_BAUD,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 0,
      .source_clk = UART_SCLK_DEFAULT,
  };
  ESP_ERROR_CHECK(uart_driver_install(WG_GPS_UART_NUM, 2048, 0, 0, NULL, 0));
  ESP_ERROR_CHECK(uart_param_config(WG_GPS_UART_NUM, &gps_cfg));
  ESP_ERROR_CHECK(uart_set_pin(WG_GPS_UART_NUM, WG_GPS_UART_TX_GPIO, WG_GPS_UART_RX_GPIO,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

  uart_config_t node_cfg = {
      .baud_rate = WG_NODE_UART_BAUD,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 0,
#if (SOC_UART_LP_NUM >= 1)
      .lp_source_clk = LP_UART_SCLK_DEFAULT,
#else
      .source_clk = UART_SCLK_DEFAULT,
#endif
  };
  ESP_ERROR_CHECK(uart_driver_install(WG_NODE_UART_NUM, 4096, 2048, 0, NULL, 0));
  ESP_ERROR_CHECK(uart_param_config(WG_NODE_UART_NUM, &node_cfg));
  ESP_ERROR_CHECK(uart_set_pin(WG_NODE_UART_NUM, WG_NODE_UART_TX_GPIO, WG_NODE_UART_RX_GPIO,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
  ESP_ERROR_CHECK(uart_flush_input(WG_NODE_UART_NUM));

  ESP_LOGI(TAG, "UART bridges ready local_mask=0x%04X node_mask24=0x%04X node_mask5=0x%011llX",
           s_local_channel_mask, s_node_channel_mask, (unsigned long long)s_node_channel_mask_5ghz);
  node_send_hello();
  log_bridge_diagnostics(true);

  xTaskCreate(gps_uart_task, "wg_gps_uart", 4096, NULL, 5, NULL);
  xTaskCreate(node_uart_task, "wg_node_uart", 4096, NULL, 5, NULL);
}

static void rate_counter_tick(int64_t ms_now) {
  if (s_rate_window_start_ms == 0) {
    s_rate_window_start_ms = ms_now;
    return;
  }
  const int64_t elapsed = ms_now - s_rate_window_start_ms;
  if (elapsed >= 1000) {
    s_packets_per_sec = (uint16_t)((s_rate_packet_acc * 1000ULL) / (uint64_t)elapsed);
    s_rate_packet_acc = 0;
    s_rate_window_start_ms = ms_now;
  }
}

static bool ble_notify_framed(uint16_t attr_handle, uint8_t type, const uint8_t *payload,
                              uint16_t payload_len) {
  if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
    return false;
  }

  uint8_t frame[WG_FRAME_HEADER_SIZE + WG_BLE_NOTIFY_FRAME_MAX_PAYLOAD_MAX];
  uint16_t max_payload = ble_notify_frame_payload_limit();
  if (payload_len > max_payload ||
      (size_t)payload_len > sizeof(frame) - WG_FRAME_HEADER_SIZE) {
    return false;
  }

  wg_frame_t out_frame = {
      .version = WG_PROTOCOL_VERSION,
      .type = type,
      .seq = s_seq++,
      .len = payload_len,
      .device_ms = ble_device_ms(),
      .payload = payload,
  };

  size_t frame_len = 0;
  if (wg_frame_encode(&out_frame, frame, sizeof(frame), &frame_len) != WG_OK) {
    return false;
  }

  struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, frame_len);
  if (om == NULL) {
    return false;
  }

  const int rc = ble_gatts_notify_custom(s_conn_handle, attr_handle, om);
  if (rc != 0) {
    s_notify_drops++;
    s_notify_drop_last_rc = rc;
    if (rc >= 0 && rc < (int)(sizeof(s_notify_drop_by_hs_rc) / sizeof(s_notify_drop_by_hs_rc[0]))) {
      s_notify_drop_by_hs_rc[rc]++;
    } else if (rc >= BLE_HS_ERR_ATT_BASE && rc < (BLE_HS_ERR_ATT_BASE + 0x100)) {
      s_notify_drop_att++;
    } else if (rc >= BLE_HS_ERR_HCI_BASE && rc < (BLE_HS_ERR_HCI_BASE + 0x100)) {
      s_notify_drop_hci++;
    } else if (rc >= BLE_HS_ERR_L2C_BASE && rc < (BLE_HS_ERR_L2C_BASE + 0x100)) {
      s_notify_drop_l2c++;
    } else {
      s_notify_drop_other++;
    }
    return false;
  }
  led_note_bt_tx();
  return true;
}

static bool send_ack(uint8_t cmd_id, uint8_t code) {
  uint8_t payload[2] = {cmd_id, code};
  return ble_notify_framed(s_status_handle, code == WG_ACK_OK ? WG_MSG_ACK : WG_MSG_ERROR,
                           payload, sizeof(payload));
}

static bool send_snapshot_end(void) {
  bool sent = false;
  if (s_status_notify_enabled) {
    sent = ble_notify_framed(s_status_handle, WG_MSG_SNAPSHOT_END, NULL, 0) || sent;
  }
  if (s_host_serial_active) {
    sent = host_serial_notify_framed(WG_MSG_SNAPSHOT_END, NULL, 0) || sent;
  }
  return sent;
}

static uint16_t rd_u16_le(const uint8_t *in) { return (uint16_t)in[0] | ((uint16_t)in[1] << 8); }

static void wr_u16_le(uint8_t *out, uint16_t value) {
  out[0] = (uint8_t)(value & 0xFF);
  out[1] = (uint8_t)((value >> 8) & 0xFF);
}

static void wr_u32_le(uint8_t *out, uint32_t value) {
  out[0] = (uint8_t)(value & 0xFF);
  out[1] = (uint8_t)((value >> 8) & 0xFF);
  out[2] = (uint8_t)((value >> 16) & 0xFF);
  out[3] = (uint8_t)((value >> 24) & 0xFF);
}

static void wr_u64_le(uint8_t *out, uint64_t value) {
  wr_u32_le(out, (uint32_t)(value & 0xFFFFFFFFULL));
  wr_u32_le(out + 4, (uint32_t)((value >> 32) & 0xFFFFFFFFULL));
}

static uint8_t rsn_cipher_flags_from_suite(const uint8_t *suite) {
  if (suite[0] != 0x00 || suite[1] != 0x0F || suite[2] != 0xAC) {
    return 0;
  }
  switch (suite[3]) {
    case 1:
    case 5:
      return WG_SEC_CIPHER_WEP;
    case 2:
      return WG_SEC_CIPHER_TKIP;
    case 4:
      return WG_SEC_CIPHER_CCMP_128;
    case 8:
      return WG_SEC_CIPHER_GCMP_128;
    case 9:
      return WG_SEC_CIPHER_GCMP_256;
    case 10:
      return WG_SEC_CIPHER_CCMP_256;
    default:
      return 0;
  }
}

static uint8_t wpa_cipher_flags_from_suite(const uint8_t *suite) {
  if (suite[0] != 0x00 || suite[1] != 0x50 || suite[2] != 0xF2) {
    return 0;
  }
  switch (suite[3]) {
    case 1:
    case 5:
      return WG_SEC_CIPHER_WEP;
    case 2:
      return WG_SEC_CIPHER_TKIP;
    case 4:
      return WG_SEC_CIPHER_CCMP_128;
    default:
      return 0;
  }
}

static uint8_t rsn_akm_flags_from_suite(const uint8_t *suite) {
  if (suite[0] != 0x00 || suite[1] != 0x0F || suite[2] != 0xAC) {
    return 0;
  }
  switch (suite[3]) {
    case 1:
    case 3:
    case 5:
      return WG_SEC_AKM_EAP;
    case 2:
    case 4:
    case 6:
      return WG_SEC_AKM_PSK;
    case 8:
    case 9:
      return WG_SEC_AKM_SAE;
    case 18:
      return WG_SEC_AKM_OWE;
    default:
      return 0;
  }
}

static uint8_t wpa_akm_flags_from_suite(const uint8_t *suite) {
  if (suite[0] != 0x00 || suite[1] != 0x50 || suite[2] != 0xF2) {
    return 0;
  }
  switch (suite[3]) {
    case 1:
      return WG_SEC_AKM_EAP;
    case 2:
      return WG_SEC_AKM_PSK;
    default:
      return 0;
  }
}

static void parse_rsn_ie(const uint8_t *ie_data, uint8_t ie_len, uint8_t *proto_flags,
                         uint8_t *akm_flags, uint8_t *cipher_flags) {
  const uint8_t *cursor = ie_data;
  size_t remaining = ie_len;

  if (remaining < 2) {
    return;
  }
  cursor += 2;
  remaining -= 2;

  if (remaining < 4) {
    return;
  }
  *cipher_flags |= rsn_cipher_flags_from_suite(cursor);
  cursor += 4;
  remaining -= 4;

  if (remaining < 2) {
    return;
  }
  uint16_t pairwise_count = rd_u16_le(cursor);
  cursor += 2;
  remaining -= 2;
  for (uint16_t i = 0; i < pairwise_count && remaining >= 4; ++i) {
    *cipher_flags |= rsn_cipher_flags_from_suite(cursor);
    cursor += 4;
    remaining -= 4;
  }

  if (remaining < 2) {
    return;
  }
  uint16_t akm_count = rd_u16_le(cursor);
  cursor += 2;
  remaining -= 2;
  for (uint16_t i = 0; i < akm_count && remaining >= 4; ++i) {
    *akm_flags |= rsn_akm_flags_from_suite(cursor);
    cursor += 4;
    remaining -= 4;
  }

  if ((*akm_flags & WG_SEC_AKM_PSK) != 0 || (*akm_flags & WG_SEC_AKM_EAP) != 0) {
    *proto_flags |= WG_SEC_PROTO_WPA2;
  }
  if ((*akm_flags & WG_SEC_AKM_SAE) != 0 || (*akm_flags & WG_SEC_AKM_OWE) != 0) {
    *proto_flags |= WG_SEC_PROTO_WPA3;
  }
  if ((*proto_flags & (WG_SEC_PROTO_WPA2 | WG_SEC_PROTO_WPA3)) == 0) {
    *proto_flags |= WG_SEC_PROTO_WPA2;
  }
}

static void parse_wpa_vendor_ie(const uint8_t *ie_data, uint8_t ie_len, uint8_t *proto_flags,
                                uint8_t *akm_flags, uint8_t *cipher_flags) {
  if (ie_len < 4 || ie_data[0] != 0x00 || ie_data[1] != 0x50 || ie_data[2] != 0xF2 ||
      ie_data[3] != 0x01) {
    return;
  }

  const uint8_t *cursor = ie_data + 4;
  size_t remaining = ie_len - 4;
  *proto_flags |= WG_SEC_PROTO_WPA;

  if (remaining < 2) {
    return;
  }
  cursor += 2;
  remaining -= 2;

  if (remaining < 4) {
    return;
  }
  *cipher_flags |= wpa_cipher_flags_from_suite(cursor);
  cursor += 4;
  remaining -= 4;

  if (remaining < 2) {
    return;
  }
  uint16_t pairwise_count = rd_u16_le(cursor);
  cursor += 2;
  remaining -= 2;
  for (uint16_t i = 0; i < pairwise_count && remaining >= 4; ++i) {
    *cipher_flags |= wpa_cipher_flags_from_suite(cursor);
    cursor += 4;
    remaining -= 4;
  }

  if (remaining < 2) {
    return;
  }
  uint16_t akm_count = rd_u16_le(cursor);
  cursor += 2;
  remaining -= 2;
  for (uint16_t i = 0; i < akm_count && remaining >= 4; ++i) {
    *akm_flags |= wpa_akm_flags_from_suite(cursor);
    cursor += 4;
    remaining -= 4;
  }
}

static wg_auth_mode_t parse_auth_mode(bool privacy, uint8_t proto_flags, uint8_t cipher_flags) {
  if ((proto_flags & WG_SEC_PROTO_WPA2) != 0 || (proto_flags & WG_SEC_PROTO_WPA3) != 0) {
    return WG_AUTH_WPA2_WPA3;
  }
  if ((proto_flags & WG_SEC_PROTO_WPA) != 0) {
    return WG_AUTH_WPA;
  }
  if ((cipher_flags & WG_SEC_CIPHER_WEP) != 0) {
    return WG_AUTH_WEP;
  }
  if (privacy) {
    return WG_AUTH_UNKNOWN;
  }
  return WG_AUTH_OPEN;
}

static bool copy_ssid_if_clean_ascii(const uint8_t *in, uint8_t len, char *out,
                                     uint8_t *out_len) {
  uint8_t capped = len;
  if (capped > 32) {
    capped = 32;
  }
  for (uint8_t i = 0; i < capped; ++i) {
    const unsigned char ch = in[i];
    if (!isprint(ch)) {
      out[0] = '\0';
      *out_len = 0;
      return false;
    }
    out[i] = (char)ch;
  }
  out[capped] = '\0';
  *out_len = capped;
  return true;
}

static void parse_beacon_probe(const uint8_t *payload, size_t payload_len, char *ssid,
                               uint8_t *ssid_len, uint8_t *auth_mode_out, uint8_t *proto_flags_out,
                               uint8_t *akm_flags_out, uint8_t *cipher_flags_out) {
  bool privacy = false;
  *ssid_len = 0;
  ssid[0] = '\0';
  *auth_mode_out = WG_AUTH_UNKNOWN;
  *proto_flags_out = 0;
  *akm_flags_out = 0;
  *cipher_flags_out = 0;

  const size_t mgmt_hdr_len = wardrive_mgmt_header_len(payload, payload_len);
  if (mgmt_hdr_len == 0 || payload_len < mgmt_hdr_len + 12) {
    return;
  }

  const uint16_t capability_info =
      (uint16_t)payload[mgmt_hdr_len + 10] | ((uint16_t)payload[mgmt_hdr_len + 11] << 8);
  privacy = (capability_info & 0x0010U) != 0;

  const uint8_t *ie = payload + mgmt_hdr_len + 12;
  size_t remaining = payload_len - mgmt_hdr_len - 12;

  while (remaining >= 2) {
    const uint8_t id = ie[0];
    const uint8_t len = ie[1];
    if ((size_t)len + 2 > remaining) {
      break;
    }

    if (id == 0) {
      if (len > 0 || *ssid_len == 0) {
        uint8_t parsed_len = 0;
        if (copy_ssid_if_clean_ascii(ie + 2, len, ssid, &parsed_len)) {
          *ssid_len = parsed_len;
        } else if (*ssid_len == 0) {
          ssid[0] = '\0';
          *ssid_len = 0;
        }
      }
    } else if (id == 48) {
      parse_rsn_ie(ie + 2, len, proto_flags_out, akm_flags_out, cipher_flags_out);
    } else if (id == 221 && len >= 4) {
      parse_wpa_vendor_ie(ie + 2, len, proto_flags_out, akm_flags_out, cipher_flags_out);
    }

    ie += (size_t)len + 2;
    remaining -= (size_t)len + 2;
  }

  *auth_mode_out = (uint8_t)parse_auth_mode(privacy, *proto_flags_out, *cipher_flags_out);
}

static int find_ap_entry(const uint8_t bssid[6]) {
  for (int i = 0; i < WG_SEEN_CAPACITY; ++i) {
    if (s_seen[i].in_use && memcmp(s_seen[i].bssid, bssid, 6) == 0) {
      return i;
    }
  }
  return -1;
}

static int pick_entry_slot(void) {
  int free_idx = -1;
  int oldest_idx = 0;
  int64_t oldest_ms = INT64_MAX;

  for (int i = 0; i < WG_SEEN_CAPACITY; ++i) {
    if (!s_seen[i].in_use) {
      if (free_idx < 0) {
        free_idx = i;
      }
      continue;
    }
    if (s_seen[i].last_seen_ms < oldest_ms) {
      oldest_ms = s_seen[i].last_seen_ms;
      oldest_idx = i;
    }
  }

  if (free_idx >= 0) {
    return free_idx;
  }
  return oldest_idx;
}

static uint8_t bit_count_u8(uint8_t value) {
  uint8_t count = 0;
  while (value != 0) {
    count += (uint8_t)(value & 1u);
    value >>= 1;
  }
  return count;
}

static uint16_t security_score(uint8_t auth_mode, uint8_t proto_flags, uint8_t akm_flags,
                               uint8_t cipher_flags) {
  uint16_t score = 0;
  if (proto_flags != 0) {
    score = 120;
    score += (uint16_t)bit_count_u8(proto_flags) * 12u;
    score += (uint16_t)bit_count_u8(akm_flags) * 6u;
    score += (uint16_t)bit_count_u8(cipher_flags) * 3u;
    return score;
  }
  if (auth_mode == WG_AUTH_WEP || (cipher_flags & WG_SEC_CIPHER_WEP) != 0) {
    return 70;
  }
  if (auth_mode == WG_AUTH_OPEN) {
    return 30;
  }
  return 0;
}

static uint8_t resolve_auth_mode(uint8_t fallback_auth_mode, uint8_t proto_flags,
                                 uint8_t cipher_flags) {
  if ((proto_flags & WG_SEC_PROTO_WPA2) != 0 || (proto_flags & WG_SEC_PROTO_WPA3) != 0) {
    return WG_AUTH_WPA2_WPA3;
  }
  if ((proto_flags & WG_SEC_PROTO_WPA) != 0) {
    return WG_AUTH_WPA;
  }
  if ((cipher_flags & WG_SEC_CIPHER_WEP) != 0 || fallback_auth_mode == WG_AUTH_WEP) {
    return WG_AUTH_WEP;
  }
  if (fallback_auth_mode == WG_AUTH_OPEN) {
    return WG_AUTH_OPEN;
  }
  return WG_AUTH_UNKNOWN;
}

static ap_entry_t *upsert_ap_entry(const uint8_t bssid[6], const char *ssid, uint8_t ssid_len,
                                   uint8_t auth_mode, uint8_t proto_flags, uint8_t akm_flags,
                                   uint8_t cipher_flags, uint8_t channel, int8_t rssi,
                                   int64_t ts_ms, bool allow_ssid_replace, bool *is_new,
                                   bool *is_updated) {
  int idx = find_ap_entry(bssid);
  *is_new = false;
  *is_updated = false;

  if (idx < 0) {
    idx = pick_entry_slot();
    unique_bssid_track(bssid);
    memset(&s_seen[idx], 0, sizeof(s_seen[idx]));
    memcpy(s_seen[idx].bssid, bssid, 6);
    s_seen[idx].in_use = true;
    s_seen[idx].last_notified_ms = 0;
    *is_new = true;
    *is_updated = true;
  }

  ap_entry_t *entry = &s_seen[idx];
  uint8_t merged_auth = auth_mode;
  uint8_t merged_proto = proto_flags;
  uint8_t merged_akm = akm_flags;
  uint8_t merged_cipher = cipher_flags;

  if (!*is_new) {
    const uint16_t existing_score =
        security_score(entry->auth_mode, entry->proto_flags, entry->akm_flags, entry->cipher_flags);
    const uint16_t incoming_score = security_score(auth_mode, proto_flags, akm_flags, cipher_flags);
    if (incoming_score < existing_score) {
      merged_auth = entry->auth_mode;
      merged_proto = entry->proto_flags;
      merged_akm = entry->akm_flags;
      merged_cipher = entry->cipher_flags;
    } else {
      merged_proto = entry->proto_flags | proto_flags;
      merged_akm = entry->akm_flags | akm_flags;
      merged_cipher = entry->cipher_flags | cipher_flags;
      uint8_t auth_hint = auth_mode != WG_AUTH_UNKNOWN ? auth_mode : entry->auth_mode;
      merged_auth = resolve_auth_mode(auth_hint, merged_proto, merged_cipher);
    }
  } else {
    merged_auth = resolve_auth_mode(auth_mode, merged_proto, merged_cipher);
  }

  bool accept_ssid = false;
  if (ssid_len > 0) {
    if (entry->ssid_len == 0) {
      accept_ssid = true;
    } else if (allow_ssid_replace &&
               (entry->ssid_len != ssid_len || memcmp(entry->ssid, ssid, ssid_len) != 0)) {
      accept_ssid = true;
    }
  }
  bool ssid_changed = accept_ssid;

  if (entry->channel != channel || entry->rssi != rssi || entry->auth_mode != merged_auth ||
      entry->proto_flags != merged_proto || entry->akm_flags != merged_akm ||
      entry->cipher_flags != merged_cipher || ssid_changed) {
    *is_updated = true;
  }

  if (accept_ssid) {
    memcpy(entry->ssid, ssid, ssid_len);
    entry->ssid[ssid_len] = '\0';
    entry->ssid_len = ssid_len;
  }
  entry->auth_mode = merged_auth;
  entry->proto_flags = merged_proto;
  entry->akm_flags = merged_akm;
  entry->cipher_flags = merged_cipher;
  entry->channel = channel;
  entry->rssi = rssi;
  entry->last_seen_ms = ts_ms;
  return entry;
}

static bool should_notify_entry(ap_entry_t *entry, bool is_new, bool is_updated, int64_t ts_ms) {
  if (is_new || is_updated) {
    if (entry->last_notified_ms == 0 || ts_ms - entry->last_notified_ms >= 1000) {
      entry->last_notified_ms = ts_ms;
      return true;
    }
  }
  if (ts_ms - entry->last_notified_ms >= WG_SIGHTING_NOTIFY_INTERVAL_MS) {
    entry->last_notified_ms = ts_ms;
    return true;
  }
  return false;
}

static bool should_persist_entry(ap_entry_t *entry, uint8_t flags, const gps_fix_t *gps,
                                 int64_t ts_ms) {
  if (entry == NULL || gps == NULL || !gps->valid) {
    return false;
  }
  if ((flags & (WG_SIGHTING_FLAG_NEW | WG_SIGHTING_FLAG_UPDATED)) != 0) {
    return true;
  }
  if (entry->last_persisted_ms <= 0) {
    return true;
  }

  int64_t age_ms = ts_ms - entry->last_persisted_ms;
  if (age_ms < 0) {
    age_ms = 0;
  }
  if (age_ms >= WG_STORAGE_PERSIST_MAX_INTERVAL_MS) {
    return true;
  }

  int rssi_delta = (int)entry->rssi - (int)entry->last_persisted_rssi;
  if (rssi_delta < 0) {
    rssi_delta = -rssi_delta;
  }
  if (rssi_delta >= WG_STORAGE_PERSIST_RSSI_DELTA_DB && age_ms >= 3000) {
    return true;
  }

  int64_t min_interval_ms = (gps->speed_mmps >= WG_STORAGE_MOVING_SPEED_MMPS)
                                ? WG_STORAGE_PERSIST_MIN_MOVING_MS
                                : WG_STORAGE_PERSIST_MIN_STATIONARY_MS;
  return age_ms >= min_interval_ms;
}

static void authmode_to_security(wifi_auth_mode_t wifi_auth, uint8_t *auth_mode_out,
                                 uint8_t *proto_flags_out, uint8_t *akm_flags_out,
                                 uint8_t *cipher_flags_out) {
  *auth_mode_out = WG_AUTH_UNKNOWN;
  *proto_flags_out = 0;
  *akm_flags_out = 0;
  *cipher_flags_out = 0;

  switch (wifi_auth) {
    case WIFI_AUTH_OPEN:
      *auth_mode_out = WG_AUTH_OPEN;
      return;
    case WIFI_AUTH_WEP:
      *auth_mode_out = WG_AUTH_WEP;
      *cipher_flags_out = WG_SEC_CIPHER_WEP;
      return;
    case WIFI_AUTH_WPA_PSK:
      *auth_mode_out = WG_AUTH_WPA;
      *proto_flags_out = WG_SEC_PROTO_WPA;
      *akm_flags_out = WG_SEC_AKM_PSK;
      *cipher_flags_out = WG_SEC_CIPHER_TKIP | WG_SEC_CIPHER_CCMP_128;
      return;
    case WIFI_AUTH_WPA_WPA2_PSK:
      *auth_mode_out = WG_AUTH_WPA2_WPA3;
      *proto_flags_out = WG_SEC_PROTO_WPA | WG_SEC_PROTO_WPA2;
      *akm_flags_out = WG_SEC_AKM_PSK;
      *cipher_flags_out = WG_SEC_CIPHER_TKIP | WG_SEC_CIPHER_CCMP_128;
      return;
    case WIFI_AUTH_WPA3_PSK:
      *auth_mode_out = WG_AUTH_WPA2_WPA3;
      *proto_flags_out = WG_SEC_PROTO_WPA3;
      *akm_flags_out = WG_SEC_AKM_SAE;
      *cipher_flags_out = WG_SEC_CIPHER_CCMP_128;
      return;
    case WIFI_AUTH_WPA2_WPA3_PSK:
      *auth_mode_out = WG_AUTH_WPA2_WPA3;
      *proto_flags_out = WG_SEC_PROTO_WPA2 | WG_SEC_PROTO_WPA3;
      *akm_flags_out = WG_SEC_AKM_PSK | WG_SEC_AKM_SAE;
      *cipher_flags_out = WG_SEC_CIPHER_CCMP_128;
      return;
    case WIFI_AUTH_OWE:
      *auth_mode_out = WG_AUTH_WPA2_WPA3;
      *proto_flags_out = WG_SEC_PROTO_WPA3;
      *akm_flags_out = WG_SEC_AKM_OWE;
      *cipher_flags_out = WG_SEC_CIPHER_CCMP_128;
      return;
    case WIFI_AUTH_WPA_ENTERPRISE:
      *auth_mode_out = WG_AUTH_WPA;
      *proto_flags_out = WG_SEC_PROTO_WPA;
      *akm_flags_out = WG_SEC_AKM_EAP;
      *cipher_flags_out = WG_SEC_CIPHER_TKIP | WG_SEC_CIPHER_CCMP_128;
      return;
    case WIFI_AUTH_WPA2_ENTERPRISE:
      *auth_mode_out = WG_AUTH_WPA2_WPA3;
      *proto_flags_out = WG_SEC_PROTO_WPA2;
      *akm_flags_out = WG_SEC_AKM_EAP;
      *cipher_flags_out = WG_SEC_CIPHER_CCMP_128;
      return;
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENT_192:
      *auth_mode_out = WG_AUTH_WPA2_WPA3;
      *proto_flags_out = WG_SEC_PROTO_WPA3;
      *akm_flags_out = WG_SEC_AKM_EAP;
      *cipher_flags_out = WG_SEC_CIPHER_CCMP_128;
      return;
    default:
      if (wifi_auth >= WIFI_AUTH_WPA2_PSK) {
        *auth_mode_out = WG_AUTH_WPA2_WPA3;
      }
      return;
  }
}

static void seed_ap_cache_from_active_scan(void) {
  wifi_scan_config_t cfg = {0};
  cfg.show_hidden = true;
  cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  cfg.scan_time.active.min = 20;
  cfg.scan_time.active.max = 60;

  esp_err_t rc = esp_wifi_scan_start(&cfg, true);
  if (rc != ESP_OK) {
    ESP_LOGW(TAG, "active scan seed failed start rc=%s", esp_err_to_name(rc));
    return;
  }

  uint16_t ap_count = 0;
  rc = esp_wifi_scan_get_ap_num(&ap_count);
  if (rc != ESP_OK || ap_count == 0) {
    return;
  }

  if (ap_count > WG_SCAN_SEED_MAX_APS) {
    ap_count = WG_SCAN_SEED_MAX_APS;
  }

  wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
  if (records == NULL) {
    ESP_LOGW(TAG, "active scan seed alloc failed count=%u", ap_count);
    return;
  }

  uint16_t fetched = ap_count;
  rc = esp_wifi_scan_get_ap_records(&fetched, records);
  if (rc != ESP_OK) {
    free(records);
    ESP_LOGW(TAG, "active scan seed records failed rc=%s", esp_err_to_name(rc));
    return;
  }

  int64_t ts_ms = now_ms();
  uint16_t updated = 0;
  for (uint16_t i = 0; i < fetched; ++i) {
    char ssid[33] = {0};
    uint8_t ssid_len = (uint8_t)strnlen((const char *)records[i].ssid, 32);
    if (ssid_len > 0) {
      memcpy(ssid, records[i].ssid, ssid_len);
      ssid[ssid_len] = '\0';
    }

    uint8_t auth_mode = WG_AUTH_UNKNOWN;
    uint8_t proto_flags = 0;
    uint8_t akm_flags = 0;
    uint8_t cipher_flags = 0;
    authmode_to_security(records[i].authmode, &auth_mode, &proto_flags, &akm_flags,
                         &cipher_flags);

    bool is_new = false;
    bool is_updated = false;
    ap_entry_t *entry =
        upsert_ap_entry(records[i].bssid, ssid, ssid_len, auth_mode, proto_flags, akm_flags,
                        cipher_flags, records[i].primary, records[i].rssi, ts_ms, true, &is_new,
                        &is_updated);
    if (entry != NULL && (is_new || is_updated)) {
      updated++;
    }
  }
  free(records);
  ESP_LOGI(TAG, "active scan seed fetched=%u updated=%u", fetched, updated);
}

static bool notify_status_frame(void) {
  wg_status_payload_t payload = {0};
  build_status_payload(&payload);
  uint8_t bytes[WG_STATUS_PAYLOAD_SIZE] = {0};
  const size_t len = wg_build_status_payload(&payload, bytes, sizeof(bytes));
  bool sent = false;
  if (s_status_notify_enabled) {
    sent = ble_notify_framed(s_status_handle, WG_MSG_STATUS, bytes, (uint16_t)len) || sent;
  }
  if (s_host_serial_active) {
    sent = host_serial_notify_framed(WG_MSG_STATUS, bytes, (uint16_t)len) || sent;
  }
  return sent;
}

static bool notify_gps_frame(void) {
  wg_gps_payload_t payload = {
      .valid = s_latest_gps.valid,
      .source = (uint8_t)s_gps_source,
      .lat_e7 = s_latest_gps.lat_e7,
      .lon_e7 = s_latest_gps.lon_e7,
      .alt_mm = s_latest_gps.alt_mm,
      .speed_mmps = s_latest_gps.speed_mmps,
      .bearing_mdeg = s_latest_gps.bearing_mdeg,
      .unix_time_s = s_latest_gps.unix_time_s,
      .accuracy_cm = s_latest_gps.accuracy_cm,
      .sat_in_use = s_latest_gps.sat_in_use,
      .sat_in_view = s_latest_gps.sat_in_view,
      .sat_count = s_latest_gps.sat_count,
      .hdop_centi = s_latest_gps.hdop_centi,
      .pdop_centi = s_latest_gps.pdop_centi,
      .vdop_centi = s_latest_gps.vdop_centi,
  };
  uint8_t bytes[WG_GPS_PAYLOAD_SIZE] = {0};
  const size_t len = wg_build_gps_payload(&payload, bytes, sizeof(bytes));
  bool sent = false;
  if (s_gps_notify_enabled) {
    sent = ble_notify_framed(s_status_handle, WG_MSG_GPS, bytes, (uint16_t)len) || sent;
  }
  if (s_host_serial_active) {
    sent = host_serial_notify_framed(WG_MSG_GPS, bytes, (uint16_t)len) || sent;
  }
  return sent;
}

static size_t build_sighting_payload_bytes(const ap_entry_t *entry, uint8_t flags, uint8_t node_id,
                                           uint64_t session_id, uint32_t record_seq,
                                           uint8_t source_flags, const gps_fix_t *gps,
                                           uint8_t gps_source, uint8_t *out, size_t out_size) {
  if (entry == NULL || out == NULL) {
    return 0;
  }
  wg_sighting_payload_t payload = {0};
  memcpy(payload.bssid, entry->bssid, 6);
  payload.channel = entry->channel;
  payload.rssi = entry->rssi;
  payload.auth_mode = entry->auth_mode;
  payload.proto_flags = entry->proto_flags;
  payload.akm_flags = entry->akm_flags;
  payload.cipher_flags = entry->cipher_flags;
  payload.ssid_len = entry->ssid_len;
  memcpy(payload.ssid, entry->ssid, entry->ssid_len);
  payload.flags = flags;
  payload.session_id = session_id;
  payload.record_seq = record_seq;
  payload.node_id = node_id;
  payload.source_flags = source_flags;
  if (gps != NULL && gps->valid) {
    payload.gps_valid = 1;
    payload.gps_source = gps_source;
    payload.gps_lat_e7 = gps->lat_e7;
    payload.gps_lon_e7 = gps->lon_e7;
    payload.gps_alt_mm = gps->alt_mm;
    payload.gps_unix_time_s = gps->unix_time_s;
    payload.gps_accuracy_cm = gps->accuracy_cm;
  }

  return wg_build_sighting_payload(&payload, out, out_size);
}

static bool notify_sighting_frame_ex(const ap_entry_t *entry, uint8_t flags, uint8_t node_id,
                                     uint64_t session_id, uint32_t record_seq,
                                     uint8_t source_flags, const gps_fix_t *gps,
                                     uint8_t gps_source) {
  if (!s_sighting_notify_enabled && !s_host_serial_active) {
    return false;
  }
  uint8_t bytes[96] = {0};
  const size_t len = build_sighting_payload_bytes(entry, flags, node_id, session_id, record_seq,
                                                  source_flags, gps, gps_source, bytes,
                                                  sizeof(bytes));
  if (len == 0) {
    return false;
  }
  bool sent = false;
  if (s_sighting_notify_enabled) {
    sent = ble_notify_framed(s_sighting_handle, WG_MSG_SIGHTING, bytes, (uint16_t)len) || sent;
  }
  if (s_host_serial_active) {
    sent = host_serial_notify_framed(WG_MSG_SIGHTING, bytes, (uint16_t)len) || sent;
  }
  return sent;
}

static void publish_sighting_event(ap_entry_t *entry, uint8_t flags, uint8_t node_id) {
  if (entry == NULL) {
    return;
  }
  uint64_t session_id = 0;
  uint32_t record_seq = 0;
  bool persisted = false;
  gps_fix_t notify_gps = {0};
  uint8_t notify_gps_source = 0;

  if (s_latest_gps.valid) {
    notify_gps = s_latest_gps;
    notify_gps_source = (uint8_t)s_gps_source;
  }

  if (s_storage_ready && storage_lock(pdMS_TO_TICKS(40))) {
    if (s_session_open && s_latest_gps.valid) {
      int64_t ts_ms = now_ms();
      uint8_t record[96] = {0};
      gps_fix_t capture_gps = s_latest_gps;
      uint8_t capture_gps_source = (uint8_t)s_gps_source;
      if (should_persist_entry(entry, flags, &capture_gps, ts_ms)) {
        session_id = s_session_id;
        record_seq = s_session_next_seq;
        size_t record_len = build_sighting_payload_bytes(entry, flags, node_id, session_id, record_seq,
                                                         WG_SIGHTING_SOURCE_LIVE, &capture_gps,
                                                         capture_gps_source, record, sizeof(record));
        if (record_len > 0 && storage_enqueue_record_locked(record, (uint16_t)record_len, record_seq)) {
          if (s_session_next_seq < UINT32_MAX) {
            s_session_next_seq++;
          }
          entry->last_persisted_ms = ts_ms;
          entry->last_persisted_rssi = entry->rssi;
          persisted = true;
          notify_gps = capture_gps;
          notify_gps_source = capture_gps_source;
        } else {
          session_id = 0;
          record_seq = 0;
        }
      }
    }
    storage_unlock();
  }

  if (persisted && (s_replay_active || s_blob_active)) {
    return;
  }

  uint8_t source_flags = persisted ? WG_SIGHTING_SOURCE_LIVE : 0;
  const gps_fix_t *gps_for_notify = notify_gps.valid ? &notify_gps : NULL;
  (void)notify_sighting_frame_ex(entry, flags, node_id, session_id, record_seq, source_flags,
                                 gps_for_notify, notify_gps_source);
}

static void channel_hopper_cb(void *arg) {
  (void)arg;
  uint8_t next = s_current_channel;
  if (!wg_channel_next(s_local_channel_mask, s_current_channel, &next)) {
    return;
  }
  s_current_channel = next;
  esp_wifi_set_channel(s_current_channel, WIFI_SECOND_CHAN_NONE);
}

static void status_tick_once(void) {
  rate_counter_tick(now_ms());
  unique_bssid_refresh_estimate();
  update_die_temp_sample();
  storage_gpx_tick();
  (void)storage_flush_pending(false);
  storage_refresh_backlog(false);
  if (s_node_status.link_up && (now_ms() - s_node_status.last_rx_ms) > WG_NODE_STALE_MS) {
    s_node_status.link_up = false;
    ESP_LOGW(TAG, "node link stale (status tick)");
  }
  log_bridge_diagnostics(false);
  notify_status_frame();
  notify_gps_frame();
}

static void status_task(void *arg) {
  (void)arg;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    status_tick_once();
  }
}

static void replay_task(void *arg) {
  (void)arg;
  TickType_t delay_ticks = pdMS_TO_TICKS(WG_REPLAY_TASK_INTERVAL_MS);
  if (delay_ticks < 1) {
    delay_ticks = 1;
  }
  while (true) {
    (void)ulTaskNotifyTake(pdTRUE, delay_ticks);
    for (uint8_t rounds = 0; rounds < WG_REPLAY_TASK_PUMP_ROUNDS; ++rounds) {
      bool progressed = false;
      progressed = storage_run_blob_tick() || progressed;
      progressed = storage_run_replay_tick() || progressed;
      if (!progressed) {
        break;
      }
      taskYIELD();
    }
  }
}

static esp_err_t start_scan(void) {
  scan_state_lock();
  esp_err_t rc = ESP_OK;
  if (!s_latest_gps.valid) {
    ESP_LOGW(TAG, "Scan start blocked: no valid GPS fix");
    rc = ESP_ERR_INVALID_STATE;
    goto out;
  }
  if (s_wifi_dump_requested || s_wifi_dump_active) {
    ESP_LOGW(TAG, "Scan start blocked: Wi-Fi dump mode is active");
    rc = ESP_ERR_INVALID_STATE;
    goto out;
  }
  if (s_scanning) {
    rc = ESP_OK;
    goto out;
  }

  storage_set_replay_enabled(false);
  storage_set_blob_enabled(false);
  seed_ap_cache_from_active_scan();
  if (s_storage_ready && !storage_open_session()) {
    ESP_LOGW(TAG, "Scan start blocked: storage session open failed");
    rc = ESP_FAIL;
    goto out;
  }

  s_current_channel = pick_first_channel(s_local_channel_mask);
  rc = esp_wifi_set_channel(s_current_channel, WIFI_SECOND_CHAN_NONE);
  if (rc != ESP_OK) {
    ESP_LOGW(TAG, "Scan start failed: set channel rc=%s", esp_err_to_name(rc));
    if (s_storage_ready) {
      storage_close_session(WG_SESSION_STATE_ABORTED);
    }
    goto out;
  }
  rc = esp_wifi_set_promiscuous(true);
  if (rc != ESP_OK) {
    ESP_LOGW(TAG, "Scan start failed: enable promiscuous rc=%s", esp_err_to_name(rc));
    if (s_storage_ready) {
      storage_close_session(WG_SESSION_STATE_ABORTED);
    }
    goto out;
  }

  if (!s_hop_timer_started) {
    rc = esp_timer_start_periodic(s_hop_timer, (uint64_t)s_hop_ms * 1000ULL);
    if (rc != ESP_OK) {
      ESP_LOGW(TAG, "Scan start failed: hop timer start rc=%s", esp_err_to_name(rc));
      (void)esp_wifi_set_promiscuous(false);
      if (s_storage_ready) {
        storage_close_session(WG_SESSION_STATE_ABORTED);
      }
      goto out;
    }
    s_hop_timer_started = true;
  }
  s_scanning = true;
  led_sync_scan_gps_state();
  notify_status_frame();
  node_send_config();
out:
  scan_state_unlock();
  return rc;
}

static esp_err_t stop_scan(void) {
  scan_state_lock();
  esp_err_t rc = ESP_OK;
  if (!s_scanning) {
    goto out;
  }
  bool timer_stopped = false;
  if (s_hop_timer_started) {
    esp_err_t timer_rc = esp_timer_stop(s_hop_timer);
    if (timer_rc == ESP_OK || timer_rc == ESP_ERR_INVALID_STATE) {
      s_hop_timer_started = false;
      timer_stopped = true;
    } else {
      ESP_LOGW(TAG, "Scan stop warning: hop timer stop rc=%s", esp_err_to_name(timer_rc));
    }
  }

  rc = esp_wifi_set_promiscuous(false);
  if (rc != ESP_OK) {
    ESP_LOGW(TAG, "Scan stop failed: disable promiscuous rc=%s", esp_err_to_name(rc));
    if (timer_stopped && !s_hop_timer_started) {
      esp_err_t restart_rc = esp_timer_start_periodic(s_hop_timer, (uint64_t)s_hop_ms * 1000ULL);
      if (restart_rc == ESP_OK) {
        s_hop_timer_started = true;
      } else {
        ESP_LOGW(TAG, "Scan stop rollback warning: timer restart rc=%s", esp_err_to_name(restart_rc));
      }
    }
    goto out;
  }

  s_scanning = false;
  if (s_storage_ready) {
    storage_close_session(WG_SESSION_STATE_CLOSED);
  }
  led_sync_scan_gps_state();
  notify_status_frame();
  node_send_config();
out:
  scan_state_unlock();
  return rc;
}

static void save_runtime_config(void) {
  nvs_handle_t h = 0;
  if (nvs_open("wifinder", NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGW(TAG, "nvs open failed for save");
    return;
  }
  nvs_set_u16(h, "hop_ms", s_hop_ms);
  nvs_set_u16(h, "chanmask", s_local_channel_mask);
  nvs_set_u16(h, "chanmask_l", s_local_channel_mask);
  nvs_set_u16(h, "chanmask_n", s_node_channel_mask);
  nvs_set_u64(h, "chanmask5n", s_node_channel_mask_5ghz);
  nvs_set_u8(h, "bootmode", s_boot_mode);
  nvs_commit(h);
  nvs_close(h);
}

static void load_runtime_config(void) {
  s_hop_ms = WG_DEFAULT_HOP_MS;
  s_channel_mask = WG_DEFAULT_CHANNEL_MASK;
  s_boot_mode = WG_BOOT_MANUAL;
  s_node_channel_mask_5ghz = WG_NODE_5GHZ_MASK_ALL;
  split_channel_mask(s_channel_mask, &s_local_channel_mask, &s_node_channel_mask);

  nvs_handle_t h = 0;
  if (nvs_open("wifinder", NVS_READONLY, &h) != ESP_OK) {
    apply_channel_plan(s_local_channel_mask, s_node_channel_mask, s_node_channel_mask_5ghz);
    return;
  }

  uint16_t hop_ms = 0;
  uint16_t legacy_channel_mask = 0;
  uint16_t local_channel_mask = 0;
  uint16_t node_channel_mask = 0;
  uint64_t node_channel_mask_5ghz = 0;
  uint8_t boot_mode = 0;
  bool have_local_channel_mask = false;
  bool have_node_channel_mask = false;
  bool have_node_channel_mask_5ghz = false;

  if (nvs_get_u16(h, "hop_ms", &hop_ms) == ESP_OK && hop_ms >= WG_MIN_HOP_MS &&
      hop_ms <= WG_MAX_HOP_MS) {
    s_hop_ms = hop_ms;
  }
  if (nvs_get_u16(h, "chanmask_l", &local_channel_mask) == ESP_OK &&
      wg_channel_mask_is_valid(local_channel_mask)) {
    have_local_channel_mask = true;
  }
  if (nvs_get_u16(h, "chanmask_n", &node_channel_mask) == ESP_OK &&
      node_channel_mask_24_is_valid(node_channel_mask)) {
    have_node_channel_mask = true;
  }
  if (nvs_get_u64(h, "chanmask5n", &node_channel_mask_5ghz) == ESP_OK &&
      node_channel_mask_5ghz_is_valid(node_channel_mask_5ghz)) {
    have_node_channel_mask_5ghz = true;
  }
  if (nvs_get_u16(h, "chanmask", &legacy_channel_mask) == ESP_OK &&
      wg_channel_mask_is_valid(legacy_channel_mask)) {
    s_channel_mask = legacy_channel_mask;
  }
  if (nvs_get_u8(h, "bootmode", &boot_mode) == ESP_OK &&
      (boot_mode == WG_BOOT_MANUAL || boot_mode == WG_BOOT_AUTO)) {
    s_boot_mode = boot_mode;
  }
  nvs_close(h);

  if (have_local_channel_mask && have_node_channel_mask && have_node_channel_mask_5ghz &&
      node_channel_plan_enabled(node_channel_mask, node_channel_mask_5ghz)) {
    apply_channel_plan(local_channel_mask, node_channel_mask, node_channel_mask_5ghz);
  } else {
    split_channel_mask(s_channel_mask, &s_local_channel_mask, &s_node_channel_mask);
    apply_channel_plan(s_local_channel_mask, s_node_channel_mask, s_node_channel_mask_5ghz);
  }
  ESP_LOGI(TAG,
           "config loaded hop=%u local_mask=0x%04X node_mask24=0x%04X node_mask5=0x%011llX boot=%u",
           (unsigned)s_hop_ms, (unsigned)s_local_channel_mask, (unsigned)s_node_channel_mask,
           (unsigned long long)s_node_channel_mask_5ghz, (unsigned)s_boot_mode);
}

static void send_snapshot(void) {
  if (!s_sighting_notify_enabled && !s_host_serial_active) {
    send_snapshot_end();
    return;
  }
  const gps_fix_t *snapshot_gps = s_latest_gps.valid ? &s_latest_gps : NULL;
  uint8_t snapshot_gps_source = s_latest_gps.valid ? (uint8_t)s_gps_source : 0;
  for (int i = 0; i < WG_SEEN_CAPACITY; ++i) {
    if (!s_seen[i].in_use) {
      continue;
    }
    notify_sighting_frame_ex(&s_seen[i], WG_SIGHTING_FLAG_SNAPSHOT, 0, 0, 0, 0, snapshot_gps,
                             snapshot_gps_source);
  }
  send_snapshot_end();
}

static uint8_t apply_command(const wg_command_t *cmd) {
  switch (cmd->id) {
    case WG_CMD_START:
      if (!s_latest_gps.valid) {
        return WG_ERR_BAD_ARG;
      }
      if (s_wifi_dump_requested || s_wifi_dump_active) {
        return WG_ERR_BUSY;
      }
      wg_scan_policy_on_explicit_start(&s_auto_scan_paused);
      return start_scan() == ESP_OK ? WG_ACK_OK : WG_ERR_INTERNAL;
    case WG_CMD_STOP:
      wg_scan_policy_on_manual_stop(s_boot_mode, &s_auto_scan_paused);
      return stop_scan() == ESP_OK ? WG_ACK_OK : WG_ERR_INTERNAL;
    case WG_CMD_SET_HOP_MS:
      if (cmd->hop_ms < WG_MIN_HOP_MS || cmd->hop_ms > WG_MAX_HOP_MS) {
        return WG_ERR_BAD_ARG;
      }
      s_hop_ms = cmd->hop_ms;
      if (s_hop_timer_started) {
        esp_timer_stop(s_hop_timer);
        esp_timer_start_periodic(s_hop_timer, (uint64_t)s_hop_ms * 1000ULL);
      }
      save_runtime_config();
      notify_status_frame();
      node_send_config();
      return WG_ACK_OK;
    case WG_CMD_SET_CHANNEL_MASK:
      if (!wg_channel_mask_is_valid(cmd->channel_mask)) {
        return WG_ERR_BAD_ARG;
      }
      s_channel_mask = cmd->channel_mask;
      split_channel_mask(s_channel_mask, &s_local_channel_mask, &s_node_channel_mask);
      apply_channel_plan(s_local_channel_mask, s_node_channel_mask, s_node_channel_mask_5ghz);
      if (s_scanning) {
        s_current_channel = pick_first_channel(s_local_channel_mask);
        esp_wifi_set_channel(s_current_channel, WIFI_SECOND_CHAN_NONE);
      }
      save_runtime_config();
      notify_status_frame();
      node_send_config();
      return WG_ACK_OK;
    case WG_CMD_SET_CHANNEL_PLAN:
      if (!wg_channel_mask_is_valid(cmd->local_channel_mask) ||
          !node_channel_mask_24_is_valid(cmd->node_channel_mask) ||
          !node_channel_mask_5ghz_is_valid(cmd->node_channel_mask_5ghz)) {
        return WG_ERR_BAD_ARG;
      }
      if (!node_channel_plan_enabled(cmd->node_channel_mask, cmd->node_channel_mask_5ghz)) {
        return WG_ERR_BAD_ARG;
      }
      apply_channel_plan(cmd->local_channel_mask, cmd->node_channel_mask, cmd->node_channel_mask_5ghz);
      if (s_scanning) {
        s_current_channel = pick_first_channel(s_local_channel_mask);
        esp_wifi_set_channel(s_current_channel, WIFI_SECOND_CHAN_NONE);
      }
      save_runtime_config();
      notify_status_frame();
      node_send_config();
      return WG_ACK_OK;
    case WG_CMD_SET_BOOT_MODE:
      if (cmd->boot_mode != WG_BOOT_MANUAL && cmd->boot_mode != WG_BOOT_AUTO) {
        return WG_ERR_BAD_ARG;
      }
      s_boot_mode = cmd->boot_mode;
      wg_scan_policy_on_boot_mode_set(s_boot_mode, &s_auto_scan_paused);
      save_runtime_config();
      notify_status_frame();
      return WG_ACK_OK;
    case WG_CMD_REQUEST_STATUS:
      notify_status_frame();
      return WG_ACK_OK;
    case WG_CMD_REQUEST_SNAPSHOT:
      send_snapshot();
      return WG_ACK_OK;
    case WG_CMD_REPLAY_ACK:
      if (cmd->replay_session_id == 0 || cmd->replay_highest_seq == 0) {
        return WG_ERR_BAD_ARG;
      }
      return storage_apply_replay_ack(cmd->replay_session_id, cmd->replay_highest_seq) ? WG_ACK_OK
                                                                                       : WG_ERR_INTERNAL;
    case WG_CMD_SET_REPLAY:
      if (cmd->replay_enable > 1) {
        return WG_ERR_BAD_ARG;
      }
      if (cmd->replay_enable == 1 && (s_wifi_dump_requested || s_wifi_dump_active)) {
        return WG_ERR_BUSY;
      }
      if (cmd->replay_enable == 1 && s_scanning) {
        esp_err_t stop_rc = stop_scan();
        if (stop_rc != ESP_OK) {
          ESP_LOGW(TAG, "set replay failed: stop scan rc=%s", esp_err_to_name(stop_rc));
          return WG_ERR_INTERNAL;
        }
      }
      storage_set_replay_enabled(cmd->replay_enable == 1);
      notify_status_frame();
      return WG_ACK_OK;
    case WG_CMD_SET_BACKLOG_BLOB:
      if (cmd->backlog_blob_enable > 1) {
        return WG_ERR_BAD_ARG;
      }
      if (cmd->backlog_blob_enable == 1 && (s_wifi_dump_requested || s_wifi_dump_active)) {
        return WG_ERR_BUSY;
      }
      if (cmd->backlog_blob_enable == 1 && s_scanning) {
        esp_err_t stop_rc = stop_scan();
        if (stop_rc != ESP_OK) {
          ESP_LOGW(TAG, "set backlog blob failed: stop scan rc=%s", esp_err_to_name(stop_rc));
          return WG_ERR_INTERNAL;
        }
      }
      storage_set_blob_enabled(cmd->backlog_blob_enable == 1);
      notify_status_frame();
      return WG_ACK_OK;
    case WG_CMD_BACKLOG_BLOB_CHUNK_REPLY:
      if (cmd->backlog_blob_session_id == 0) {
        return WG_ERR_BAD_ARG;
      }
      if (cmd->backlog_blob_chunk_reply != WG_BACKLOG_BLOB_CHUNK_REPLY_ACK &&
          cmd->backlog_blob_chunk_reply != WG_BACKLOG_BLOB_CHUNK_REPLY_NAK) {
        return WG_ERR_BAD_ARG;
      }
      if (cmd->backlog_blob_chunk_reply == WG_BACKLOG_BLOB_CHUNK_REPLY_ACK &&
          cmd->backlog_blob_chunk_len == 0) {
        return WG_ERR_BAD_ARG;
      }
      return storage_apply_blob_chunk_reply(cmd->backlog_blob_session_id,
                                            cmd->backlog_blob_chunk_offset,
                                            cmd->backlog_blob_chunk_len,
                                            cmd->backlog_blob_chunk_reply)
                 ? WG_ACK_OK
                 : WG_ERR_INTERNAL;
    case WG_CMD_SET_WIFI_DUMP:
      if (cmd->wifi_dump_enable > 1) {
        return WG_ERR_BAD_ARG;
      }
      if (cmd->wifi_dump_enable == 1 && s_scanning) {
        esp_err_t stop_rc = stop_scan();
        if (stop_rc != ESP_OK) {
          ESP_LOGW(TAG, "set wifi dump failed: stop scan rc=%s", esp_err_to_name(stop_rc));
          return WG_ERR_INTERNAL;
        }
      }
      return storage_set_wifi_dump_enabled(cmd->wifi_dump_enable == 1) ? WG_ACK_OK
                                                                        : WG_ERR_INTERNAL;
    case WG_CMD_COMMIT_WIFI_DUMP:
      if (cmd->wifi_dump_run_id == 0) {
        return WG_ERR_BAD_ARG;
      }
      return storage_commit_wifi_dump_run(cmd->wifi_dump_run_id) ? WG_ACK_OK
                                                                  : WG_ERR_INTERNAL;
    case WG_CMD_DEBUG_SEED_STORAGE: {
      if (s_scanning) {
        return WG_ERR_BUSY;
      }
      uint32_t records_added = 0;
      uint32_t bytes_added = 0;
      if (!storage_seed_synthetic(cmd->debug_seed_target_bytes, &records_added, &bytes_added)) {
        return WG_ERR_INTERNAL;
      }
      ESP_LOGI(TAG, "debug seed added records=%lu bytes=%lu target=%lu", (unsigned long)records_added,
               (unsigned long)bytes_added, (unsigned long)cmd->debug_seed_target_bytes);
      notify_status_frame();
      return WG_ACK_OK;
    }
    case WG_CMD_CLEAR_STORAGE:
      if (s_scanning || s_wifi_dump_requested || s_wifi_dump_active) {
        return WG_ERR_BUSY;
      }
      if (!storage_clear_sessions()) {
        return WG_ERR_INTERNAL;
      }
      notify_status_frame();
      return WG_ACK_OK;
    case WG_CMD_SET_GPS_NAV_RATE: {
      if (!gps_nav_mode_is_valid(cmd->gps_nav_mode)) {
        return WG_ERR_BAD_ARG;
      }
      s_gps_nav_mode = cmd->gps_nav_mode;
      uint8_t current_hz = s_gps_nav_rate_hz > 0 ? s_gps_nav_rate_hz : 1;
      uint8_t desired_hz = gps_target_nav_rate_hz(current_hz, &s_uart_gps, s_gps_nav_mode);
      if (!gps_apply_nav_profile(desired_hz, false)) {
        return WG_ERR_INTERNAL;
      }
      s_gps_nav_rate_hz = desired_hz;
      s_gps_last_rate_change_ms = now_ms();
      ESP_LOGI(TAG, "GPS nav mode set: %s (%uHz)", gps_nav_mode_name(s_gps_nav_mode),
               (unsigned)s_gps_nav_rate_hz);
      return WG_ACK_OK;
    }
    case WG_CMD_SET_GPS_FIX: {
      const uint8_t known_flag_mask = WG_GPS_FLAG_VALID | WG_GPS_FLAG_HAS_ALT | WG_GPS_FLAG_HAS_SPEED |
                                      WG_GPS_FLAG_HAS_BEARING;
      if ((cmd->gps_flags & (uint8_t)(~known_flag_mask)) != 0) {
        return WG_ERR_BAD_ARG;
      }
      if ((cmd->gps_flags & WG_GPS_FLAG_VALID) == 0) {
        memset(&s_phone_gps, 0, sizeof(s_phone_gps));
        recompute_effective_gps_fix();
        node_send_config();
        return WG_ACK_OK;
      }
      if (cmd->gps_lat_e7 < -900000000 || cmd->gps_lat_e7 > 900000000 ||
          cmd->gps_lon_e7 < -1800000000 || cmd->gps_lon_e7 > 1800000000 ||
          cmd->gps_accuracy_cm == 0) {
        return WG_ERR_BAD_ARG;
      }

      s_phone_gps.valid = true;
      s_phone_gps.flags = cmd->gps_flags;
      s_phone_gps.lat_e7 = cmd->gps_lat_e7;
      s_phone_gps.lon_e7 = cmd->gps_lon_e7;
      s_phone_gps.alt_mm = (cmd->gps_flags & WG_GPS_FLAG_HAS_ALT) ? cmd->gps_alt_mm : 0;
      s_phone_gps.speed_mmps =
          (cmd->gps_flags & WG_GPS_FLAG_HAS_SPEED) ? cmd->gps_speed_mmps : 0;
      s_phone_gps.bearing_mdeg =
          (cmd->gps_flags & WG_GPS_FLAG_HAS_BEARING) ? cmd->gps_bearing_mdeg : 0;
      s_phone_gps.unix_time_s = cmd->gps_unix_time_s;
      s_phone_gps.accuracy_cm = cmd->gps_accuracy_cm;
      s_phone_gps.sat_in_use = 0;
      s_phone_gps.sat_in_view = 0;
      s_phone_gps.sat_count = 0;
      s_phone_gps.hdop_centi = 0;
      s_phone_gps.pdop_centi = 0;
      s_phone_gps.vdop_centi = 0;
      s_phone_gps.received_ms = now_ms();
      recompute_effective_gps_fix();
      node_send_config();
      return WG_ACK_OK;
    }
    default:
      return WG_ERR_BAD_CMD;
  }
}

static uint8_t process_control_frame(const uint8_t *data, size_t data_len, uint8_t *cmd_id_out) {
  wg_frame_t frame = {0};
  if (wg_frame_decode(data, data_len, &frame) != WG_OK) {
    *cmd_id_out = 0;
    return WG_ERR_BAD_CMD;
  }
  if (frame.version != WG_PROTOCOL_VERSION || frame.type != WG_MSG_COMMAND) {
    *cmd_id_out = 0;
    return WG_ERR_BAD_CMD;
  }

  wg_command_t cmd = {0};
  if (wg_command_decode(frame.payload, frame.len, &cmd) != WG_OK) {
    *cmd_id_out = 0;
    return WG_ERR_BAD_CMD;
  }

  *cmd_id_out = (uint8_t)cmd.id;
  return apply_command(&cmd);
}

static int append_frame_to_om(uint8_t type, const uint8_t *payload, uint16_t payload_len,
                              struct os_mbuf *om) {
  uint8_t frame[WG_FRAME_HEADER_SIZE + 64] = {0};
  if ((size_t)payload_len > sizeof(frame) - WG_FRAME_HEADER_SIZE) {
    return BLE_ATT_ERR_INSUFFICIENT_RES;
  }

  wg_frame_t out_frame = {
      .version = WG_PROTOCOL_VERSION,
      .type = type,
      .seq = s_seq++,
      .len = payload_len,
      .device_ms = ble_device_ms(),
      .payload = payload,
  };
  size_t frame_len = 0;
  if (wg_frame_encode(&out_frame, frame, sizeof(frame), &frame_len) != WG_OK) {
    return BLE_ATT_ERR_UNLIKELY;
  }
  return os_mbuf_append(om, frame, frame_len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int handle_gatt_read(uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt) {
  if (attr_handle == s_status_handle) {
    wg_status_payload_t payload = {0};
    build_status_payload(&payload);
    uint8_t bytes[WG_STATUS_PAYLOAD_SIZE] = {0};
    const size_t len = wg_build_status_payload(&payload, bytes, sizeof(bytes));
    return append_frame_to_om(WG_MSG_STATUS, bytes, (uint16_t)len, ctxt->om);
  }

  if (attr_handle == s_config_handle) {
    uint8_t cfg_payload[5] = {0};
    cfg_payload[0] = (uint8_t)(s_hop_ms & 0xFF);
    cfg_payload[1] = (uint8_t)((s_hop_ms >> 8) & 0xFF);
    cfg_payload[2] = (uint8_t)(s_channel_mask & 0xFF);
    cfg_payload[3] = (uint8_t)((s_channel_mask >> 8) & 0xFF);
    cfg_payload[4] = s_boot_mode;
    return append_frame_to_om(WG_MSG_CONFIG, cfg_payload, sizeof(cfg_payload), ctxt->om);
  }

  if (attr_handle == s_device_info_handle) {
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *fw_ver = (app_desc != NULL && app_desc->version[0] != '\0') ? app_desc->version : "unknown";
    char info[96] = {0};
    (void)snprintf(info, sizeof(info), "%s FW %s", WG_DEVICE_NAME, fw_ver);
    return os_mbuf_append(ctxt->om, info, strlen(info)) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
  }

  return BLE_ATT_ERR_UNLIKELY;
}

static int handle_gatt_write(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt) {
  if (attr_handle != s_control_handle && attr_handle != s_config_handle) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  if (wg_ble_should_request_security_on_protected_write(s_ble_encrypted)) {
    request_link_security(conn_handle, "gatt_write");
    return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
  }

  const uint16_t incoming_len = OS_MBUF_PKTLEN(ctxt->om);
  if (incoming_len > WG_HOST_FRAME_MAX_PAYLOAD) {
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  }
  uint8_t incoming[WG_HOST_FRAME_MAX_PAYLOAD] = {0};
  uint16_t copied = 0;
  if (ble_hs_mbuf_to_flat(ctxt->om, incoming, sizeof(incoming), &copied) != 0) {
    return BLE_ATT_ERR_UNLIKELY;
  }
  led_note_bt_rx();

  uint8_t cmd_id = 0;
  const uint8_t result = process_control_frame(incoming, copied, &cmd_id);
  send_ack(cmd_id, result);
  return result == WG_ACK_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
}

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
  (void)conn_handle;
  (void)arg;

  switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
      return handle_gatt_read(attr_handle, ctxt);
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
      return handle_gatt_write(conn_handle, attr_handle, ctxt);
    default:
      return BLE_ATT_ERR_UNLIKELY;
  }
}

static void ble_advertise(void) {
  struct ble_hs_adv_fields fields;
  memset(&fields, 0, sizeof(fields));
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = (ble_uuid128_t *)&WG_SERVICE_UUID;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gap_adv_set_fields failed rc=%d", rc);
    return;
  }

  struct ble_hs_adv_fields rsp_fields;
  memset(&rsp_fields, 0, sizeof(rsp_fields));
  rsp_fields.name = (const uint8_t *)WG_DEVICE_NAME;
  rsp_fields.name_len = strlen(WG_DEVICE_NAME);
  rsp_fields.name_is_complete = 1;
  rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed rc=%d", rc);
    return;
  }

  struct ble_gap_adv_params adv_params;
  memset(&adv_params, 0, sizeof(adv_params));
  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

  rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event_cb,
                         NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gap_adv_start failed rc=%d", rc);
  } else {
    ESP_LOGI(TAG, "BLE advertising started");
  }
}

static void refresh_link_security_state(uint16_t conn_handle) {
  struct ble_gap_conn_desc desc;
  const int rc = ble_gap_conn_find(conn_handle, &desc);
  if (rc != 0) {
    s_ble_encrypted = false;
    ESP_LOGW(TAG, "ble_gap_conn_find failed rc=%d", rc);
    return;
  }

  s_ble_encrypted = desc.sec_state.encrypted;
  ESP_LOGI(TAG, "BLE link security encrypted=%d authenticated=%d bonded=%d",
           desc.sec_state.encrypted, desc.sec_state.authenticated, desc.sec_state.bonded);
}

static const char *ble_phy_name(uint8_t phy) {
  switch (phy) {
    case BLE_GAP_LE_PHY_1M:
      return "1M";
    case BLE_GAP_LE_PHY_2M:
      return "2M";
    case BLE_GAP_LE_PHY_CODED:
      return "CODED";
    default:
      return "unknown";
  }
}

static uint16_t ble_notify_frame_payload_limit(void) {
  uint16_t mtu = s_ble_att_mtu;
  if (mtu < 23) {
    mtu = 23;
  }
  if (mtu <= (WG_FRAME_HEADER_SIZE + 3)) {
    return 0;
  }
  uint16_t limit = (uint16_t)(mtu - WG_FRAME_HEADER_SIZE - 3);
  if (limit > WG_BLE_NOTIFY_FRAME_MAX_PAYLOAD_MAX) {
    limit = WG_BLE_NOTIFY_FRAME_MAX_PAYLOAD_MAX;
  }
  // Cap payload so the entire BLE notification fits within 2 LL PDU fragments.
  // Some Android BLE stacks silently fail to reassemble L2CAP PDUs spanning 3+
  // LL fragments.  With DLE=251 and encryption (4-byte MIC), each fragment
  // carries at most 247 bytes of L2CAP data.  Two fragments = 494 bytes L2CAP
  // → 490 ATT PDU → 487 notification value → 475 WG frame payload.
  const uint16_t dle = WG_BLE_DATA_LEN_OCTETS;
  const uint16_t mic = s_ble_encrypted ? 4 : 0;
  const uint16_t l2cap_per_frag = (dle > mic) ? (uint16_t)(dle - mic) : 1;
  const uint16_t safe_l2cap = (uint16_t)(l2cap_per_frag * 2U);
  const uint16_t safe_att = (safe_l2cap > 4) ? (uint16_t)(safe_l2cap - 4) : 0;
  const uint16_t safe_notify = (safe_att > 3) ? (uint16_t)(safe_att - 3) : 0;
  const uint16_t safe_payload = (safe_notify > WG_FRAME_HEADER_SIZE)
                                    ? (uint16_t)(safe_notify - WG_FRAME_HEADER_SIZE)
                                    : 0;
  if (limit > safe_payload) {
    limit = safe_payload;
  }
  return limit;
}

static void refresh_ble_link_metrics(uint16_t conn_handle, const char *reason_tag) {
  struct ble_gap_conn_desc desc;
  const int rc = ble_gap_conn_find(conn_handle, &desc);
  if (rc != 0) {
    ESP_LOGW(TAG, "%s: ble_gap_conn_find failed rc=%d", reason_tag, rc);
    return;
  }
  s_ble_conn_itvl_1p25ms = desc.conn_itvl;
  s_ble_conn_latency = desc.conn_latency;
  s_ble_conn_supervision_10ms = desc.supervision_timeout;
  s_ble_att_mtu = ble_att_mtu(conn_handle);
  ESP_LOGI(TAG,
           "%s: conn itvl=%u(%.2fms) latency=%u timeout=%u(%ums) mtu=%u phy=%s/%s dle tx=%u/%uus "
           "rx=%u/%uus",
           reason_tag, (unsigned)s_ble_conn_itvl_1p25ms, (double)s_ble_conn_itvl_1p25ms * 1.25,
           (unsigned)s_ble_conn_latency, (unsigned)s_ble_conn_supervision_10ms,
           (unsigned)(s_ble_conn_supervision_10ms * 10U), (unsigned)s_ble_att_mtu,
           ble_phy_name(s_ble_tx_phy), ble_phy_name(s_ble_rx_phy), (unsigned)s_ble_max_tx_octets,
           (unsigned)s_ble_max_tx_time_us, (unsigned)s_ble_max_rx_octets,
           (unsigned)s_ble_max_rx_time_us);
}

static void request_fast_ble_conn_params(uint16_t conn_handle) {
  struct ble_gap_upd_params params;
  memset(&params, 0, sizeof(params));
  params.itvl_min = WG_BLE_CONN_ITVL_MIN_1P25MS;
  params.itvl_max = WG_BLE_CONN_ITVL_MAX_1P25MS;
  params.latency = WG_BLE_CONN_LATENCY;
  params.supervision_timeout = WG_BLE_CONN_SUPERVISION_TIMEOUT_10MS;
  const int rc = ble_gap_update_params(conn_handle, &params);
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    ESP_LOGW(TAG, "ble_gap_update_params failed rc=%d", rc);
  }
}

static void request_fast_ble_phy_data_len(uint16_t conn_handle) {
  int rc = ble_gap_set_prefered_le_phy(conn_handle, BLE_GAP_LE_PHY_1M_MASK,
                                       BLE_GAP_LE_PHY_1M_MASK, BLE_GAP_LE_PHY_CODED_ANY);
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    ESP_LOGW(TAG, "ble_gap_set_prefered_le_phy failed rc=%d", rc);
  } else {
    ESP_LOGI(TAG, "ble_gap_set_prefered_le_phy requested 1M");
  }

  rc = ble_gap_set_data_len(conn_handle, WG_BLE_DATA_LEN_OCTETS, WG_BLE_DATA_LEN_TIME_US);
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    ESP_LOGW(TAG, "ble_gap_set_data_len failed rc=%d", rc);
  } else {
    ESP_LOGI(TAG, "ble_gap_set_data_len requested tx_octets=%u tx_time=%u",
             (unsigned)WG_BLE_DATA_LEN_OCTETS, (unsigned)WG_BLE_DATA_LEN_TIME_US);
  }
}

static void clear_peer_bond_for_conn(uint16_t conn_handle, const char *reason_tag) {
  struct ble_gap_conn_desc desc;
  const int rc = ble_gap_conn_find(conn_handle, &desc);
  if (rc != 0) {
    ESP_LOGW(TAG, "%s: ble_gap_conn_find failed rc=%d", reason_tag, rc);
    return;
  }
  const int del_rc = ble_store_util_delete_peer(&desc.peer_id_addr);
  if (del_rc != 0) {
    ESP_LOGW(TAG, "%s: ble_store_util_delete_peer failed rc=%d", reason_tag, del_rc);
  } else {
    ESP_LOGW(TAG, "%s: deleted peer bond", reason_tag);
  }
}

static void log_ble_store_counts(const char *tag) {
  int peer_sec = 0;
  int our_sec = 0;
  int cccd = 0;
  const int peer_rc = ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &peer_sec);
  const int our_rc = ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &our_sec);
  const int cccd_rc = ble_store_util_count(BLE_STORE_OBJ_TYPE_CCCD, &cccd);
  ESP_LOGI(TAG,
           "%s: ble_store counts peer_sec=%d(rc=%d) our_sec=%d(rc=%d) cccd=%d(rc=%d)",
           tag, peer_sec, peer_rc, our_sec, our_rc, cccd, cccd_rc);
}

static void request_link_security(uint16_t conn_handle, const char *reason_tag) {
  const int rc = ble_gap_security_initiate(conn_handle);
  if (wg_ble_security_initiate_is_ok(rc)) {
    ESP_LOGI(TAG, "%s: ble_gap_security_initiate requested/in-progress", reason_tag);
    return;
  }
  if (wg_ble_security_initiate_is_busy(rc)) {
    ESP_LOGW(TAG,
             "%s: ble_gap_security_initiate busy; will retry on next secure operation",
             reason_tag);
    return;
  }
  ESP_LOGW(TAG, "%s: ble_gap_security_initiate failed rc=%d", reason_tag, rc);
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
  (void)arg;
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      ESP_LOGI(TAG, "BLE connect event status=%d", event->connect.status);
      if (event->connect.status == 0) {
        s_conn_handle = event->connect.conn_handle;
        s_security_retry_attempted = false;
        s_ble_tx_phy = 0;
        s_ble_rx_phy = 0;
        s_ble_max_tx_octets = 0;
        s_ble_max_tx_time_us = 0;
        s_ble_max_rx_octets = 0;
        s_ble_max_rx_time_us = 0;
        log_ble_store_counts("connect");
        refresh_link_security_state(s_conn_handle);
        request_fast_ble_conn_params(s_conn_handle);
        request_fast_ble_phy_data_len(s_conn_handle);
        refresh_ble_link_metrics(s_conn_handle, "connect");
        led_sync_link_state();
        if (!s_ble_encrypted) {
          ESP_LOGI(TAG, "connect: deferring security initiation until subscribe/write");
        }
        notify_status_frame();
      } else {
        led_sync_link_state();
        ble_advertise();
      }
      return 0;
    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGI(TAG, "BLE disconnect reason=%d", event->disconnect.reason);
      s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
      s_ble_encrypted = false;
      s_security_retry_attempted = false;
      s_ble_conn_itvl_1p25ms = 0;
      s_ble_conn_latency = 0;
      s_ble_conn_supervision_10ms = 0;
      s_ble_att_mtu = 0;
      s_ble_tx_phy = 0;
      s_ble_rx_phy = 0;
      s_ble_max_tx_octets = 0;
      s_ble_max_tx_time_us = 0;
      s_ble_max_rx_octets = 0;
      s_ble_max_rx_time_us = 0;
      s_status_notify_enabled = false;
      s_gps_notify_enabled = false;
      s_sighting_notify_enabled = false;
      storage_set_replay_enabled(false);
      storage_set_blob_enabled(false);
      led_sync_link_state();
      ble_advertise();
      return 0;
    case BLE_GAP_EVENT_CONN_UPDATE:
      ESP_LOGI(TAG, "BLE conn update status=%d", event->conn_update.status);
      if (event->conn_update.status == 0) {
        refresh_ble_link_metrics(event->conn_update.conn_handle, "conn_update");
      }
      return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
      ESP_LOGI(TAG, "BLE encryption change status=%d", event->enc_change.status);
      if (event->enc_change.status == 0) {
        refresh_link_security_state(event->enc_change.conn_handle);
        refresh_ble_link_metrics(event->enc_change.conn_handle, "enc_change");
        s_security_retry_attempted = false;
      } else {
        s_ble_encrypted = false;
        if (!s_security_retry_attempted) {
          s_security_retry_attempted = true;
          if (wg_ble_enc_change_status_should_drop_bond(event->enc_change.status)) {
            clear_peer_bond_for_conn(event->enc_change.conn_handle, "enc_change");
          } else {
            ESP_LOGW(TAG, "enc_change: keeping peer bond status=%d", event->enc_change.status);
          }
          if (wg_ble_enc_change_status_should_retry(event->enc_change.status)) {
            request_link_security(event->enc_change.conn_handle, "enc_change_retry");
          } else {
            ESP_LOGW(TAG, "enc_change: not retrying security status=%d", event->enc_change.status);
          }
        }
      }
      led_sync_link_state();
      notify_status_frame();
      return 0;
    case BLE_GAP_EVENT_MTU:
      s_ble_att_mtu = event->mtu.value;
      ESP_LOGI(TAG, "BLE MTU updated channel=%u mtu=%u", (unsigned)event->mtu.channel_id,
               (unsigned)event->mtu.value);
      refresh_ble_link_metrics(event->mtu.conn_handle, "mtu");
      return 0;
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
      if (event->phy_updated.status == 0) {
        s_ble_tx_phy = event->phy_updated.tx_phy;
        s_ble_rx_phy = event->phy_updated.rx_phy;
        ESP_LOGI(TAG, "BLE PHY update tx=%s rx=%s", ble_phy_name(s_ble_tx_phy),
                 ble_phy_name(s_ble_rx_phy));
        refresh_ble_link_metrics(event->phy_updated.conn_handle, "phy_update");
      } else {
        ESP_LOGW(TAG, "BLE PHY update failed status=%d", event->phy_updated.status);
      }
      return 0;
    case BLE_GAP_EVENT_DATA_LEN_CHG:
      s_ble_max_tx_octets = event->data_len_chg.max_tx_octets;
      s_ble_max_tx_time_us = event->data_len_chg.max_tx_time;
      s_ble_max_rx_octets = event->data_len_chg.max_rx_octets;
      s_ble_max_rx_time_us = event->data_len_chg.max_rx_time;
      ESP_LOGI(TAG, "BLE data len tx=%u/%uus rx=%u/%uus",
               (unsigned)s_ble_max_tx_octets, (unsigned)s_ble_max_tx_time_us,
               (unsigned)s_ble_max_rx_octets, (unsigned)s_ble_max_rx_time_us);
      refresh_ble_link_metrics(event->data_len_chg.conn_handle, "data_len");
      return 0;
    case BLE_GAP_EVENT_NOTIFY_TX:
      if (event->notify_tx.attr_handle == s_sighting_handle && event->notify_tx.status == 0) {
        wake_replay_task();
      }
      return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
      struct ble_sm_io pkey = {0};
      int rc = 0;
      switch (event->passkey.params.action) {
        case BLE_SM_IOACT_DISP:
          pkey.action = BLE_SM_IOACT_DISP;
          pkey.passkey = WG_BLE_PASSKEY;
          rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
          if (rc != 0) {
            ESP_LOGW(TAG, "ble_sm_inject_io(DISPLAY) failed rc=%d", rc);
          } else {
            ESP_LOGI(TAG, "BLE passkey is %06u", (unsigned int)WG_BLE_PASSKEY);
          }
          return 0;
        case BLE_SM_IOACT_NUMCMP:
          pkey.action = BLE_SM_IOACT_NUMCMP;
          pkey.numcmp_accept = 1;
          rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
          if (rc != 0) {
            ESP_LOGW(TAG, "ble_sm_inject_io(NUMCMP) failed rc=%d", rc);
          }
          return 0;
        default:
          ESP_LOGW(TAG, "Unhandled passkey action=%d", event->passkey.params.action);
          return 0;
      }
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
      if (event->subscribe.attr_handle == s_status_handle) {
        s_status_notify_enabled = event->subscribe.cur_notify;
        s_gps_notify_enabled = s_status_notify_enabled;
      } else if (event->subscribe.attr_handle == s_sighting_handle) {
        s_sighting_notify_enabled = event->subscribe.cur_notify;
        if (event->subscribe.cur_notify) {
          wake_replay_task();
        }
      }
      if (wg_ble_should_request_security_on_subscribe(event->subscribe.cur_notify,
                                                      s_ble_encrypted)) {
        request_link_security(event->subscribe.conn_handle, "subscribe");
      } else if (event->subscribe.cur_notify && !s_ble_encrypted) {
        ESP_LOGI(TAG, "subscribe: security deferred until protected write");
      }
      return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      clear_peer_bond_for_conn(event->repeat_pairing.conn_handle, "repeat_pairing");
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
      ESP_LOGI(TAG, "BLE advertising complete reason=%d", event->adv_complete.reason);
      ble_advertise();
      return 0;
    default:
      return 0;
  }
}

static void ble_on_sync(void) {
  int rc = ble_hs_util_ensure_addr(0);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed rc=%d", rc);
    return;
  }

  rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_hs_id_infer_auto failed rc=%d", rc);
    return;
  }

  uint8_t addr_val[6] = {0};
  rc = ble_hs_id_copy_addr(s_own_addr_type, addr_val, NULL);
  if (rc == 0) {
    ESP_LOGI(TAG, "BLE addr=%02x:%02x:%02x:%02x:%02x:%02x type=%u", addr_val[5], addr_val[4],
             addr_val[3], addr_val[2], addr_val[1], addr_val[0], s_own_addr_type);
  } else {
    ESP_LOGW(TAG, "ble_hs_id_copy_addr failed rc=%d", rc);
  }

  ble_advertise();
}

static void ble_on_reset(int reason) { ESP_LOGW(TAG, "BLE reset reason=%d", reason); }

static void ble_host_task(void *param) {
  (void)param;
  nimble_port_run();
  nimble_port_freertos_deinit();
}

static void init_ble(void) {
  ESP_ERROR_CHECK(nimble_port_init());
  int mtu_rc = ble_att_set_preferred_mtu(WG_BLE_ATT_PREFERRED_MTU);
  if (mtu_rc != 0) {
    ESP_LOGW(TAG, "ble_att_set_preferred_mtu(%u) failed rc=%d",
             (unsigned)WG_BLE_ATT_PREFERRED_MTU, mtu_rc);
  }

  ble_hs_cfg.reset_cb = ble_on_reset;
  ble_hs_cfg.sync_cb = ble_on_sync;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_svc_gap_device_name_set(WG_DEVICE_NAME);

  ble_gatts_count_cfg(gatt_services);
  ble_gatts_add_svcs(gatt_services);

  ble_store_config_init();
  log_ble_store_counts("init_ble");
  nimble_port_freertos_init(ble_host_task);
}

static void init_nvs(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
}

static void wifi_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT || buf == NULL || !s_scanning) {
    return;
  }
  if (!s_latest_gps.valid) {
    return;
  }

  wifi_promiscuous_pkt_t *packet = (wifi_promiscuous_pkt_t *)buf;
  if (packet->rx_ctrl.rx_state != 0) {
    return;
  }
  const uint8_t *payload = packet->payload;
  const uint16_t sig_len = packet->rx_ctrl.sig_len;
  if (sig_len < sizeof(ieee80211_mgmt_hdr_t) + 12) {
    return;
  }
  if (!wardrive_is_interesting_mgmt(payload[0])) {
    return;
  }

  s_rate_packet_acc++;
  const int64_t ts_ms = now_ms();
  rate_counter_tick(ts_ms);

  const ieee80211_mgmt_hdr_t *hdr = (const ieee80211_mgmt_hdr_t *)payload;
  char ssid[33] = {0};
  uint8_t ssid_len = 0;
  uint8_t auth_mode = WG_AUTH_UNKNOWN;
  uint8_t proto_flags = 0;
  uint8_t akm_flags = 0;
  uint8_t cipher_flags = 0;
  parse_beacon_probe(payload, sig_len, ssid, &ssid_len, &auth_mode, &proto_flags, &akm_flags,
                     &cipher_flags);

  bool is_new = false;
  bool is_updated = false;
  ap_entry_t *entry =
      upsert_ap_entry(hdr->addr3, ssid, ssid_len, auth_mode, proto_flags, akm_flags,
                      cipher_flags, packet->rx_ctrl.channel, packet->rx_ctrl.rssi, ts_ms, false,
                      &is_new, &is_updated);

  if (entry == NULL) {
    return;
  }
  if (!should_notify_entry(entry, is_new, is_updated, ts_ms)) {
    return;
  }

  uint8_t flags = 0;
  if (is_new) {
    flags |= WG_SIGHTING_FLAG_NEW;
  }
  if (is_updated) {
    flags |= WG_SIGHTING_FLAG_UPDATED;
  }

  publish_sighting_event(entry, flags, 0);
}

static void init_wifi(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  esp_netif_create_default_wifi_ap();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

  wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
  ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
  ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb));
  ESP_ERROR_CHECK(esp_wifi_set_promiscuous(false));
}

static void init_timers(void) {
  const esp_timer_create_args_t hop_args = {
      .callback = channel_hopper_cb,
      .name = "hop_timer",
  };
  if (s_scan_mutex == NULL) {
    s_scan_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_scan_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM);
  }
  ESP_ERROR_CHECK(esp_timer_create(&hop_args, &s_hop_timer));
  BaseType_t rc = xTaskCreate(status_task, "wg_status", 6144, NULL, 4, &s_status_task);
  ESP_ERROR_CHECK(rc == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
  rc = xTaskCreate(replay_task, "wg_replay", 4096, NULL, 5, &s_replay_task);
  ESP_ERROR_CHECK(rc == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void app_main(void) {
  init_nvs();
  if (s_prev_log_vprintf == NULL) {
    s_prev_log_vprintf = esp_log_set_vprintf(log_mux_vprintf);
  }
  esp_log_level_set("NimBLE", ESP_LOG_WARN);
  load_runtime_config();
  init_wifi();
  init_storage();
  init_die_temp_sensor();
  update_die_temp_sample();
  init_timers();
  init_uart_bridges();
  init_host_serial_bridge();
  init_status_led();
  init_ble();

  if (s_boot_mode == WG_BOOT_AUTO && s_latest_gps.valid) {
    start_scan();
  } else if (s_boot_mode == WG_BOOT_AUTO) {
    ESP_LOGI(TAG, "Boot auto requested, waiting for valid GPS fix before scanning");
  }

  ESP_LOGI(TAG, "BLE control ready. channel_mask=0x%04X hop_ms=%u boot_mode=%u", s_channel_mask,
           s_hop_ms, s_boot_mode);
}
