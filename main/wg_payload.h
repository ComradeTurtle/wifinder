#ifndef WG_PAYLOAD_H
#define WG_PAYLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  WG_BOOT_MANUAL = 0,
  WG_BOOT_AUTO = 1,
} wg_boot_mode_t;

typedef enum {
  WG_AUTH_UNKNOWN = 0,
  WG_AUTH_OPEN = 1,
  WG_AUTH_WEP = 2,
  WG_AUTH_WPA = 3,
  WG_AUTH_WPA2_WPA3 = 4,
} wg_auth_mode_t;

typedef enum {
  WG_SEC_PROTO_WPA = 1 << 0,
  WG_SEC_PROTO_WPA2 = 1 << 1,
  WG_SEC_PROTO_WPA3 = 1 << 2,
} wg_sec_proto_flags_t;

typedef enum {
  WG_SEC_AKM_EAP = 1 << 0,
  WG_SEC_AKM_PSK = 1 << 1,
  WG_SEC_AKM_SAE = 1 << 2,
  WG_SEC_AKM_OWE = 1 << 3,
} wg_sec_akm_flags_t;

typedef enum {
  WG_SEC_CIPHER_TKIP = 1 << 0,
  WG_SEC_CIPHER_CCMP_128 = 1 << 1,
  WG_SEC_CIPHER_GCMP_128 = 1 << 2,
  WG_SEC_CIPHER_GCMP_256 = 1 << 3,
  WG_SEC_CIPHER_CCMP_256 = 1 << 4,
  WG_SEC_CIPHER_WEP = 1 << 5,
} wg_sec_cipher_flags_t;

typedef struct {
  bool scanning;
  bool ble_encrypted;
  uint8_t current_channel;
  uint16_t hop_ms;
  uint16_t channel_mask;
  uint32_t unique_bssids_estimate;
  uint16_t packets_per_sec;
  uint16_t dropped_notifies;
  uint8_t boot_mode;
  bool gps_valid;
  uint16_t gps_age_s;
  uint16_t gps_accuracy_dm;
  bool node_link_up;
  uint16_t node_last_seen_s;
  uint16_t node_packets_per_sec;
  uint16_t node_forwarded_sightings;
  uint8_t node_channel;
  uint16_t node_channel_mask;
  bool session_open;
  uint64_t session_id;
  uint32_t queued_records;
  uint32_t queued_bytes;
  bool replay_active;
  uint32_t replay_cursor;
  bool queue_full;
  uint32_t dropped_flash_full;
  uint8_t node_count;
  uint8_t gps_nav_applied_hz;
  uint32_t spiffs_total_bytes;
  uint32_t spiffs_used_bytes;
  uint32_t spiffs_free_bytes;
  bool blob_active;
  uint64_t blob_session_id;
  uint32_t blob_bytes_sent;
  uint32_t blob_bytes_total;
} wg_status_payload_t;

typedef struct {
  uint8_t bssid[6];
  uint8_t channel;
  int8_t rssi;
  uint8_t auth_mode;
  uint8_t proto_flags;
  uint8_t akm_flags;
  uint8_t cipher_flags;
  uint8_t ssid_len;
  uint8_t ssid[32];
  uint8_t flags;
  uint64_t session_id;
  uint32_t record_seq;
  uint8_t node_id;
  uint8_t source_flags;
  uint8_t gps_valid;
  uint8_t gps_source;
  int32_t gps_lat_e7;
  int32_t gps_lon_e7;
  int32_t gps_alt_mm;
  uint32_t gps_unix_time_s;
  uint16_t gps_accuracy_cm;
} wg_sighting_payload_t;

typedef enum {
  WG_SIGHTING_SOURCE_LIVE = 1 << 0,
  WG_SIGHTING_SOURCE_REPLAY = 1 << 1,
} wg_sighting_source_flags_t;

typedef struct {
  bool valid;
  uint8_t source;
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
} wg_gps_payload_t;

#define WG_STATUS_PAYLOAD_SIZE 91
#define WG_GPS_PAYLOAD_SIZE 33

size_t wg_build_status_payload(const wg_status_payload_t *status, uint8_t *out, size_t out_size);
size_t wg_build_sighting_payload(const wg_sighting_payload_t *sighting, uint8_t *out,
                                 size_t out_size);
size_t wg_build_gps_payload(const wg_gps_payload_t *gps, uint8_t *out, size_t out_size);

#endif
