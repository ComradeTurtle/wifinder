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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/rmt_tx.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

#include "soc/soc_caps.h"

#include "channel_plan.h"
#include "sniffer_logic.h"
#include "wg_payload.h"
#include "wg_protocol.h"

extern void ble_store_config_init(void);

#if CONFIG_IDF_TARGET_ESP32C6
#define WG_DEVICE_NAME "ESPWIGLE-C6"
#define WG_DEVICE_INFO_TEXT "ESPWIGLE-C6 FW1"
#else
#define WG_DEVICE_NAME "ESPWIGLE-S3"
#define WG_DEVICE_INFO_TEXT "ESPWIGLE-S3 FW1"
#endif

#define WG_DEFAULT_HOP_MS 250
#define WG_MIN_HOP_MS 50
#define WG_MAX_HOP_MS 2000
#define WG_DEFAULT_CHANNEL_MASK 0x1FFFU
#define WG_SIGHTING_NOTIFY_INTERVAL_MS 5000
#define WG_SEEN_CAPACITY 512
#define WG_UNIQUE_BSSID_HASH_SLOTS 8192
#define WG_SCAN_SEED_MAX_APS 128
#define WG_BLE_PASSKEY 123456U
#define WG_BLE_NOTIFY_FRAME_MAX_PAYLOAD 170
#define WG_BLE_CONN_ITVL_MIN_1P25MS 6
#define WG_BLE_CONN_ITVL_MAX_1P25MS 12
#define WG_BLE_CONN_LATENCY 0
#define WG_BLE_CONN_SUPERVISION_TIMEOUT_10MS 400
_Static_assert((WG_UNIQUE_BSSID_HASH_SLOTS & (WG_UNIQUE_BSSID_HASH_SLOTS - 1)) == 0,
               "WG_UNIQUE_BSSID_HASH_SLOTS must be power-of-two");

#define WG_GPS_UART_NUM UART_NUM_1
#define WG_GPS_UART_TX_GPIO 10
#define WG_GPS_UART_RX_GPIO 11
#define WG_GPS_UART_BAUD 9600
#define WG_GPS_UART_TARGET_BAUD 38400
#define WG_GPS_MAX_AGE_MS 3000
#define WG_GPS_PHONE_MAX_AGE_MS 5000
#define WG_GPS_UART_ACCURACY_FALLBACK_CM 5000
#define WG_GPS_GSV_FRESH_MS 5000
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

#define WG_STORAGE_BASE_PATH "/spiffs"
#define WG_STORAGE_FILE_PREFIX "wq_"
#define WG_STORAGE_BLOCK_SIZE 1024
#define WG_STORAGE_BLOCK_MAGIC 0x314B4257U
#define WG_STORAGE_META_MAGIC 0x314D5157U
#define WG_STORAGE_VERSION 1
#define WG_STORAGE_BLOCK_FLUSH_MS 1200
#define WG_STORAGE_RECORD_HDR_SIZE 2
#define WG_STORAGE_REPLAY_BUDGET_PER_TICK 4
#define WG_REPLAY_TASK_INTERVAL_MS 20
#define WG_REPLAY_BATCH_MAX_RECORDS 8
#define WG_REPLAY_BATCH_HEADER_SIZE 1
#define WG_REPLAY_BATCH_RECORD_HDR_SIZE 2
#define WG_STORAGE_MIN_FREE_BYTES (WG_STORAGE_BLOCK_SIZE * 4)
#define WG_STORAGE_PERSIST_MIN_STATIONARY_MS 30000
#define WG_STORAGE_PERSIST_MIN_MOVING_MS 8000
#define WG_STORAGE_PERSIST_MAX_INTERVAL_MS 120000
#define WG_STORAGE_PERSIST_RSSI_DELTA_DB 6
#define WG_STORAGE_MOVING_SPEED_MMPS 2000

#define WG_NODE_UART_NUM LP_UART_NUM_0
#define WG_NODE_UART_TX_GPIO 5
#define WG_NODE_UART_RX_GPIO 4
#define WG_NODE_UART_BAUD 460800
#define WG_NODE_FRAME_SYNC0 0xA5
#define WG_NODE_FRAME_SYNC1 0x5A
#define WG_NODE_FRAME_VERSION 1
#define WG_NODE_FRAME_HEADER_SIZE 10
#define WG_NODE_FRAME_MAX_PAYLOAD 96
#define WG_NODE_STALE_MS 3000

#define WG_HOST_FRAME_MAX_PAYLOAD 196
#define WG_HOST_RX_BUF_SIZE (WG_FRAME_HEADER_SIZE + WG_HOST_FRAME_MAX_PAYLOAD)
#define WG_HOST_SERIAL_READ_MS 30
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
  uint8_t sat_count;
  uint16_t hdop_centi;
  uint16_t pdop_centi;
  int64_t received_ms;
} gps_fix_t;

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
  int64_t last_rx_ms;
} node_link_status_t;

typedef enum {
  WG_SESSION_STATE_OPEN = 1,
  WG_SESSION_STATE_CLOSED = 2,
  WG_SESSION_STATE_ABORTED = 3,
} wg_session_state_t;

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

static const char *TAG = "espwigle";

static ap_entry_t s_seen[WG_SEEN_CAPACITY];
static uint32_t s_unique_bssid_hashes[WG_UNIQUE_BSSID_HASH_SLOTS];
static uint16_t s_unique_bssid_count = 0;
static uint32_t s_unique_hash_table_full_events = 0;

static bool s_scanning = false;
static uint16_t s_hop_ms = WG_DEFAULT_HOP_MS;
static uint16_t s_channel_mask = WG_DEFAULT_CHANNEL_MASK;
static uint8_t s_current_channel = 1;
static uint8_t s_boot_mode = WG_BOOT_MANUAL;

static esp_timer_handle_t s_hop_timer = NULL;
static bool s_hop_timer_started = false;
static TaskHandle_t s_status_task = NULL;
static TaskHandle_t s_replay_task = NULL;

static int64_t s_rate_window_start_ms = 0;
static uint32_t s_rate_packet_acc = 0;
static uint16_t s_packets_per_sec = 0;
static uint16_t s_notify_drops = 0;
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
static volatile char s_gps_last_rmc_status = '?';
static volatile int64_t s_gps_last_gsv_ms = 0;
static volatile int s_gps_uart_baud_active = WG_GPS_UART_BAUD;
static volatile uint8_t s_gps_nav_rate_hz = 1;
static volatile uint8_t s_gps_nav_mode = WG_GPS_NAV_MODE_AUTO;
static volatile int64_t s_gps_last_rate_change_ms = 0;

static node_link_status_t s_node_status = {0};
static uint16_t s_node_seq = 1;
static uint16_t s_node_channel_mask = 0;
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

static uint16_t s_control_handle;
static uint16_t s_status_handle;
static uint16_t s_sighting_handle;
static uint16_t s_config_handle;
static uint16_t s_device_info_handle;

static bool s_host_serial_enabled = false;
static bool s_host_serial_active = false;
static bool s_host_frame_only_logs = false;
static uint32_t s_host_rx_frames = 0;
static uint32_t s_host_tx_frames = 0;
static uint32_t s_host_rx_errors = 0;
static uint8_t s_host_rx_buf[WG_HOST_RX_BUF_SIZE] = {0};
static size_t s_host_rx_pos = 0;
static size_t s_host_rx_expected = 0;

static int (*s_prev_log_vprintf)(const char *fmt, va_list args) = NULL;
static SemaphoreHandle_t s_host_tx_mutex = NULL;

static SemaphoreHandle_t s_store_mutex = NULL;
static bool s_storage_ready = false;
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
static bool storage_open_session(void);
static void storage_close_session(wg_session_state_t final_state);
static bool storage_flush_pending(bool force);
static void storage_refresh_backlog(bool reclaim);
static bool storage_apply_replay_ack(uint64_t session_id, uint32_t highest_seq);
static void storage_run_replay_tick(void);
static void storage_set_replay_enabled(bool enabled);
static bool storage_clear_sessions(void);
static bool storage_patch_sighting_source(uint8_t *payload, uint16_t payload_len, uint8_t source_flags);
static void request_fast_ble_conn_params(uint16_t conn_handle);
static void clear_peer_bond_for_conn(uint16_t conn_handle, const char *reason_tag);

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

static void host_serial_reset_rx(void) {
  s_host_rx_pos = 0;
  s_host_rx_expected = 0;
}

static void host_serial_mark_active(void) {
  if (s_host_serial_active) {
    return;
  }
  ESP_LOGI(TAG, "Host serial protocol active (frame-only stream)");
  s_host_serial_active = true;
  s_host_frame_only_logs = true;
}

static int host_serial_read_bytes(uint8_t *buf, size_t len, TickType_t wait_ticks) {
#if SOC_USB_SERIAL_JTAG_SUPPORTED
  if (!s_host_serial_enabled) {
    return -1;
  }
  return usb_serial_jtag_read_bytes(buf, len, wait_ticks);
#else
  (void)buf;
  (void)len;
  (void)wait_ticks;
  return -1;
#endif
}

static int host_serial_write_bytes(const uint8_t *buf, size_t len, TickType_t wait_ticks) {
#if SOC_USB_SERIAL_JTAG_SUPPORTED
  if (!s_host_serial_enabled) {
    return -1;
  }
  return usb_serial_jtag_write_bytes((const void *)buf, len, wait_ticks);
#else
  (void)buf;
  (void)len;
  (void)wait_ticks;
  return -1;
#endif
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

static uint32_t bssid_hash32(const uint8_t bssid[6]) {
  uint32_t h = 2166136261u;  // FNV-1a 32-bit
  for (int i = 0; i < 6; ++i) {
    h ^= (uint32_t)bssid[i];
    h *= 16777619u;
  }
  return h == 0 ? 1u : h;
}

static bool unique_bssid_track(const uint8_t bssid[6]) {
  const uint32_t mask = WG_UNIQUE_BSSID_HASH_SLOTS - 1u;
  uint32_t hash = bssid_hash32(bssid);
  uint32_t idx = hash & mask;

  for (uint32_t probe = 0; probe < WG_UNIQUE_BSSID_HASH_SLOTS; ++probe) {
    uint32_t slot = s_unique_bssid_hashes[idx];
    if (slot == hash) {
      return false;
    }
    if (slot == 0) {
      s_unique_bssid_hashes[idx] = hash;
      if (s_unique_bssid_count < UINT16_MAX) {
        s_unique_bssid_count++;
      }
      return true;
    }
    idx = (idx + 1u) & mask;
  }

  if (s_unique_hash_table_full_events < UINT32_MAX) {
    s_unique_hash_table_full_events++;
  }
  return false;
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
  s_led_state.replay_active = s_replay_active;
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

static void storage_session_paths(uint64_t session_id, char *meta_path, size_t meta_size,
                                  char *data_path, size_t data_size) {
  if (meta_path != NULL && meta_size > 0) {
    snprintf(meta_path, meta_size, WG_STORAGE_BASE_PATH "/" WG_STORAGE_FILE_PREFIX "%016llX.meta",
             (unsigned long long)session_id);
  }
  if (data_path != NULL && data_size > 0) {
    snprintf(data_path, data_size, WG_STORAGE_BASE_PATH "/" WG_STORAGE_FILE_PREFIX "%016llX.dat",
             (unsigned long long)session_id);
  }
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
  char meta_path[96] = {0};
  storage_session_paths(session_id, meta_path, sizeof(meta_path), NULL, 0);
  return storage_read_manifest_path(meta_path, out);
}

static bool storage_write_manifest(const wg_session_manifest_t *manifest_in) {
  if (manifest_in == NULL) {
    return false;
  }
  wg_session_manifest_t manifest = *manifest_in;
  manifest.magic = WG_STORAGE_META_MAGIC;
  manifest.version = WG_STORAGE_VERSION;
  manifest.size = sizeof(manifest);
  manifest.crc32 = 0;
  manifest.crc32 = crc32_ieee((const uint8_t *)&manifest, sizeof(manifest));

  char meta_path[96] = {0};
  storage_session_paths(manifest.session_id, meta_path, sizeof(meta_path), NULL, 0);
  FILE *fp = fopen(meta_path, "wb");
  if (fp == NULL) {
    ESP_LOGW(TAG, "manifest write open failed sid=%016llX errno=%d",
             (unsigned long long)manifest.session_id, errno);
    return false;
  }
  size_t written = fwrite(&manifest, 1, sizeof(manifest), fp);
  fclose(fp);
  if (written != sizeof(manifest)) {
    ESP_LOGW(TAG, "manifest write failed sid=%016llX", (unsigned long long)manifest.session_id);
    return false;
  }
  return true;
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

static void storage_refresh_backlog_locked(bool reclaim);

static uint32_t storage_reclaim_acked_sessions_locked(void) {
  DIR *dir = opendir(WG_STORAGE_BASE_PATH);
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
    char data_path[96] = {0};
    storage_session_paths(sid, meta_path, sizeof(meta_path), data_path, sizeof(data_path));
    if (!storage_read_manifest_path(meta_path, &manifest)) {
      continue;
    }
    if (manifest.state == WG_SESSION_STATE_OPEN) {
      continue;
    }
    if (manifest.records_written == 0 || manifest.records_acked < manifest.records_written) {
      continue;
    }
    if (unlink(meta_path) == 0) {
      reclaimed++;
    }
    (void)unlink(data_path);
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

  size_t total = 0;
  size_t used = 0;
  if (esp_spiffs_info("spiffs", &total, &used) != ESP_OK) {
    return false;
  }
  size_t free_bytes = (total > used) ? (total - used) : 0;
  if (free_bytes < WG_STORAGE_MIN_FREE_BYTES) {
    (void)storage_reclaim_acked_sessions_locked();
    if (esp_spiffs_info("spiffs", &total, &used) == ESP_OK) {
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
  fclose(fp);
  if (written != sizeof(block)) {
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
  s_session_open = true;
  storage_refresh_backlog_locked(false);
  ESP_LOGI(TAG, "session open sid=%016llX", (unsigned long long)s_session_id);
  return true;
}

static void storage_close_session_locked(wg_session_state_t final_state) {
  if (!s_storage_ready || !s_session_open) {
    return;
  }
  (void)storage_flush_pending_locked(true);
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
  DIR *dir = opendir(WG_STORAGE_BASE_PATH);
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
  if (!s_storage_ready || !s_replay_requested) {
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
  uint16_t max_batch_payload = WG_BLE_NOTIFY_FRAME_MAX_PAYLOAD;
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

static void storage_run_replay_tick(void) {
  if (!s_storage_ready) {
    return;
  }
  for (uint32_t budget = 0; budget < WG_STORAGE_REPLAY_BUDGET_PER_TICK; ++budget) {
    if (!storage_lock(pdMS_TO_TICKS(20))) {
      return;
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
        return;
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
    if (!storage_lock(pdMS_TO_TICKS(20))) {
      return;
    }
    storage_clear_replay_pending_message_locked();
    storage_unlock();
  }
}

static void storage_set_replay_enabled(bool enabled) {
  if (!storage_lock(pdMS_TO_TICKS(80))) {
    return;
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

  if (s_session_open) {
    storage_close_session_locked(WG_SESSION_STATE_ABORTED);
  }

  DIR *dir = opendir(WG_STORAGE_BASE_PATH);
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
    bool is_meta = name_len > 5 && strcmp(ent->d_name + name_len - 5, ".meta") == 0;
    bool is_data = name_len > 4 && strcmp(ent->d_name + name_len - 4, ".dat") == 0;
    if (!is_meta && !is_data) {
      continue;
    }
    // WG_STORAGE_BASE_PATH + '/' + max dirent filename + NUL
    char path[sizeof(WG_STORAGE_BASE_PATH) + 1 + 255 + 1] = {0};
    int path_len = snprintf(path, sizeof(path), WG_STORAGE_BASE_PATH "/%s", ent->d_name);
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
  DIR *dir = opendir(WG_STORAGE_BASE_PATH);
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
    storage_unlock();
    return true;
  }
  manifest.last_seq_acked = highest_seq;
  manifest.records_acked = highest_seq;
  if (manifest.records_acked > manifest.records_written) {
    manifest.records_acked = manifest.records_written;
  }
  if (!storage_write_manifest(&manifest)) {
    storage_unlock();
    return false;
  }
  if (use_current) {
    s_session_manifest = manifest;
  }
  if (s_replay_active && s_replay_session_id == session_id && highest_seq > s_replay_acked_seq) {
    s_replay_acked_seq = highest_seq;
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
  DIR *dir = opendir(WG_STORAGE_BASE_PATH);
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
      aborted++;
    }
  }
  closedir(dir);
  if (aborted > 0) {
    ESP_LOGW(TAG, "storage marked %u stale sessions as aborted", (unsigned)aborted);
  }
}

static void init_storage(void) {
  s_store_mutex = xSemaphoreCreateMutex();
  if (s_store_mutex == NULL) {
    ESP_LOGW(TAG, "storage disabled: mutex allocation failed");
    return;
  }
  esp_vfs_spiffs_conf_t conf = {
      .base_path = WG_STORAGE_BASE_PATH,
      .partition_label = "spiffs",
      .max_files = 8,
      .format_if_mount_failed = true,
  };
  esp_err_t rc = esp_vfs_spiffs_register(&conf);
  if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "storage disabled: spiffs mount failed rc=%s", esp_err_to_name(rc));
    return;
  }
  s_storage_ready = true;
  storage_abort_stale_open_sessions();
  storage_refresh_backlog(true);
  size_t total = 0;
  size_t used = 0;
  if (esp_spiffs_info("spiffs", &total, &used) == ESP_OK) {
    ESP_LOGI(TAG, "storage ready total=%lu used=%lu free=%lu", (unsigned long)total,
             (unsigned long)used, (unsigned long)((total > used) ? (total - used) : 0));
  } else {
    ESP_LOGI(TAG, "storage ready");
  }
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
           "unique_total=%u hash_full=%lu lat=%ld lon=%ld acc_cm=%u sats=%u hdop=%.2f pdop=%.2f "
           "gga_rej_latlon=%lu gga_rej_fix=%lu rmc_rej_status=%lu rmc_rej_latlon=%lu "
           "last_fixq=%d last_sats=%d last_rmc=%c",
           gps_source_name(s_gps_source), s_latest_gps.valid ? 1 : 0, s_gps_uart_baud_active,
           (unsigned)s_gps_nav_rate_hz, gps_nav_mode_name(s_gps_nav_mode),
           (unsigned long)s_gps_uart_rx_bytes,
           (long long)gps_rx_age_ms, (long long)gps_fix_age_ms, (long long)gps_rate_age_ms,
           (unsigned long)s_gps_nmea_lines_total,
           (unsigned long)s_gps_nmea_gga, (unsigned long)s_gps_nmea_rmc,
           (unsigned long)s_gps_nmea_gsa, (unsigned long)s_gps_nmea_gsv,
           (unsigned long)s_gps_nmea_other,
           (unsigned long)s_gps_fix_updates, (unsigned)s_unique_bssid_count,
           (unsigned long)s_unique_hash_table_full_events, (long)s_latest_gps.lat_e7,
           (long)s_latest_gps.lon_e7, s_latest_gps.accuracy_cm, (unsigned)s_latest_gps.sat_count,
           (double)s_latest_gps.hdop_centi / 100.0, (double)s_latest_gps.pdop_centi / 100.0,
           (unsigned long)s_gps_gga_reject_latlon, (unsigned long)s_gps_gga_reject_fix,
           (unsigned long)s_gps_rmc_reject_status, (unsigned long)s_gps_rmc_reject_latlon,
           s_gps_last_fix_quality, s_gps_last_sats, s_gps_last_rmc_status);

  const int64_t node_rx_age_ms =
      (s_node_status.last_rx_ms > 0) ? (ms - s_node_status.last_rx_ms) : -1;
  ESP_LOGI(TAG,
           "diag node link=%d hello=%d report=%d mask=0x%04X rx_bytes=%lu rx_frames=%lu "
           "crc_fail=%u rx_last=%s(%u) age_ms=%lld tx_frames=%lu tx_fail=%lu tx_last=%s(%u)",
           s_node_status.link_up ? 1 : 0, s_node_seen_hello ? 1 : 0,
           s_node_report_enable ? 1 : 0, s_node_channel_mask, (unsigned long)s_node_uart_rx_bytes,
           (unsigned long)s_node_rx_frames, s_node_rx_crc_failures,
           node_type_name(s_node_last_rx_type), (unsigned)s_node_last_rx_payload_len,
           (long long)node_rx_age_ms, (unsigned long)s_node_tx_frames,
           (unsigned long)s_node_tx_failures, node_type_name(s_node_last_tx_type),
           (unsigned)s_node_last_tx_payload_len);

  ESP_LOGI(TAG,
           "diag host enabled=%d active=%d frame_only=%d rx_frames=%lu tx_frames=%lu rx_errors=%lu",
           s_host_serial_enabled ? 1 : 0, s_host_serial_active ? 1 : 0,
           s_host_frame_only_logs ? 1 : 0, (unsigned long)s_host_rx_frames,
           (unsigned long)s_host_tx_frames, (unsigned long)s_host_rx_errors);

  ESP_LOGI(TAG,
           "diag store ready=%d open=%d sid=%016llX backlog_records=%lu backlog_bytes=%lu "
           "replay_req=%d replay=%d replay_sid=%016llX replay_cursor=%lu queue_full=%d dropped_full=%lu",
           s_storage_ready ? 1 : 0, s_session_open ? 1 : 0, (unsigned long long)s_session_id,
           (unsigned long)s_queue_backlog_records, (unsigned long)s_queue_backlog_bytes,
           s_replay_requested ? 1 : 0, s_replay_active ? 1 : 0,
           (unsigned long long)s_replay_session_id,
           (unsigned long)s_replay_cursor_seq, s_queue_full ? 1 : 0,
           (unsigned long)s_dropped_flash_full);

  if (!s_node_seen_hello && s_node_uart_rx_bytes == 0 && s_node_tx_frames >= 20 &&
      !s_node_no_rx_warned) {
    s_node_no_rx_warned = true;
    ESP_LOGW(TAG,
             "node RX is silent while TX is active. Check C6 GPIO%d->ESP8266 RX0(GPIO3), "
             "C6 GPIO%d<-ESP8266 TX0(GPIO1), common GND, and UART baud=%d on both sides.",
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
  *payload = (wg_status_payload_t){
      .scanning = s_scanning,
      .ble_encrypted = s_ble_encrypted,
      .current_channel = s_current_channel,
      .hop_ms = s_hop_ms,
      .channel_mask = s_channel_mask,
      .unique_bssid_count = s_unique_bssid_count,
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
      .node_channel_mask = s_node_status.channel_mask,
      .session_open = s_session_open,
      .session_id = s_session_open ? s_session_id : s_last_session_id,
      .queued_records = s_queue_backlog_records,
      .queued_bytes = s_queue_backlog_bytes,
      .replay_active = s_replay_active,
      .replay_cursor = s_replay_cursor_seq,
      .queue_full = s_queue_full,
      .dropped_flash_full = s_dropped_flash_full,
      .node_count = 2,
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
              s_latest_gps.sat_count != src->sat_count ||
              s_latest_gps.hdop_centi != src->hdop_centi ||
              s_latest_gps.pdop_centi != src->pdop_centi;
    s_latest_gps = *src;
    s_gps_source = source;
  }
  if (changed) {
    if (s_latest_gps.valid) {
      ESP_LOGI(TAG, "GPS selected src=%s lat=%ld lon=%ld acc_cm=%u sats=%u hdop=%.2f pdop=%.2f age_ms=%lld",
               gps_source_name(s_gps_source), (long)s_latest_gps.lat_e7, (long)s_latest_gps.lon_e7,
               s_latest_gps.accuracy_cm, (unsigned)s_latest_gps.sat_count,
               (double)s_latest_gps.hdop_centi / 100.0, (double)s_latest_gps.pdop_centi / 100.0,
               (long long)(now_ms() - s_latest_gps.received_ms));
    } else if (prev_valid || prev_source != WG_GPS_SRC_NONE) {
      ESP_LOGW(TAG, "GPS lost (prev_src=%s)", gps_source_name(prev_source));
    }
    led_sync_scan_gps_state();
    notify_status_frame();
    notify_gps_frame();
    if (!prev_valid && s_latest_gps.valid && s_boot_mode == WG_BOOT_AUTO && !s_scanning) {
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
    recompute_effective_gps_fix();
    return;
  }

  s_uart_gps.valid = true;
  s_uart_gps.flags = WG_GPS_FLAG_VALID | WG_GPS_FLAG_HAS_ALT;
  s_uart_gps.lat_e7 = lat_e7;
  s_uart_gps.lon_e7 = lon_e7;
  s_uart_gps.alt_mm = (int32_t)llround(alt_m * 1000.0);
  if (hdop < 0.3) {
    hdop = 0.3;
  }
  s_uart_gps.accuracy_cm = (uint16_t)clamp_u16_u32((uint32_t)llround(hdop * 500.0));
  int64_t now = now_ms();
  bool gsv_fresh =
      s_gps_last_gsv_ms > 0 && (now - s_gps_last_gsv_ms) >= 0 &&
      (now - s_gps_last_gsv_ms) <= WG_GPS_GSV_FRESH_MS;
  if (!gsv_fresh) {
    uint32_t sat_u32 = sats > 0 ? (uint32_t)sats : 0U;
    if (sat_u32 > UINT8_MAX) {
      sat_u32 = UINT8_MAX;
    }
    s_uart_gps.sat_count = (uint8_t)sat_u32;
    s_gps_last_sats = sats;
  }
  s_uart_gps.hdop_centi = clamp_u16_u32((uint32_t)llround(hdop * 100.0));
  s_uart_gps.received_ms = now;
  s_gps_fix_updates++;
  recompute_effective_gps_fix();
}

static void parse_nmea_gsa(char *line) {
  char *save = NULL;
  char *token = strtok_r(line, ",", &save);
  int idx = 0;
  int fix_type = 1;
  uint16_t pdop_centi = 0;
  uint16_t hdop_centi = 0;

  while (token != NULL) {
    switch (idx) {
      case 2:
        fix_type = atoi(token);
        break;
      case 15:
        pdop_centi = parse_nmea_dop_centi(token);
        break;
      case 16:
        hdop_centi = parse_nmea_dop_centi(token);
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
  if (fix_type <= 1 && s_uart_gps.valid) {
    s_uart_gps.valid = false;
    recompute_effective_gps_fix();
  }
}

static void parse_nmea_gsv(char *line) {
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
  s_uart_gps.sat_count = (uint8_t)sat_u32;
  s_gps_last_sats = sats_visible;
  s_gps_last_gsv_ms = now_ms();
  recompute_effective_gps_fix();
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
  s_gps_nmea_lines_total++;
  char copy[128] = {0};
  size_t n = strnlen(line, sizeof(copy) - 1);
  memcpy(copy, line, n);
  copy[n] = '\0';
  char *star = strchr(copy, '*');
  if (star != NULL) {
    *star = '\0';
  }
  if (strstr(copy, "GGA") != NULL) {
    s_gps_nmea_gga++;
    parse_nmea_gga(copy);
  } else if (strstr(copy, "RMC") != NULL) {
    s_gps_nmea_rmc++;
    parse_nmea_rmc(copy);
  } else if (strstr(copy, "GSA") != NULL) {
    s_gps_nmea_gsa++;
    parse_nmea_gsa(copy);
  } else if (strstr(copy, "GSV") != NULL) {
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
  uint8_t payload[5] = {0};
  bool scan_enable = s_scanning && s_node_channel_mask != 0;
  bool report_enable = scan_enable && s_latest_gps.valid;
  payload[0] = (scan_enable ? 1U : 0U) | (report_enable ? 2U : 0U);
  payload[1] = (uint8_t)(s_hop_ms & 0xFF);
  payload[2] = (uint8_t)((s_hop_ms >> 8) & 0xFF);
  payload[3] = (uint8_t)(s_node_channel_mask & 0xFF);
  payload[4] = (uint8_t)((s_node_channel_mask >> 8) & 0xFF);
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

static void host_serial_consume_byte(uint8_t b) {
  if (s_host_rx_pos == 0) {
    if (b != WG_FRAME_MAGIC0) {
      return;
    }
    s_host_rx_expected = 0;
    s_host_rx_buf[s_host_rx_pos++] = b;
    return;
  }

  if (s_host_rx_pos == 1) {
    if (b == WG_FRAME_MAGIC1) {
      s_host_rx_buf[s_host_rx_pos++] = b;
      return;
    }
    s_host_rx_pos = (b == WG_FRAME_MAGIC0) ? 1 : 0;
    s_host_rx_expected = 0;
    if (s_host_rx_pos == 1) {
      s_host_rx_buf[0] = WG_FRAME_MAGIC0;
    }
    return;
  }

  if (s_host_rx_pos >= sizeof(s_host_rx_buf)) {
    s_host_rx_errors++;
    host_serial_reset_rx();
    return;
  }

  s_host_rx_buf[s_host_rx_pos++] = b;
  if (s_host_rx_pos == WG_FRAME_HEADER_SIZE) {
    uint16_t payload_len = (uint16_t)s_host_rx_buf[6] | ((uint16_t)s_host_rx_buf[7] << 8);
    if (payload_len > WG_HOST_FRAME_MAX_PAYLOAD) {
      s_host_rx_errors++;
      host_serial_reset_rx();
      return;
    }
    s_host_rx_expected = (size_t)WG_FRAME_HEADER_SIZE + (size_t)payload_len;
    if (s_host_rx_expected > sizeof(s_host_rx_buf)) {
      s_host_rx_errors++;
      host_serial_reset_rx();
      return;
    }
  }

  if (s_host_rx_expected > 0 && s_host_rx_pos >= s_host_rx_expected) {
    wg_frame_t preview = {0};
    if (!s_host_serial_active &&
        wg_frame_decode(s_host_rx_buf, s_host_rx_expected, &preview) == WG_OK &&
        preview.version == WG_PROTOCOL_VERSION && preview.type == WG_MSG_COMMAND) {
      host_serial_mark_active();
    }
    uint8_t cmd_id = 0;
    uint8_t result = process_control_frame(s_host_rx_buf, s_host_rx_expected, &cmd_id);
    if (cmd_id != 0) {
      host_serial_mark_active();
    }
    if (s_host_serial_active) {
      (void)send_ack_host(cmd_id, result);
    }
    s_host_rx_frames++;
    host_serial_reset_rx();
  }
}

static void host_serial_task(void *arg) {
  (void)arg;
  uint8_t buf[96];
  while (true) {
    int n = host_serial_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(WG_HOST_SERIAL_READ_MS));
    if (n <= 0) {
      continue;
    }
    for (int i = 0; i < n; ++i) {
      host_serial_consume_byte(buf[i]);
    }
  }
}

static void init_host_serial_bridge(void) {
#if SOC_USB_SERIAL_JTAG_SUPPORTED
  usb_serial_jtag_driver_config_t cfg = {
      .tx_buffer_size = 4096,
      .rx_buffer_size = 1024,
  };
  esp_err_t rc = usb_serial_jtag_driver_install(&cfg);
  if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "host serial disabled: usb_serial_jtag_driver_install rc=%s",
             esp_err_to_name(rc));
    s_host_serial_enabled = false;
    return;
  }
  s_host_tx_mutex = xSemaphoreCreateMutex();
  if (s_host_tx_mutex == NULL) {
    ESP_LOGW(TAG, "host serial disabled: mutex allocation failed");
    s_host_serial_enabled = false;
    return;
  }
  s_host_serial_enabled = true;
  host_serial_reset_rx();
  if (xTaskCreate(host_serial_task, "wg_host_serial", 4096, NULL, 5, NULL) != pdPASS) {
    ESP_LOGW(TAG, "host serial disabled: task create failed");
    s_host_serial_enabled = false;
    return;
  }
  ESP_LOGI(TAG, "host serial bridge ready over USB");
#else
  ESP_LOGW(TAG, "host serial bridge unavailable on this target");
#endif
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

  split_channel_mask(s_channel_mask, &s_local_channel_mask, &s_node_channel_mask);
  ESP_LOGI(TAG, "UART bridges ready local_mask=0x%04X node_mask=0x%04X", s_local_channel_mask,
           s_node_channel_mask);
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

  uint8_t frame[WG_FRAME_HEADER_SIZE + WG_BLE_NOTIFY_FRAME_MAX_PAYLOAD];
  if (payload_len > WG_BLE_NOTIFY_FRAME_MAX_PAYLOAD ||
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
      .sat_count = s_latest_gps.sat_count,
      .hdop_centi = s_latest_gps.hdop_centi,
      .pdop_centi = s_latest_gps.pdop_centi,
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

  if (persisted && s_replay_active) {
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
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(WG_REPLAY_TASK_INTERVAL_MS));
    storage_run_replay_tick();
  }
}

static esp_err_t start_scan(void) {
  if (!s_latest_gps.valid) {
    ESP_LOGW(TAG, "Scan start blocked: no valid GPS fix");
    return ESP_ERR_INVALID_STATE;
  }
  if (s_scanning) {
    return ESP_OK;
  }

  storage_set_replay_enabled(false);
  seed_ap_cache_from_active_scan();
  if (s_storage_ready && !storage_open_session()) {
    ESP_LOGW(TAG, "Scan start blocked: storage session open failed");
    return ESP_FAIL;
  }
  s_current_channel = pick_first_channel(s_local_channel_mask);
  ESP_ERROR_CHECK(esp_wifi_set_channel(s_current_channel, WIFI_SECOND_CHAN_NONE));
  ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

  if (!s_hop_timer_started) {
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_hop_timer, (uint64_t)s_hop_ms * 1000ULL));
    s_hop_timer_started = true;
  }
  s_scanning = true;
  led_sync_scan_gps_state();
  notify_status_frame();
  node_send_config();
  return ESP_OK;
}

static esp_err_t stop_scan(void) {
  if (!s_scanning) {
    return ESP_OK;
  }
  if (s_hop_timer_started) {
    esp_timer_stop(s_hop_timer);
    s_hop_timer_started = false;
  }
  ESP_ERROR_CHECK(esp_wifi_set_promiscuous(false));
  s_scanning = false;
  if (s_storage_ready) {
    storage_close_session(WG_SESSION_STATE_CLOSED);
  }
  led_sync_scan_gps_state();
  notify_status_frame();
  node_send_config();
  return ESP_OK;
}

static void save_runtime_config(void) {
  nvs_handle_t h = 0;
  if (nvs_open("espwigle", NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGW(TAG, "nvs open failed for save");
    return;
  }
  nvs_set_u16(h, "hop_ms", s_hop_ms);
  nvs_set_u16(h, "chanmask", s_channel_mask);
  nvs_set_u8(h, "bootmode", s_boot_mode);
  nvs_commit(h);
  nvs_close(h);
}

static void load_runtime_config(void) {
  s_hop_ms = WG_DEFAULT_HOP_MS;
  s_channel_mask = WG_DEFAULT_CHANNEL_MASK;
  s_boot_mode = WG_BOOT_MANUAL;

  nvs_handle_t h = 0;
  if (nvs_open("espwigle", NVS_READONLY, &h) != ESP_OK) {
    return;
  }

  uint16_t hop_ms = 0;
  uint16_t channel_mask = 0;
  uint8_t boot_mode = 0;

  if (nvs_get_u16(h, "hop_ms", &hop_ms) == ESP_OK && hop_ms >= WG_MIN_HOP_MS &&
      hop_ms <= WG_MAX_HOP_MS) {
    s_hop_ms = hop_ms;
  }
  if (nvs_get_u16(h, "chanmask", &channel_mask) == ESP_OK &&
      wg_channel_mask_is_valid(channel_mask)) {
    s_channel_mask = channel_mask;
  }
  if (nvs_get_u8(h, "bootmode", &boot_mode) == ESP_OK &&
      (boot_mode == WG_BOOT_MANUAL || boot_mode == WG_BOOT_AUTO)) {
    s_boot_mode = boot_mode;
  }
  nvs_close(h);
  split_channel_mask(s_channel_mask, &s_local_channel_mask, &s_node_channel_mask);
  ESP_LOGI(TAG, "config loaded hop=%u mask=0x%04X boot=%u", (unsigned)s_hop_ms,
           (unsigned)s_channel_mask, (unsigned)s_boot_mode);
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
      return start_scan() == ESP_OK ? WG_ACK_OK : WG_ERR_INTERNAL;
    case WG_CMD_STOP:
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
      if (cmd->replay_enable == 1 && s_scanning) {
        return WG_ERR_BUSY;
      }
      storage_set_replay_enabled(cmd->replay_enable == 1);
      notify_status_frame();
      return WG_ACK_OK;
    case WG_CMD_CLEAR_STORAGE:
      if (s_scanning) {
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
      s_phone_gps.sat_count = 0;
      s_phone_gps.hdop_centi = 0;
      s_phone_gps.pdop_centi = 0;
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
    return os_mbuf_append(ctxt->om, WG_DEVICE_INFO_TEXT, strlen(WG_DEVICE_INFO_TEXT)) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
  }

  return BLE_ATT_ERR_UNLIKELY;
}

static int handle_gatt_write(uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt) {
  if (attr_handle != s_control_handle && attr_handle != s_config_handle) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  if (!s_ble_encrypted) {
    return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
  }

  const uint16_t incoming_len = OS_MBUF_PKTLEN(ctxt->om);
  if (incoming_len > 196) {
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  }
  uint8_t incoming[196] = {0};
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
      return handle_gatt_write(attr_handle, ctxt);
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

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
  (void)arg;
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      ESP_LOGI(TAG, "BLE connect event status=%d", event->connect.status);
      if (event->connect.status == 0) {
        s_conn_handle = event->connect.conn_handle;
        s_security_retry_attempted = false;
        refresh_link_security_state(s_conn_handle);
        request_fast_ble_conn_params(s_conn_handle);
        led_sync_link_state();
        if (!s_ble_encrypted) {
          int rc = ble_gap_security_initiate(s_conn_handle);
          if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "ble_gap_security_initiate failed rc=%d", rc);
          }
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
      s_status_notify_enabled = false;
      s_gps_notify_enabled = false;
      s_sighting_notify_enabled = false;
      storage_set_replay_enabled(false);
      led_sync_link_state();
      ble_advertise();
      return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
      ESP_LOGI(TAG, "BLE encryption change status=%d", event->enc_change.status);
      if (event->enc_change.status == 0) {
        refresh_link_security_state(event->enc_change.conn_handle);
        s_security_retry_attempted = false;
      } else {
        s_ble_encrypted = false;
        if (!s_security_retry_attempted) {
          s_security_retry_attempted = true;
          clear_peer_bond_for_conn(event->enc_change.conn_handle, "enc_change");
          int retry_rc = ble_gap_security_initiate(event->enc_change.conn_handle);
          if (retry_rc != 0 && retry_rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "ble_gap_security_initiate retry failed rc=%d", retry_rc);
          } else {
            ESP_LOGW(TAG, "ble_gap_security_initiate retry requested");
          }
        }
      }
      led_sync_link_state();
      notify_status_frame();
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

  ble_hs_cfg.reset_cb = ble_on_reset;
  ble_hs_cfg.sync_cb = ble_on_sync;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 1;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_svc_gap_device_name_set(WG_DEVICE_NAME);

  ble_gatts_count_cfg(gatt_services);
  ble_gatts_add_svcs(gatt_services);

  ble_store_config_init();
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
  load_runtime_config();
  init_wifi();
  init_storage();
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
