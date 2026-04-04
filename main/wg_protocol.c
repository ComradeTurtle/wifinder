#include "wg_protocol.h"

#define WG_MAGIC0 0x57
#define WG_MAGIC1 0x47

static uint16_t rd_u16(const uint8_t *in) { return (uint16_t)in[0] | ((uint16_t)in[1] << 8); }

static uint32_t rd_u32(const uint8_t *in) {
  return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
         ((uint32_t)in[3] << 24);
}

static uint64_t rd_u64(const uint8_t *in) {
  return (uint64_t)rd_u32(in) | ((uint64_t)rd_u32(in + 4) << 32);
}

static int32_t rd_i32(const uint8_t *in) { return (int32_t)rd_u32(in); }

static void wr_u16(uint8_t *out, uint16_t value) {
  out[0] = (uint8_t)(value & 0xFF);
  out[1] = (uint8_t)((value >> 8) & 0xFF);
}

static void wr_u32(uint8_t *out, uint32_t value) {
  out[0] = (uint8_t)(value & 0xFF);
  out[1] = (uint8_t)((value >> 8) & 0xFF);
  out[2] = (uint8_t)((value >> 16) & 0xFF);
  out[3] = (uint8_t)((value >> 24) & 0xFF);
}

wg_result_t wg_frame_encode(const wg_frame_t *frame, uint8_t *out, size_t out_capacity,
                            size_t *out_len) {
  if (frame == 0 || out == 0 || out_len == 0) {
    return WG_ERR_INVALID_ARG;
  }
  if (frame->len > 0 && frame->payload == 0) {
    return WG_ERR_INVALID_ARG;
  }

  const size_t total = WG_FRAME_HEADER_SIZE + (size_t)frame->len;
  if (out_capacity < total) {
    return WG_ERR_BUFFER_TOO_SMALL;
  }

  out[0] = WG_MAGIC0;
  out[1] = WG_MAGIC1;
  out[2] = frame->version;
  out[3] = frame->type;
  wr_u16(&out[4], frame->seq);
  wr_u16(&out[6], frame->len);
  wr_u32(&out[8], frame->device_ms);

  for (uint16_t i = 0; i < frame->len; ++i) {
    out[WG_FRAME_HEADER_SIZE + i] = frame->payload[i];
  }
  *out_len = total;
  return WG_OK;
}

wg_result_t wg_frame_decode(const uint8_t *bytes, size_t bytes_len, wg_frame_t *out) {
  if (bytes == 0 || out == 0) {
    return WG_ERR_INVALID_ARG;
  }
  if (bytes_len < WG_FRAME_HEADER_SIZE) {
    return WG_ERR_INVALID_FRAME;
  }
  if (bytes[0] != WG_MAGIC0 || bytes[1] != WG_MAGIC1) {
    return WG_ERR_INVALID_FRAME;
  }

  const uint16_t payload_len = rd_u16(&bytes[6]);
  const size_t expected = WG_FRAME_HEADER_SIZE + (size_t)payload_len;
  if (bytes_len < expected) {
    return WG_ERR_INVALID_FRAME;
  }

  out->version = bytes[2];
  out->type = bytes[3];
  out->seq = rd_u16(&bytes[4]);
  out->len = payload_len;
  out->device_ms = rd_u32(&bytes[8]);
  out->payload = &bytes[WG_FRAME_HEADER_SIZE];
  return WG_OK;
}

wg_result_t wg_command_decode(const uint8_t *payload, size_t payload_len, wg_command_t *out) {
  if (payload == 0 || out == 0 || payload_len < 1) {
    return WG_ERR_INVALID_ARG;
  }

  out->id = (wg_command_id_t)payload[0];
  out->hop_ms = 0;
  out->channel_mask = 0;
  out->boot_mode = 0;
  out->gps_flags = 0;
  out->gps_lat_e7 = 0;
  out->gps_lon_e7 = 0;
  out->gps_alt_mm = 0;
  out->gps_speed_mmps = 0;
  out->gps_bearing_mdeg = 0;
  out->gps_unix_time_s = 0;
  out->gps_accuracy_cm = 0;
  out->replay_session_id = 0;
  out->replay_highest_seq = 0;
  out->replay_enable = 0;
  out->gps_nav_mode = 0;
  out->backlog_blob_enable = 0;

  switch (out->id) {
    case WG_CMD_START:
    case WG_CMD_STOP:
    case WG_CMD_REQUEST_STATUS:
    case WG_CMD_REQUEST_SNAPSHOT:
    case WG_CMD_CLEAR_STORAGE:
      if (payload_len != 1) {
        return WG_ERR_INVALID_FRAME;
      }
      return WG_OK;
    case WG_CMD_SET_HOP_MS:
      if (payload_len != 3) {
        return WG_ERR_INVALID_FRAME;
      }
      out->hop_ms = rd_u16(&payload[1]);
      return WG_OK;
    case WG_CMD_SET_CHANNEL_MASK:
      if (payload_len != 3) {
        return WG_ERR_INVALID_FRAME;
      }
      out->channel_mask = rd_u16(&payload[1]);
      return WG_OK;
    case WG_CMD_SET_BOOT_MODE:
      if (payload_len != 2) {
        return WG_ERR_INVALID_FRAME;
      }
      out->boot_mode = payload[1];
      return WG_OK;
    case WG_CMD_SET_GPS_FIX:
      if (payload_len != (size_t)(1 + WG_GPS_FIX_PAYLOAD_SIZE)) {
        return WG_ERR_INVALID_FRAME;
      }
      out->gps_flags = payload[1];
      out->gps_lat_e7 = rd_i32(&payload[2]);
      out->gps_lon_e7 = rd_i32(&payload[6]);
      out->gps_alt_mm = rd_i32(&payload[10]);
      out->gps_speed_mmps = rd_u32(&payload[14]);
      out->gps_bearing_mdeg = rd_u32(&payload[18]);
      out->gps_unix_time_s = rd_u32(&payload[22]);
      out->gps_accuracy_cm = rd_u16(&payload[26]);
      return WG_OK;
    case WG_CMD_REPLAY_ACK:
      if (payload_len != (size_t)(1 + WG_REPLAY_ACK_PAYLOAD_SIZE)) {
        return WG_ERR_INVALID_FRAME;
      }
      out->replay_session_id = rd_u64(&payload[1]);
      out->replay_highest_seq = rd_u32(&payload[9]);
      return WG_OK;
    case WG_CMD_SET_REPLAY:
      if (payload_len != (size_t)(1 + WG_REPLAY_TOGGLE_PAYLOAD_SIZE)) {
        return WG_ERR_INVALID_FRAME;
      }
      out->replay_enable = payload[1];
      return WG_OK;
    case WG_CMD_SET_GPS_NAV_RATE:
      if (payload_len != (size_t)(1 + WG_GPS_NAV_RATE_PAYLOAD_SIZE)) {
        return WG_ERR_INVALID_FRAME;
      }
      out->gps_nav_mode = payload[1];
      return WG_OK;
    case WG_CMD_SET_BACKLOG_BLOB:
      if (payload_len != (size_t)(1 + WG_BACKLOG_BLOB_TOGGLE_PAYLOAD_SIZE)) {
        return WG_ERR_INVALID_FRAME;
      }
      out->backlog_blob_enable = payload[1];
      return WG_OK;
    default:
      return WG_ERR_UNSUPPORTED;
  }
}
