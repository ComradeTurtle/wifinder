#include <Arduino.h>
#include <ESP8266WiFi.h>

#include <ctype.h>
#include <string.h>

extern "C" {
#include "user_interface.h"
}

namespace {

constexpr uint8_t WG_NODE_FRAME_SYNC0 = 0xA5;
constexpr uint8_t WG_NODE_FRAME_SYNC1 = 0x5A;
constexpr uint8_t WG_NODE_FRAME_VERSION = 1;
constexpr uint8_t WG_NODE_FRAME_HEADER_SIZE = 10;
constexpr uint16_t WG_NODE_FRAME_MAX_PAYLOAD = 96;

constexpr uint16_t WG_DEFAULT_CHANNEL_MASK = 0x1FFF;
constexpr uint16_t WG_DEFAULT_HOP_MS = 250;
constexpr uint16_t WG_MIN_HOP_MS = 50;
constexpr uint16_t WG_MAX_HOP_MS = 2000;

constexpr uint32_t WG_NODE_STATUS_INTERVAL_MS = 1000;
constexpr uint32_t WG_NODE_LINK_TIMEOUT_MS = 6000;
constexpr uint32_t WG_SIGHTING_NOTIFY_INTERVAL_MS = 3000;

constexpr uint8_t WG_AP_CAPACITY = 180;
constexpr uint8_t WG_QUEUE_CAPACITY = 80;

constexpr uint32_t WG_UART_BAUD = 460800;
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif
constexpr uint8_t WG_STATUS_LED_PRIMARY_GPIO = LED_BUILTIN;
constexpr uint8_t WG_STATUS_LED_FALLBACK_GPIO = (LED_BUILTIN == 2) ? 16 : 2;
constexpr bool WG_STATUS_LED_ACTIVE_LOW = true;

enum : uint8_t {
  WG_NODE_MSG_HELLO = 0x01,
  WG_NODE_MSG_HELLO_ACK = 0x02,
  WG_NODE_MSG_CONFIG = 0x03,
  WG_NODE_MSG_PING = 0x04,
  WG_NODE_MSG_PONG = 0x05,
  WG_NODE_MSG_STATUS = 0x06,
  WG_NODE_MSG_SIGHTING = 0x07,
};

enum : uint8_t {
  WG_AUTH_UNKNOWN = 0,
  WG_AUTH_OPEN = 1,
  WG_AUTH_WEP = 2,
  WG_AUTH_WPA = 3,
  WG_AUTH_WPA2_WPA3 = 4,
};

enum : uint8_t {
  WG_SEC_PROTO_WPA = 1 << 0,
  WG_SEC_PROTO_WPA2 = 1 << 1,
  WG_SEC_PROTO_WPA3 = 1 << 2,
};

enum : uint8_t {
  WG_SEC_AKM_EAP = 1 << 0,
  WG_SEC_AKM_PSK = 1 << 1,
  WG_SEC_AKM_SAE = 1 << 2,
  WG_SEC_AKM_OWE = 1 << 3,
};

enum : uint8_t {
  WG_SEC_CIPHER_TKIP = 1 << 0,
  WG_SEC_CIPHER_CCMP_128 = 1 << 1,
  WG_SEC_CIPHER_GCMP_128 = 1 << 2,
  WG_SEC_CIPHER_GCMP_256 = 1 << 3,
  WG_SEC_CIPHER_CCMP_256 = 1 << 4,
  WG_SEC_CIPHER_WEP = 1 << 5,
};

enum : uint8_t {
  WG_SIGHTING_FLAG_NEW = 1 << 0,
  WG_SIGHTING_FLAG_UPDATED = 1 << 2,
};

struct ap_entry_t {
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
  uint32_t last_seen_ms;
  uint32_t last_notified_ms;
};

struct sighting_event_t {
  uint8_t bssid[6];
  char ssid[33];
  uint8_t ssid_len;
  uint8_t auth_mode;
  uint8_t proto_flags;
  uint8_t akm_flags;
  uint8_t cipher_flags;
  uint8_t channel;
  int8_t rssi;
  uint8_t flags;
};

// ESP8266 NonOS SDK promiscuous RX metadata header.
// Management frame bytes start immediately after this header.
struct sniffer_rx_ctrl_t {
  signed rssi : 8;
  unsigned rate : 4;
  unsigned is_group : 1;
  unsigned : 1;
  unsigned sig_mode : 2;
  unsigned legacy_length : 12;
  unsigned damatch0 : 1;
  unsigned damatch1 : 1;
  unsigned bssidmatch0 : 1;
  unsigned bssidmatch1 : 1;
  unsigned MCS : 7;
  unsigned CWB : 1;
  unsigned HT_length : 16;
  unsigned Smoothing : 1;
  unsigned Not_Sounding : 1;
  unsigned : 1;
  unsigned Aggregation : 1;
  unsigned STBC : 2;
  unsigned FEC_CODING : 1;
  unsigned SGI : 1;
  unsigned rxend_state : 8;
  unsigned ampdu_cnt : 8;
  unsigned channel : 4;
  unsigned : 12;
};

static_assert(sizeof(sniffer_rx_ctrl_t) == 12, "unexpected sniffer_rx_ctrl_t size");

ap_entry_t s_seen[WG_AP_CAPACITY] = {};
sighting_event_t s_event_queue[WG_QUEUE_CAPACITY] = {};
volatile uint8_t s_event_head = 0;
volatile uint8_t s_event_tail = 0;
volatile uint32_t s_event_drop_count = 0;

uint16_t s_hop_ms = WG_DEFAULT_HOP_MS;
uint16_t s_channel_mask = WG_DEFAULT_CHANNEL_MASK;
uint8_t s_current_channel = 1;

bool s_scan_enabled = false;
bool s_scanning = false;
bool s_report_enabled = false;
bool s_link_established = false;

uint32_t s_last_hop_ms = 0;
uint32_t s_last_status_ms = 0;
uint32_t s_last_rx_ms = 0;
uint32_t s_led_last_activity_ms = 0;

uint16_t s_tx_seq = 1;

uint32_t s_rate_window_start_ms = 0;
uint32_t s_rate_packet_acc = 0;
uint16_t s_packets_per_sec = 0;

uint8_t s_rx_state = 0;
uint8_t s_rx_hdr[WG_NODE_FRAME_HEADER_SIZE] = {0};
uint8_t s_rx_hdr_pos = 0;
uint8_t s_rx_payload[WG_NODE_FRAME_MAX_PAYLOAD] = {0};
uint16_t s_rx_payload_len = 0;
uint16_t s_rx_payload_pos = 0;
uint8_t s_rx_crc_bytes[2] = {0};
uint8_t s_rx_crc_pos = 0;
bool s_led_is_on = false;

uint16_t rd_u16_le(const uint8_t *in) { return (uint16_t)in[0] | ((uint16_t)in[1] << 8); }

void wr_u16_le(uint8_t *out, uint16_t v) {
  out[0] = (uint8_t)(v & 0xFF);
  out[1] = (uint8_t)((v >> 8) & 0xFF);
}

void wr_u32_le(uint8_t *out, uint32_t v) {
  out[0] = (uint8_t)(v & 0xFF);
  out[1] = (uint8_t)((v >> 8) & 0xFF);
  out[2] = (uint8_t)((v >> 16) & 0xFF);
  out[3] = (uint8_t)((v >> 24) & 0xFF);
}

uint16_t crc16_ccitt_seed(uint16_t seed, const uint8_t *data, size_t len) {
  uint16_t crc = seed;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      if ((crc & 0x8000U) != 0U) {
        crc = (uint16_t)((crc << 1) ^ 0x1021U);
      } else {
        crc <<= 1;
      }
    }
  }
  return crc;
}

uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
  return crc16_ccitt_seed(0xFFFFU, data, len);
}

bool channel_mask_is_valid(uint16_t mask) {
  if ((mask & 0x1FFFU) == 0U) {
    return false;
  }
  return (mask & (uint16_t)(~0x1FFFU)) == 0U;
}

uint8_t pick_first_channel(uint16_t mask) {
  for (uint8_t ch = 1; ch <= 13; ++ch) {
    if ((mask & (uint16_t)(1U << (ch - 1U))) != 0U) {
      return ch;
    }
  }
  return 1;
}

uint8_t next_enabled_channel(uint16_t mask, uint8_t current) {
  for (uint8_t i = 1; i <= 13; ++i) {
    uint8_t ch = (uint8_t)(((current + i - 1U) % 13U) + 1U);
    if ((mask & (uint16_t)(1U << (ch - 1U))) != 0U) {
      return ch;
    }
  }
  return 1;
}

bool is_interesting_mgmt(uint8_t fc0) {
  const uint8_t frame_type = (uint8_t)(fc0 & 0x0CU);
  if (frame_type != 0x00U) {
    return false;
  }
  const uint8_t subtype = (uint8_t)(fc0 & 0xF0U);
  return subtype == 0x80U || subtype == 0x50U;
}

size_t mgmt_header_len(const uint8_t *payload, size_t payload_len) {
  if (payload == nullptr || payload_len < 24U) {
    return 0;
  }
  size_t hdr_len = 24;
  if ((payload[1] & 0x80U) != 0U) {
    hdr_len += 4;
  }
  return payload_len >= hdr_len ? hdr_len : 0;
}

bool copy_ssid_if_clean_ascii(const uint8_t *in, uint8_t len, char *out, uint8_t *out_len) {
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

uint8_t rsn_cipher_flags_from_suite(const uint8_t *suite) {
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

uint8_t wpa_cipher_flags_from_suite(const uint8_t *suite) {
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

uint8_t rsn_akm_flags_from_suite(const uint8_t *suite) {
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

uint8_t wpa_akm_flags_from_suite(const uint8_t *suite) {
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

void parse_rsn_ie(const uint8_t *ie_data, uint8_t ie_len, uint8_t *proto_flags,
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

void parse_wpa_vendor_ie(const uint8_t *ie_data, uint8_t ie_len, uint8_t *proto_flags,
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

uint8_t parse_auth_mode(bool privacy, uint8_t proto_flags, uint8_t cipher_flags) {
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

void parse_beacon_probe(const uint8_t *payload, size_t payload_len, char *ssid, uint8_t *ssid_len,
                        uint8_t *auth_mode_out, uint8_t *proto_flags_out, uint8_t *akm_flags_out,
                        uint8_t *cipher_flags_out) {
  bool privacy = false;
  *ssid_len = 0;
  ssid[0] = '\0';
  *auth_mode_out = WG_AUTH_UNKNOWN;
  *proto_flags_out = 0;
  *akm_flags_out = 0;
  *cipher_flags_out = 0;

  const size_t hdr_len = mgmt_header_len(payload, payload_len);
  if (hdr_len == 0 || payload_len < hdr_len + 12) {
    return;
  }

  const uint16_t capability_info =
      (uint16_t)payload[hdr_len + 10] | ((uint16_t)payload[hdr_len + 11] << 8);
  privacy = (capability_info & 0x0010U) != 0;

  const uint8_t *ie = payload + hdr_len + 12;
  size_t remaining = payload_len - hdr_len - 12;

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

  *auth_mode_out = parse_auth_mode(privacy, *proto_flags_out, *cipher_flags_out);
}

int find_ap_entry(const uint8_t bssid[6]) {
  for (uint8_t i = 0; i < WG_AP_CAPACITY; ++i) {
    if (s_seen[i].in_use && memcmp(s_seen[i].bssid, bssid, 6) == 0) {
      return (int)i;
    }
  }
  return -1;
}

int pick_entry_slot() {
  int free_idx = -1;
  int oldest_idx = 0;
  uint32_t oldest_ms = UINT32_MAX;
  for (uint8_t i = 0; i < WG_AP_CAPACITY; ++i) {
    if (!s_seen[i].in_use) {
      free_idx = (int)i;
      break;
    }
    if (s_seen[i].last_seen_ms < oldest_ms) {
      oldest_ms = s_seen[i].last_seen_ms;
      oldest_idx = (int)i;
    }
  }
  return free_idx >= 0 ? free_idx : oldest_idx;
}

uint8_t bit_count_u8(uint8_t value) {
  uint8_t count = 0;
  while (value != 0) {
    count += (uint8_t)(value & 1U);
    value >>= 1;
  }
  return count;
}

uint16_t security_score(uint8_t auth_mode, uint8_t proto_flags, uint8_t akm_flags,
                        uint8_t cipher_flags) {
  uint16_t score = 0;
  if (proto_flags != 0) {
    score = 120;
    score += (uint16_t)bit_count_u8(proto_flags) * 12U;
    score += (uint16_t)bit_count_u8(akm_flags) * 6U;
    score += (uint16_t)bit_count_u8(cipher_flags) * 3U;
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

uint8_t resolve_auth_mode(uint8_t fallback_auth_mode, uint8_t proto_flags, uint8_t cipher_flags) {
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

ap_entry_t *upsert_ap_entry(const uint8_t bssid[6], const char *ssid, uint8_t ssid_len,
                            uint8_t auth_mode, uint8_t proto_flags, uint8_t akm_flags,
                            uint8_t cipher_flags, uint8_t channel, int8_t rssi, uint32_t ts_ms,
                            bool *is_new, bool *is_updated) {
  int idx = find_ap_entry(bssid);
  *is_new = false;
  *is_updated = false;

  if (idx < 0) {
    idx = pick_entry_slot();
    memset(&s_seen[idx], 0, sizeof(s_seen[idx]));
    memcpy(s_seen[idx].bssid, bssid, 6);
    s_seen[idx].in_use = true;
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
    } else if (entry->ssid_len != ssid_len || memcmp(entry->ssid, ssid, ssid_len) != 0) {
      // Keep latest non-empty SSID for this BSSID.
      accept_ssid = true;
    }
  }

  bool ssid_changed = false;
  if (accept_ssid) {
    memcpy(entry->ssid, ssid, ssid_len);
    entry->ssid[ssid_len] = '\0';
    entry->ssid_len = ssid_len;
    ssid_changed = true;
  }

  if (entry->channel != channel || entry->rssi != rssi || entry->auth_mode != merged_auth ||
      entry->proto_flags != merged_proto || entry->akm_flags != merged_akm ||
      entry->cipher_flags != merged_cipher || ssid_changed) {
    *is_updated = true;
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

bool should_emit_event(ap_entry_t *entry, bool is_new, bool is_updated, uint32_t ts_ms) {
  if (is_new || is_updated) {
    if (entry->last_notified_ms == 0 || ts_ms - entry->last_notified_ms >= 1000) {
      entry->last_notified_ms = ts_ms;
      return true;
    }
  }

  if (entry->last_notified_ms == 0 ||
      ts_ms - entry->last_notified_ms >= WG_SIGHTING_NOTIFY_INTERVAL_MS) {
    entry->last_notified_ms = ts_ms;
    return true;
  }
  return false;
}

bool enqueue_event(const sighting_event_t &evt) {
  uint8_t next = (uint8_t)((s_event_head + 1U) % WG_QUEUE_CAPACITY);
  if (next == s_event_tail) {
    s_event_drop_count++;
    return false;
  }
  memcpy(&s_event_queue[s_event_head], &evt, sizeof(evt));
  s_event_head = next;
  return true;
}

bool dequeue_event(sighting_event_t *out) {
  if (s_event_head == s_event_tail) {
    return false;
  }
  memcpy(out, &s_event_queue[s_event_tail], sizeof(*out));
  s_event_tail = (uint8_t)((s_event_tail + 1U) % WG_QUEUE_CAPACITY);
  return true;
}

void rate_counter_tick(uint32_t now_ms) {
  if (s_rate_window_start_ms == 0) {
    s_rate_window_start_ms = now_ms;
    s_packets_per_sec = 0;
    s_rate_packet_acc = 0;
    return;
  }

  uint32_t elapsed = now_ms - s_rate_window_start_ms;
  if (elapsed < 1000U) {
    return;
  }
  s_packets_per_sec = (uint16_t)((s_rate_packet_acc * 1000U) / elapsed);
  s_rate_packet_acc = 0;
  s_rate_window_start_ms = now_ms;
}

void set_status_led(bool on) {
  s_led_is_on = on;
  bool electrical_high = WG_STATUS_LED_ACTIVE_LOW ? !on : on;
  const uint8_t level = electrical_high ? HIGH : LOW;
  digitalWrite(WG_STATUS_LED_PRIMARY_GPIO, level);
  if (WG_STATUS_LED_FALLBACK_GPIO != WG_STATUS_LED_PRIMARY_GPIO) {
    digitalWrite(WG_STATUS_LED_FALLBACK_GPIO, level);
  }
}

void update_status_led(uint32_t now_ms) {
  bool next_on = false;
  if (!s_link_established) {
    // Waiting for host link: slow heartbeat.
    next_on = ((now_ms / 500U) % 2U) == 0U;
  } else {
    bool activity_recent = (now_ms - s_led_last_activity_ms) <= 180U;
    if (activity_recent) {
      // Link established + UART activity: fast pulse.
      next_on = ((now_ms / 50U) % 2U) == 0U;
    } else {
      // Link established idle: solid.
      next_on = true;
    }
  }

  if (next_on != s_led_is_on) {
    set_status_led(next_on);
  }
}

bool node_write_frame(uint8_t type, const uint8_t *payload, uint16_t payload_len) {
  if (payload_len > WG_NODE_FRAME_MAX_PAYLOAD) {
    return false;
  }

  uint8_t frame_hdr[2 + WG_NODE_FRAME_HEADER_SIZE] = {0};
  frame_hdr[0] = WG_NODE_FRAME_SYNC0;
  frame_hdr[1] = WG_NODE_FRAME_SYNC1;
  frame_hdr[2] = WG_NODE_FRAME_VERSION;
  frame_hdr[3] = type;
  wr_u16_le(&frame_hdr[4], s_tx_seq);
  wr_u16_le(&frame_hdr[6], payload_len);
  wr_u32_le(&frame_hdr[8], millis());
  s_tx_seq++;

  uint16_t crc = crc16_ccitt(&frame_hdr[2], WG_NODE_FRAME_HEADER_SIZE);
  if (payload_len > 0 && payload != nullptr) {
    crc = crc16_ccitt_seed(crc, payload, payload_len);
  }
  uint8_t crc_bytes[2];
  wr_u16_le(crc_bytes, crc);

  size_t wrote = 0;
  wrote += Serial.write(frame_hdr, sizeof(frame_hdr));
  if (payload_len > 0 && payload != nullptr) {
    wrote += Serial.write(payload, payload_len);
  }
  wrote += Serial.write(crc_bytes, sizeof(crc_bytes));

  bool ok = wrote == (size_t)(sizeof(frame_hdr) + payload_len + sizeof(crc_bytes));
  if (ok) {
    s_led_last_activity_ms = millis();
  }
  return ok;
}

bool send_hello_ack() {
  const uint8_t payload[1] = {1};
  return node_write_frame(WG_NODE_MSG_HELLO_ACK, payload, sizeof(payload));
}

bool send_pong() { return node_write_frame(WG_NODE_MSG_PONG, nullptr, 0); }

bool send_status() {
  uint8_t payload[10] = {0};
  payload[0] = (s_scanning ? 1U : 0U) | (s_report_enabled ? 2U : 0U);
  payload[1] = s_current_channel;
  wr_u16_le(&payload[2], s_channel_mask);
  wr_u16_le(&payload[4], s_hop_ms);
  wr_u16_le(&payload[6], s_packets_per_sec);
  uint16_t drops = (uint16_t)((s_event_drop_count > 0xFFFFU) ? 0xFFFFU : s_event_drop_count);
  wr_u16_le(&payload[8], drops);
  return node_write_frame(WG_NODE_MSG_STATUS, payload, sizeof(payload));
}

bool send_sighting(const sighting_event_t &evt) {
  uint8_t ssid_len = evt.ssid_len;
  if (ssid_len > 32) {
    ssid_len = 32;
  }

  uint8_t payload[64] = {0};
  memcpy(payload, evt.bssid, 6);
  payload[6] = evt.channel;
  payload[7] = (uint8_t)evt.rssi;
  payload[8] = evt.auth_mode;
  payload[9] = evt.proto_flags;
  payload[10] = evt.akm_flags;
  payload[11] = evt.cipher_flags;
  payload[12] = ssid_len;
  if (ssid_len > 0) {
    memcpy(&payload[13], evt.ssid, ssid_len);
  }
  payload[13 + ssid_len] = evt.flags;

  const uint16_t total = (uint16_t)(14 + ssid_len);
  return node_write_frame(WG_NODE_MSG_SIGHTING, payload, total);
}

void stop_scanning() {
  if (!s_scanning) {
    return;
  }
  wifi_promiscuous_enable(0);
  s_scanning = false;
}

void start_scanning() {
  if (s_scanning) {
    return;
  }
  if (!channel_mask_is_valid(s_channel_mask)) {
    return;
  }
  s_current_channel = pick_first_channel(s_channel_mask);
  wifi_set_channel(s_current_channel);
  wifi_promiscuous_enable(1);
  s_last_hop_ms = millis();
  s_scanning = true;
}

void apply_scan_enable() {
  if (s_scan_enabled) {
    start_scanning();
  } else {
    stop_scanning();
    s_report_enabled = false;
  }
}

void maybe_hop_channel(uint32_t now_ms) {
  if (!s_scanning) {
    return;
  }
  if (now_ms - s_last_hop_ms < s_hop_ms) {
    return;
  }
  s_last_hop_ms = now_ms;
  s_current_channel = next_enabled_channel(s_channel_mask, s_current_channel);
  wifi_set_channel(s_current_channel);
}

void handle_config(const uint8_t *payload, uint16_t payload_len) {
  if (payload_len < 5) {
    return;
  }

  const uint8_t flags = payload[0];
  s_scan_enabled = (flags & 0x01U) != 0U;
  s_report_enabled = (flags & 0x02U) != 0U;
  if (payload_len >= 6) {
    s_report_enabled = payload[5] != 0;
  }
  if (!s_scan_enabled) {
    s_report_enabled = false;
  }

  uint16_t hop_ms = rd_u16_le(&payload[1]);
  if (hop_ms < WG_MIN_HOP_MS) {
    hop_ms = WG_MIN_HOP_MS;
  }
  if (hop_ms > WG_MAX_HOP_MS) {
    hop_ms = WG_MAX_HOP_MS;
  }
  s_hop_ms = hop_ms;

  uint16_t mask = rd_u16_le(&payload[3]);
  if (channel_mask_is_valid(mask)) {
    s_channel_mask = mask;
  }

  apply_scan_enable();
}

void handle_node_frame(const uint8_t *header, const uint8_t *payload, uint16_t payload_len) {
  const uint8_t type = header[1];
  s_last_rx_ms = millis();
  s_link_established = true;

  switch (type) {
    case WG_NODE_MSG_HELLO:
      send_hello_ack();
      send_status();
      break;
    case WG_NODE_MSG_CONFIG:
      handle_config(payload, payload_len);
      send_status();
      break;
    case WG_NODE_MSG_PING:
      send_pong();
      break;
    default:
      break;
  }
}

void consume_rx_byte(uint8_t b) {
  switch (s_rx_state) {
    case 0:
      if (b == WG_NODE_FRAME_SYNC0) {
        s_rx_state = 1;
      }
      break;

    case 1:
      if (b == WG_NODE_FRAME_SYNC1) {
        s_rx_state = 2;
        s_rx_hdr_pos = 0;
      } else {
        s_rx_state = 0;
      }
      break;

    case 2:
      s_rx_hdr[s_rx_hdr_pos++] = b;
      if (s_rx_hdr_pos >= WG_NODE_FRAME_HEADER_SIZE) {
        if (s_rx_hdr[0] != WG_NODE_FRAME_VERSION) {
          s_rx_state = 0;
          return;
        }
        s_rx_payload_len = rd_u16_le(&s_rx_hdr[4]);
        if (s_rx_payload_len > WG_NODE_FRAME_MAX_PAYLOAD) {
          s_rx_state = 0;
          return;
        }
        s_rx_payload_pos = 0;
        s_rx_crc_pos = 0;
        s_rx_state = (s_rx_payload_len == 0) ? 4 : 3;
      }
      break;

    case 3:
      s_rx_payload[s_rx_payload_pos++] = b;
      if (s_rx_payload_pos >= s_rx_payload_len) {
        s_rx_state = 4;
      }
      break;

    case 4:
      s_rx_crc_bytes[s_rx_crc_pos++] = b;
      if (s_rx_crc_pos >= 2) {
        uint16_t rx_crc = rd_u16_le(s_rx_crc_bytes);
        uint16_t calc = crc16_ccitt(s_rx_hdr, WG_NODE_FRAME_HEADER_SIZE);
        if (s_rx_payload_len > 0) {
          calc = crc16_ccitt_seed(calc, s_rx_payload, s_rx_payload_len);
        }
        if (calc == rx_crc) {
          handle_node_frame(s_rx_hdr, s_rx_payload, s_rx_payload_len);
        }
        s_rx_state = 0;
      }
      break;

    default:
      s_rx_state = 0;
      break;
  }
}

void drain_host_uart_rx() {
  bool got_bytes = false;
  while (Serial.available() > 0) {
    got_bytes = true;
    consume_rx_byte((uint8_t)Serial.read());
  }
  if (got_bytes) {
    s_led_last_activity_ms = millis();
  }
}

void drain_event_queue() {
  if (!s_scanning) {
    return;
  }
  sighting_event_t evt = {};
  while (dequeue_event(&evt)) {
    if (!s_report_enabled) {
      continue;
    }
    send_sighting(evt);
  }
}

void ICACHE_FLASH_ATTR sniffer_callback(uint8_t *buf, uint16_t len) {
  if (!s_scanning || buf == nullptr) {
    return;
  }
  if (len <= sizeof(sniffer_rx_ctrl_t)) {
    return;
  }

  const sniffer_rx_ctrl_t *rx = reinterpret_cast<const sniffer_rx_ctrl_t *>(buf);
  const uint8_t *payload = buf + sizeof(sniffer_rx_ctrl_t);
  const uint16_t payload_len = (uint16_t)(len - sizeof(sniffer_rx_ctrl_t));
  if (payload_len < 36) {
    return;
  }
  if (!is_interesting_mgmt(payload[0])) {
    return;
  }

  s_rate_packet_acc++;
  const uint32_t ts_ms = millis();

  char ssid[33] = {0};
  uint8_t ssid_len = 0;
  uint8_t auth_mode = WG_AUTH_UNKNOWN;
  uint8_t proto_flags = 0;
  uint8_t akm_flags = 0;
  uint8_t cipher_flags = 0;
  parse_beacon_probe(payload, payload_len, ssid, &ssid_len, &auth_mode, &proto_flags, &akm_flags,
                     &cipher_flags);

  const uint8_t *bssid = &payload[16];
  bool is_new = false;
  bool is_updated = false;
  ap_entry_t *entry =
      upsert_ap_entry(bssid, ssid, ssid_len, auth_mode, proto_flags, akm_flags, cipher_flags,
                      rx->channel, (int8_t)rx->rssi, ts_ms, &is_new, &is_updated);
  if (entry == nullptr) {
    return;
  }
  if (!should_emit_event(entry, is_new, is_updated, ts_ms)) {
    return;
  }

  sighting_event_t evt = {};
  memcpy(evt.bssid, entry->bssid, 6);
  evt.ssid_len = entry->ssid_len;
  if (evt.ssid_len > 0) {
    memcpy(evt.ssid, entry->ssid, evt.ssid_len);
    evt.ssid[evt.ssid_len] = '\0';
  }
  evt.auth_mode = entry->auth_mode;
  evt.proto_flags = entry->proto_flags;
  evt.akm_flags = entry->akm_flags;
  evt.cipher_flags = entry->cipher_flags;
  evt.channel = entry->channel;
  evt.rssi = entry->rssi;
  if (is_new) {
    evt.flags |= WG_SIGHTING_FLAG_NEW;
  }
  if (is_updated) {
    evt.flags |= WG_SIGHTING_FLAG_UPDATED;
  }
  enqueue_event(evt);
}

void setup_wifi_sniffer() {
  wifi_set_opmode(STATION_MODE);
  wifi_promiscuous_enable(0);
  s_current_channel = pick_first_channel(s_channel_mask);
  wifi_set_channel(s_current_channel);
  wifi_set_promiscuous_rx_cb(sniffer_callback);
  wifi_promiscuous_enable(0);
}

}  // namespace

void setup() {
  Serial.begin(WG_UART_BAUD);
  Serial.setDebugOutput(false);

  pinMode(WG_STATUS_LED_PRIMARY_GPIO, OUTPUT);
  if (WG_STATUS_LED_FALLBACK_GPIO != WG_STATUS_LED_PRIMARY_GPIO) {
    pinMode(WG_STATUS_LED_FALLBACK_GPIO, OUTPUT);
  }
  set_status_led(false);
  Serial.printf("status LED primary=GPIO%u fallback=GPIO%u active=%s\n", WG_STATUS_LED_PRIMARY_GPIO,
                WG_STATUS_LED_FALLBACK_GPIO, WG_STATUS_LED_ACTIVE_LOW ? "low" : "high");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  setup_wifi_sniffer();
  s_last_status_ms = millis();
}

void loop() {
  const uint32_t now_ms = millis();

  drain_host_uart_rx();
  rate_counter_tick(now_ms);
  maybe_hop_channel(now_ms);
  drain_event_queue();

  if (s_link_established) {
    if ((now_ms - s_last_status_ms) >= WG_NODE_STATUS_INTERVAL_MS) {
      send_status();
      s_last_status_ms = now_ms;
    }
    if ((now_ms - s_last_rx_ms) > WG_NODE_LINK_TIMEOUT_MS) {
      s_link_established = false;
    }
  }

  update_status_led(now_ms);

  yield();
}
