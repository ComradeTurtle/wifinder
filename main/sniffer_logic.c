#include "sniffer_logic.h"

#include <stdio.h>

uint8_t wardrive_next_channel(uint8_t current, uint8_t min_channel,
                              uint8_t max_channel) {
  if (min_channel == 0 || max_channel < min_channel) {
    return 1;
  }
  if (current < min_channel || current >= max_channel) {
    return min_channel;
  }
  return (uint8_t)(current + 1);
}

bool wardrive_is_interesting_mgmt(uint8_t frame_control_byte0) {
  const uint8_t frame_type = (uint8_t)(frame_control_byte0 & 0x0C);
  if (frame_type != 0x00) {
    return false;
  }
  const uint8_t subtype = (uint8_t)(frame_control_byte0 & 0xF0);
  return subtype == 0x80 || subtype == 0x50;
}

bool wardrive_format_bssid(const uint8_t bssid[6], char *out, size_t out_size) {
  if (bssid == NULL || out == NULL || out_size < 18) {
    return false;
  }
  const int written = snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X",
                               bssid[0], bssid[1], bssid[2], bssid[3], bssid[4],
                               bssid[5]);
  return written == 17;
}

size_t wardrive_mgmt_header_len(const uint8_t *payload, size_t payload_len) {
  if (payload == NULL || payload_len < 24) {
    return 0;
  }
  size_t hdr_len = 24;
  if ((payload[1] & 0x80) != 0) {
    hdr_len += 4;
  }
  return payload_len >= hdr_len ? hdr_len : 0;
}
