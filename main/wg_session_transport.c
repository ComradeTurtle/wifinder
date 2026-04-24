#include "wg_session_transport.h"

#include <stddef.h>

uint16_t wg_transport_rd_u16_le(const uint8_t *in) {
  if (in == NULL) {
    return 0;
  }
  return (uint16_t)in[0] | ((uint16_t)in[1] << 8);
}

void wg_transport_wr_u16_le(uint8_t *out, uint16_t value) {
  if (out == NULL) {
    return;
  }
  out[0] = (uint8_t)(value & 0xFFU);
  out[1] = (uint8_t)((value >> 8) & 0xFFU);
}

void wg_transport_wr_u32_le(uint8_t *out, uint32_t value) {
  if (out == NULL) {
    return;
  }
  out[0] = (uint8_t)(value & 0xFFU);
  out[1] = (uint8_t)((value >> 8) & 0xFFU);
  out[2] = (uint8_t)((value >> 16) & 0xFFU);
  out[3] = (uint8_t)((value >> 24) & 0xFFU);
}

void wg_transport_wr_u64_le(uint8_t *out, uint64_t value) {
  if (out == NULL) {
    return;
  }
  wg_transport_wr_u32_le(out, (uint32_t)(value & 0xFFFFFFFFULL));
  wg_transport_wr_u32_le(out + 4, (uint32_t)((value >> 32) & 0xFFFFFFFFULL));
}

const char *wg_transport_ble_phy_name(uint8_t phy) {
  switch (phy) {
    case 1:
      return "1M";
    case 2:
      return "2M";
    case 3:
      return "CODED";
    default:
      return "unknown";
  }
}

uint16_t wg_transport_ble_notify_payload_limit(uint16_t att_mtu, uint16_t frame_header_size,
                                               uint16_t payload_max, uint16_t data_len_octets,
                                               bool encrypted) {
  uint16_t mtu = att_mtu;
  if (mtu < 23U) {
    mtu = 23U;
  }
  if (mtu <= (uint16_t)(frame_header_size + 3U)) {
    return 0U;
  }

  uint16_t limit = (uint16_t)(mtu - frame_header_size - 3U);
  if (limit > payload_max) {
    limit = payload_max;
  }

  const uint16_t mic = encrypted ? 4U : 0U;
  const uint16_t l2cap_per_frag = (data_len_octets > mic) ? (uint16_t)(data_len_octets - mic) : 1U;
  const uint16_t safe_l2cap = (uint16_t)(l2cap_per_frag * 2U);
  const uint16_t safe_att = (safe_l2cap > 4U) ? (uint16_t)(safe_l2cap - 4U) : 0U;
  const uint16_t safe_notify = (safe_att > 3U) ? (uint16_t)(safe_att - 3U) : 0U;
  const uint16_t safe_payload =
      (safe_notify > frame_header_size) ? (uint16_t)(safe_notify - frame_header_size) : 0U;
  if (limit > safe_payload) {
    limit = safe_payload;
  }

  return limit;
}
