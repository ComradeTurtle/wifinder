#include <Arduino.h>
#include <WiFi.h>
#include <wifi_conf.h>
#include <wifi_drv.h>

#include <ctype.h>
#include <string.h>

namespace {

// BW16 uses LOG UART on PA7/PA8 for the node link.
#define WG_NODE_SERIAL Serial

constexpr uint8_t WG_NODE_FRAME_SYNC0 = 0xA5;
constexpr uint8_t WG_NODE_FRAME_SYNC1 = 0x5A;
constexpr uint8_t WG_NODE_FRAME_VERSION = 1;
constexpr uint8_t WG_NODE_FRAME_HEADER_SIZE = 10;
constexpr uint16_t WG_NODE_FRAME_MAX_PAYLOAD = 96;

constexpr uint16_t WG_DEFAULT_CHANNEL_MASK = 0x1FFF;
constexpr uint8_t WG_NODE_5GHZ_MASK_BIT_COUNT = 43;
constexpr uint64_t WG_NODE_5GHZ_MASK_ALL = (1ULL << WG_NODE_5GHZ_MASK_BIT_COUNT) - 1ULL;
constexpr uint16_t WG_DEFAULT_HOP_MS = 250;
constexpr uint16_t WG_MIN_HOP_MS = 50;
constexpr uint16_t WG_MAX_HOP_MS = 2000;

constexpr uint32_t WG_NODE_STATUS_INTERVAL_MS = 1000;
constexpr uint32_t WG_NODE_LINK_TIMEOUT_MS = 6000;
constexpr uint32_t WG_SIGHTING_NOTIFY_INTERVAL_MS = 3000;
constexpr uint32_t WG_SCAN_RETRY_BACKOFF_MS = 250;
constexpr int WG_SCAN_BUF_LEN = 8192;

constexpr uint16_t WG_AP_CAPACITY = 320;
constexpr uint16_t WG_QUEUE_CAPACITY = 192;
constexpr uint16_t WG_QUEUE_SCAN_BACKPRESSURE_HIGH = (WG_QUEUE_CAPACITY * 3U) / 4U;
constexpr uint16_t WG_QUEUE_SCAN_BACKPRESSURE_LOW = WG_QUEUE_CAPACITY / 2U;

constexpr uint32_t WG_UART_BAUD = 115200;

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

ap_entry_t s_seen[WG_AP_CAPACITY] = {};
sighting_event_t s_event_queue[WG_QUEUE_CAPACITY] = {};
volatile uint16_t s_event_head = 0;
volatile uint16_t s_event_tail = 0;
volatile uint32_t s_event_drop_count = 0;

uint16_t s_hop_ms = WG_DEFAULT_HOP_MS;
uint16_t s_channel_mask = WG_DEFAULT_CHANNEL_MASK;
uint64_t s_channel_mask_5ghz = WG_NODE_5GHZ_MASK_ALL;
uint8_t s_current_channel = 1;
bool s_scan_paused_for_backpressure = false;
bool s_scan_task_running = false;

volatile bool s_scan_enabled = false;
volatile bool s_scanning = false;
volatile bool s_report_enabled = false;
volatile bool s_link_established = false;

volatile uint32_t s_next_scan_earliest_ms = 0;
uint32_t s_last_status_ms = 0;
uint32_t s_last_rx_ms = 0;

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

uint16_t rd_u16_le(const uint8_t *in) { return (uint16_t)in[0] | ((uint16_t)in[1] << 8); }

uint64_t rd_u64_le(const uint8_t *in) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value |= ((uint64_t)in[i] << (8U * i));
  }
  return value;
}

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

void wr_u64_le(uint8_t *out, uint64_t v) {
  for (size_t i = 0; i < 8; ++i) {
    out[i] = (uint8_t)((v >> (8U * i)) & 0xFFU);
  }
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

bool channel_mask_24_format_is_valid(uint16_t mask) { return (mask & (uint16_t)(~0x1FFFU)) == 0U; }

bool channel_mask_5ghz_is_valid(uint64_t mask) { return (mask & ~WG_NODE_5GHZ_MASK_ALL) == 0ULL; }

int8_t channel_to_5ghz_bit(uint8_t channel) {
  if (channel >= 32 && channel <= 176 && ((channel - 32U) % 4U) == 0U) {
    return (int8_t)((channel - 32U) / 4U);
  }
  if (channel == 177) {
    return 37;
  }
  if (channel >= 182 && channel <= 196 && ((channel - 182U) % 4U) == 0U) {
    return (int8_t)(38 + ((channel - 182U) / 4U));
  }
  return -1;
}

bool is_24ghz_channel(uint8_t channel) { return channel >= 1 && channel <= 14; }

bool is_5ghz_channel(uint8_t channel) { return channel_to_5ghz_bit(channel) >= 0; }

bool channel_allowed(uint8_t channel) {
  if (is_24ghz_channel(channel)) {
    if (channel >= 1 && channel <= 13) {
      const uint16_t bit = (uint16_t)(1U << (channel - 1));
      return (s_channel_mask & bit) != 0;
    }
    return true;
  }
  if (is_5ghz_channel(channel)) {
    const int8_t bit = channel_to_5ghz_bit(channel);
    if (bit < 0) {
      return false;
    }
    return ((s_channel_mask_5ghz >> bit) & 1ULL) != 0ULL;
  }
  return false;
}

uint8_t pick_first_channel(uint16_t mask) {
  for (uint8_t ch = 1; ch <= 13; ++ch) {
    if ((mask & (uint16_t)(1U << (ch - 1U))) != 0U) {
      return ch;
    }
  }
  return 1;
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

int find_ap_entry(const uint8_t bssid[6]) {
  for (uint16_t i = 0; i < WG_AP_CAPACITY; ++i) {
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
  for (uint16_t i = 0; i < WG_AP_CAPACITY; ++i) {
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
    if (entry->last_notified_ms == 0 || ts_ms - entry->last_notified_ms >= 1000U) {
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

uint16_t event_queue_count(void) {
  uint16_t head = 0;
  uint16_t tail = 0;
  noInterrupts();
  head = s_event_head;
  tail = s_event_tail;
  interrupts();
  if (head >= tail) {
    return (uint16_t)(head - tail);
  }
  return (uint16_t)(WG_QUEUE_CAPACITY - (tail - head));
}

bool enqueue_event(const sighting_event_t &evt) {
  bool ok = false;
  noInterrupts();
  uint16_t next = (uint16_t)(s_event_head + 1U);
  if (next >= WG_QUEUE_CAPACITY) {
    next = 0;
  }
  if (next == s_event_tail) {
    s_event_drop_count++;
  } else {
    memcpy(&s_event_queue[s_event_head], &evt, sizeof(evt));
    s_event_head = next;
    ok = true;
  }
  interrupts();
  return ok;
}

bool peek_event(sighting_event_t *out) {
  bool ok = false;
  noInterrupts();
  if (s_event_head != s_event_tail) {
    memcpy(out, &s_event_queue[s_event_tail], sizeof(*out));
    ok = true;
  }
  interrupts();
  return ok;
}

bool pop_event(void) {
  bool ok = false;
  noInterrupts();
  if (s_event_head != s_event_tail) {
    s_event_tail = (uint16_t)(s_event_tail + 1U);
    if (s_event_tail >= WG_QUEUE_CAPACITY) {
      s_event_tail = 0;
    }
    ok = true;
  }
  interrupts();
  return ok;
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
  wrote += WG_NODE_SERIAL.write(frame_hdr, sizeof(frame_hdr));
  if (payload_len > 0 && payload != nullptr) {
    wrote += WG_NODE_SERIAL.write(payload, payload_len);
  }
  wrote += WG_NODE_SERIAL.write(crc_bytes, sizeof(crc_bytes));

  return wrote == (size_t)(sizeof(frame_hdr) + payload_len + sizeof(crc_bytes));
}

bool send_hello_ack() {
  const uint8_t payload[1] = {1};
  return node_write_frame(WG_NODE_MSG_HELLO_ACK, payload, sizeof(payload));
}

bool send_pong() { return node_write_frame(WG_NODE_MSG_PONG, nullptr, 0); }

bool send_status() {
  uint8_t payload[18] = {0};
  payload[0] = (s_scanning ? 1U : 0U) | (s_report_enabled ? 2U : 0U);
  payload[1] = s_current_channel;
  wr_u16_le(&payload[2], s_channel_mask);
  wr_u16_le(&payload[4], s_hop_ms);
  wr_u16_le(&payload[6], s_packets_per_sec);
  uint16_t drops = (uint16_t)((s_event_drop_count > 0xFFFFU) ? 0xFFFFU : s_event_drop_count);
  wr_u16_le(&payload[8], drops);
  wr_u64_le(&payload[10], s_channel_mask_5ghz);
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
  s_scanning = false;
  s_next_scan_earliest_ms = 0;
  s_scan_paused_for_backpressure = false;
}

void start_scanning() {
  if (!channel_mask_is_valid(s_channel_mask) && s_channel_mask_5ghz == 0ULL) {
    return;
  }
  if (channel_mask_is_valid(s_channel_mask)) {
    s_current_channel = pick_first_channel(s_channel_mask);
  } else {
    s_current_channel = 0;
  }
  // BW16 runs full-band scans, so the instantaneous channel is not meaningful in status.
  s_current_channel = 0;
  s_next_scan_earliest_ms = 0;
  s_scanning = true;
  s_scan_paused_for_backpressure = false;
}

void apply_scan_enable() {
  if (s_scan_enabled) {
    start_scanning();
  } else {
    stop_scanning();
    s_report_enabled = false;
  }
}

void handle_config(const uint8_t *payload, uint16_t payload_len) {
  if (payload_len < 5) {
    return;
  }

  const uint8_t flags = payload[0];
  s_scan_enabled = (flags & 0x01U) != 0U;
  s_report_enabled = (flags & 0x02U) != 0U;
  if (payload_len == 6) {
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
  if (channel_mask_24_format_is_valid(mask)) {
    s_channel_mask = mask;
  }
  if (payload_len >= 13) {
    uint64_t mask5 = rd_u64_le(&payload[5]);
    if (channel_mask_5ghz_is_valid(mask5)) {
      s_channel_mask_5ghz = mask5;
    }
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
  while (WG_NODE_SERIAL.available() > 0) {
    consume_rx_byte((uint8_t)WG_NODE_SERIAL.read());
  }
}

void drain_event_queue() {
  if (!s_scanning) {
    return;
  }
  sighting_event_t evt = {};
  while (peek_event(&evt)) {
    if (!s_report_enabled) {
      (void)pop_event();
      continue;
    }
    if (!send_sighting(evt)) {
      break;
    }
    (void)pop_event();
  }
}

#ifndef ENTERPRISE_ENABLED
#define ENTERPRISE_ENABLED 0x0020
#endif

void map_security_type(uint32_t sec, uint8_t *auth_mode, uint8_t *proto_flags, uint8_t *akm_flags,
                       uint8_t *cipher_flags) {
  *auth_mode = WG_AUTH_UNKNOWN;
  *proto_flags = 0;
  *akm_flags = 0;
  *cipher_flags = 0;

  if (sec == SECURITY_OPEN) {
    *auth_mode = WG_AUTH_OPEN;
    return;
  }

  if ((sec & WEP_ENABLED) != 0) {
    *auth_mode = WG_AUTH_WEP;
    *cipher_flags = WG_SEC_CIPHER_WEP;
    return;
  }

  if ((sec & WPA_SECURITY) != 0) {
    *proto_flags |= WG_SEC_PROTO_WPA;
  }
  if ((sec & WPA2_SECURITY) != 0) {
    *proto_flags |= WG_SEC_PROTO_WPA2;
  }
  if ((sec & WPA3_SECURITY) != 0) {
    *proto_flags |= WG_SEC_PROTO_WPA3;
  }

  if ((sec & TKIP_ENABLED) != 0) {
    *cipher_flags |= WG_SEC_CIPHER_TKIP;
  }
  if ((sec & AES_ENABLED) != 0) {
    *cipher_flags |= WG_SEC_CIPHER_CCMP_128;
  }

  if ((sec & ENTERPRISE_ENABLED) != 0) {
    *akm_flags |= WG_SEC_AKM_EAP;
  } else if ((sec & WPA3_SECURITY) != 0) {
    // WPA3-Personal networks use SAE.
    *akm_flags |= WG_SEC_AKM_SAE;
    if ((sec & (WPA_SECURITY | WPA2_SECURITY)) != 0) {
      *akm_flags |= WG_SEC_AKM_PSK;
    }
  } else if ((sec & (WPA_SECURITY | WPA2_SECURITY)) != 0) {
    *akm_flags |= WG_SEC_AKM_PSK;
  }

  *auth_mode = resolve_auth_mode(*auth_mode, *proto_flags, *cipher_flags);
}

bool copy_ssid_if_clean_ascii_buf(const uint8_t *in, size_t in_len, char *out, uint8_t *out_len) {
  size_t len = in_len;
  if (len > 32) {
    len = 32;
  }
  for (size_t i = 0; i < len; ++i) {
    unsigned char ch = in[i];
    if (!isprint(ch)) {
      out[0] = '\0';
      *out_len = 0;
      return false;
    }
    out[i] = (char)ch;
  }
  out[len] = '\0';
  *out_len = (uint8_t)len;
  return true;
}

void process_scan_record(const rtw_scan_result_t *record, uint32_t ts_ms) {
  if (record == nullptr) {
    return;
  }

  if (record->channel > 255U) {
    return;
  }
  uint8_t channel = (uint8_t)record->channel;
  if (!channel_allowed(channel)) {
    return;
  }

  uint8_t bssid[6] = {0};
  memcpy(bssid, record->BSSID.octet, sizeof(bssid));

  char ssid[33] = {0};
  uint8_t ssid_len = 0;
  (void)copy_ssid_if_clean_ascii_buf(record->SSID.val, record->SSID.len, ssid, &ssid_len);

  int8_t rssi = (record->signal_strength < -128) ? -128
               : (record->signal_strength > 127) ? 127
                                                 : (int8_t)record->signal_strength;

  uint8_t auth_mode = WG_AUTH_UNKNOWN;
  uint8_t proto_flags = 0;
  uint8_t akm_flags = 0;
  uint8_t cipher_flags = 0;
  map_security_type((uint32_t)record->security, &auth_mode, &proto_flags, &akm_flags, &cipher_flags);

  bool is_new = false;
  bool is_updated = false;
  ap_entry_t *entry = upsert_ap_entry(bssid, ssid, ssid_len, auth_mode, proto_flags, akm_flags,
                                      cipher_flags, channel, rssi, ts_ms, &is_new, &is_updated);
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
  (void)enqueue_event(evt);
  s_rate_packet_acc++;
}

uint32_t s_scan_ts_ms = 0;

static inline int32_t rd_i32_le(const uint8_t *p) {
  return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                   ((uint32_t)p[3] << 24));
}

static inline uint32_t rd_u32_le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

void process_scan_buf_entry(const uint8_t *entry, uint8_t len, uint32_t ts_ms) {
  if (entry == nullptr || len == 0) {
    return;
  }
  const uint8_t base_common = 1 + 6 + 4 + 1 + 1 + 1;
  const uint8_t base_extended = 1 + 6 + 4 + 4 + 1 + 1;
  if (len <= base_common) {
    return;
  }

  uint8_t security_bytes = 1;
  uint8_t payload_after_header = (uint8_t)(len - base_common);
  if (len > base_extended) {
    uint8_t ext_payload = (uint8_t)(len - base_extended);
    if (ext_payload <= 32) {
      security_bytes = 4;
      payload_after_header = ext_payload;
    }
  }

  if (payload_after_header > 32) {
    return;
  }

  const uint8_t mac_off = 1;
  const uint8_t rssi_off = (uint8_t)(mac_off + 6);
  const uint8_t sec_off = (uint8_t)(rssi_off + 4);
  const uint8_t wps_off = (uint8_t)(sec_off + security_bytes);
  const uint8_t chan_off = (uint8_t)(wps_off + 1);
  const uint8_t ssid_off = (uint8_t)(chan_off + 1);
  const uint8_t ssid_len = payload_after_header;

  rtw_scan_result_t record = {};
  memcpy(record.BSSID.octet, &entry[mac_off], 6);
  record.signal_strength = rd_i32_le(&entry[rssi_off]);
  record.security = (security_bytes == 4) ? rd_u32_le(&entry[sec_off]) : entry[sec_off];
  record.channel = entry[chan_off];
  record.SSID.len = ssid_len;
  if (ssid_len > 0) {
    memcpy(record.SSID.val, &entry[ssid_off], ssid_len);
  }
  if (record.SSID.len < sizeof(record.SSID.val)) {
    record.SSID.val[record.SSID.len] = 0;
  } else {
    record.SSID.val[sizeof(record.SSID.val) - 1] = 0;
  }
  process_scan_record(&record, ts_ms);
}

void run_scan_once(uint32_t now_ms) {
  static uint8_t scan_buf_mem[WG_SCAN_BUF_LEN] = {0};
  scan_buf_arg scan_buf = {
      .buf = reinterpret_cast<char *>(scan_buf_mem),
      .buf_len = WG_SCAN_BUF_LEN,
  };
  memset(scan_buf_mem, 0, sizeof(scan_buf_mem));

  s_scan_ts_ms = now_ms;
  s_current_channel = 0;
  if (wifi_scan(RTW_SCAN_TYPE_PASSIVE, RTW_BSS_TYPE_ANY, &scan_buf) < 0) {
    s_next_scan_earliest_ms = now_ms + WG_SCAN_RETRY_BACKOFF_MS;
    return;
  }
  s_next_scan_earliest_ms = 0;

  int plen = 0;
  while (plen < scan_buf.buf_len) {
    const uint8_t len = static_cast<uint8_t>(scan_buf_mem[plen]);
    if (len == 0) {
      break;
    }
    if ((plen + len) > scan_buf.buf_len) {
      break;
    }
    process_scan_buf_entry(&scan_buf_mem[plen], len, s_scan_ts_ms);
    plen += len;
  }
}

void maybe_run_scan(uint32_t now_ms) {
  if (!s_scanning) {
    return;
  }
  const uint16_t queued = event_queue_count();
  if (s_scan_paused_for_backpressure) {
    if (queued > WG_QUEUE_SCAN_BACKPRESSURE_LOW) {
      return;
    }
    s_scan_paused_for_backpressure = false;
  } else if (queued >= WG_QUEUE_SCAN_BACKPRESSURE_HIGH) {
    s_scan_paused_for_backpressure = true;
    return;
  }
  if ((int32_t)(now_ms - s_next_scan_earliest_ms) < 0) {
    return;
  }
  run_scan_once(now_ms);
}

void scan_task(const void *arg) {
  (void)arg;
  while (true) {
    maybe_run_scan(millis());
    delay(1);
  }
}

}  // namespace

void setup() {
  WG_NODE_SERIAL.begin(WG_UART_BAUD);

  WiFiDrv::wifiDriverInit();
  WiFi.disconnect();

  s_current_channel = pick_first_channel(s_channel_mask);
  s_last_status_ms = millis();
  if (os_thread_create_arduino(scan_task, nullptr, OS_PRIORITY_ABOVENORMAL, 2048) != 0) {
    s_scan_task_running = true;
  }
}

void loop() {
  const uint32_t now_ms = millis();

  drain_host_uart_rx();
  rate_counter_tick(now_ms);
  if (!s_scan_task_running) {
    maybe_run_scan(now_ms);
  }
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

  delay(1);
}
