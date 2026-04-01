#ifndef WG_PROTOCOL_H
#define WG_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define WG_PROTOCOL_VERSION 2
#define WG_FRAME_HEADER_SIZE 12

typedef enum {
  WG_OK = 0,
  WG_ERR_INVALID_ARG = -1,
  WG_ERR_INVALID_FRAME = -2,
  WG_ERR_BUFFER_TOO_SMALL = -3,
  WG_ERR_UNSUPPORTED = -4,
} wg_result_t;

typedef enum {
  WG_MSG_STATUS = 0x01,
  WG_MSG_SIGHTING = 0x02,
  WG_MSG_ACK = 0x03,
  WG_MSG_ERROR = 0x04,
  WG_MSG_SNAPSHOT_END = 0x05,
  WG_MSG_CONFIG = 0x06,
  WG_MSG_GPS = 0x07,
  WG_MSG_REPLAY_ACK = 0x08,
  WG_MSG_NODE_TABLE = 0x09,
  WG_MSG_COMMAND = 0x81,
} wg_message_type_t;

typedef enum {
  WG_CMD_START = 0x01,
  WG_CMD_STOP = 0x02,
  WG_CMD_SET_HOP_MS = 0x03,
  WG_CMD_SET_CHANNEL_MASK = 0x04,
  WG_CMD_SET_BOOT_MODE = 0x05,
  WG_CMD_REQUEST_STATUS = 0x06,
  WG_CMD_REQUEST_SNAPSHOT = 0x07,
  WG_CMD_SET_GPS_FIX = 0x08,
  WG_CMD_REPLAY_ACK = 0x09,
  WG_CMD_SET_REPLAY = 0x0A,
  WG_CMD_CLEAR_STORAGE = 0x0B,
  WG_CMD_SET_GPS_NAV_RATE = 0x0C,
} wg_command_id_t;

typedef enum {
  WG_GPS_FLAG_VALID = 1 << 0,
  WG_GPS_FLAG_HAS_ALT = 1 << 1,
  WG_GPS_FLAG_HAS_SPEED = 1 << 2,
  WG_GPS_FLAG_HAS_BEARING = 1 << 3,
} wg_gps_flags_t;

#define WG_GPS_FIX_PAYLOAD_SIZE 27
#define WG_REPLAY_ACK_PAYLOAD_SIZE 12
#define WG_REPLAY_TOGGLE_PAYLOAD_SIZE 1
#define WG_GPS_NAV_RATE_PAYLOAD_SIZE 1

typedef struct {
  uint8_t version;
  uint8_t type;
  uint16_t seq;
  uint16_t len;
  uint32_t device_ms;
  const uint8_t *payload;
} wg_frame_t;

typedef struct {
  wg_command_id_t id;
  uint16_t hop_ms;
  uint16_t channel_mask;
  uint8_t boot_mode;
  uint8_t gps_flags;
  int32_t gps_lat_e7;
  int32_t gps_lon_e7;
  int32_t gps_alt_mm;
  uint32_t gps_speed_mmps;
  uint32_t gps_bearing_mdeg;
  uint32_t gps_unix_time_s;
  uint16_t gps_accuracy_cm;
  uint64_t replay_session_id;
  uint32_t replay_highest_seq;
  uint8_t replay_enable;
  uint8_t gps_nav_mode;
} wg_command_t;

wg_result_t wg_frame_encode(const wg_frame_t *frame, uint8_t *out, size_t out_capacity,
                            size_t *out_len);
wg_result_t wg_frame_decode(const uint8_t *bytes, size_t bytes_len, wg_frame_t *out);
wg_result_t wg_command_decode(const uint8_t *payload, size_t payload_len, wg_command_t *out);

#endif
