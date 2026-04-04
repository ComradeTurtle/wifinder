#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wg_payload.h"

static int tests_run = 0;

static void assert_true(bool condition, const char *message) {
  tests_run++;
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static void assert_u8(uint8_t actual, uint8_t expected, const char *message) {
  tests_run++;
  if (actual != expected) {
    fprintf(stderr, "FAIL: %s (actual=%u expected=%u)\n", message, actual, expected);
    exit(1);
  }
}

static void assert_u16(uint16_t actual, uint16_t expected, const char *message) {
  tests_run++;
  if (actual != expected) {
    fprintf(stderr, "FAIL: %s (actual=%u expected=%u)\n", message, actual, expected);
    exit(1);
  }
}

static void assert_u32(uint32_t actual, uint32_t expected, const char *message) {
  tests_run++;
  if (actual != expected) {
    fprintf(stderr, "FAIL: %s (actual=%u expected=%u)\n", message, actual, expected);
    exit(1);
  }
}

static void assert_u64(uint64_t actual, uint64_t expected, const char *message) {
  tests_run++;
  if (actual != expected) {
    fprintf(stderr, "FAIL: %s (actual=%llu expected=%llu)\n", message,
            (unsigned long long)actual, (unsigned long long)expected);
    exit(1);
  }
}

static uint16_t rd_u16(const uint8_t *in) { return (uint16_t)in[0] | ((uint16_t)in[1] << 8); }
static uint32_t rd_u32(const uint8_t *in) {
  return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
         ((uint32_t)in[3] << 24);
}
static uint64_t rd_u64(const uint8_t *in) {
  return (uint64_t)rd_u32(in) | ((uint64_t)rd_u32(in + 4) << 32);
}

static void test_status_payload_encoding(void) {
  wg_status_payload_t status = {
      .scanning = true,
      .ble_encrypted = true,
      .current_channel = 6,
      .hop_ms = 250,
      .channel_mask = 0x1001,
      .unique_bssids_estimate = 70000,
      .packets_per_sec = 120,
      .dropped_notifies = 2,
      .boot_mode = WG_BOOT_AUTO,
      .gps_valid = true,
      .gps_age_s = 7,
      .gps_accuracy_dm = 14,
      .node_link_up = true,
      .node_last_seen_s = 2,
      .node_packets_per_sec = 123,
      .node_forwarded_sightings = 77,
      .node_channel = 6,
      .node_channel_mask = 0x1554,
      .session_open = true,
      .session_id = 0x1122334455667788ULL,
      .queued_records = 314,
      .queued_bytes = 8192,
      .replay_active = true,
      .replay_cursor = 271,
      .queue_full = false,
      .dropped_flash_full = 7,
      .node_count = 2,
      .gps_nav_applied_hz = 4,
      .spiffs_total_bytes = 16 * 1024 * 1024,
      .spiffs_used_bytes = 9 * 1024 * 1024,
      .spiffs_free_bytes = 7 * 1024 * 1024,
      .blob_active = true,
      .blob_session_id = 0x8877665544332211ULL,
      .blob_bytes_sent = 123456,
      .blob_bytes_total = 456789,
  };

  uint8_t out[WG_STATUS_PAYLOAD_SIZE] = {0};
  size_t written = wg_build_status_payload(&status, out, sizeof(out));
  assert_true(written == WG_STATUS_PAYLOAD_SIZE, "status payload should have fixed size");
  assert_u8(out[0], 1, "scanning flag should be encoded");
  assert_u8(out[1], 1, "ble_encrypted flag should be encoded");
  assert_u8(out[2], 6, "current channel should be encoded");
  assert_u16(rd_u16(&out[3]), 250, "hop interval should be little-endian");
  assert_u16(rd_u16(&out[5]), 0x1001, "channel mask should be little-endian");
  assert_u16(rd_u16(&out[7]), 0xFFFF, "legacy unique bssid count should clamp to u16");
  assert_u8(out[14], 1, "gps_valid flag should be encoded");
  assert_u16(rd_u16(&out[15]), 7, "gps age should be little-endian");
  assert_u16(rd_u16(&out[17]), 14, "gps accuracy should be little-endian");
  assert_u8(out[19], 1, "node_link_up should be encoded");
  assert_u16(rd_u16(&out[20]), 2, "node_last_seen_s should be little-endian");
  assert_u16(rd_u16(&out[22]), 123, "node_packets_per_sec should be little-endian");
  assert_u16(rd_u16(&out[24]), 77, "node_forwarded_sightings should be little-endian");
  assert_u8(out[26], 6, "node_channel should be encoded");
  assert_u16(rd_u16(&out[27]), 0x1554, "node_channel_mask should be little-endian");
  assert_u8(out[29], 1, "session_open should be encoded");
  assert_u64(rd_u64(&out[30]), 0x1122334455667788ULL, "session_id should be little-endian");
  assert_u32(rd_u32(&out[38]), 314, "queued_records should be little-endian");
  assert_u32(rd_u32(&out[42]), 8192, "queued_bytes should be little-endian");
  assert_u8(out[46], 1, "replay_active should be encoded");
  assert_u32(rd_u32(&out[47]), 271, "replay_cursor should be little-endian");
  assert_u8(out[51], 0, "queue_full should be encoded");
  assert_u32(rd_u32(&out[52]), 7, "dropped_flash_full should be little-endian");
  assert_u8(out[56], 2, "node_count should be encoded");
  assert_u32(rd_u32(&out[57]), 70000, "extended unique bssid estimate should be encoded");
  assert_u8(out[61], 4, "gps nav applied hz should be encoded");
  assert_u32(rd_u32(&out[62]), 16 * 1024 * 1024, "spiffs total bytes should be encoded");
  assert_u32(rd_u32(&out[66]), 9 * 1024 * 1024, "spiffs used bytes should be encoded");
  assert_u32(rd_u32(&out[70]), 7 * 1024 * 1024, "spiffs free bytes should be encoded");
  assert_u8(out[74], 1, "blob active should be encoded");
  assert_u64(rd_u64(&out[75]), 0x8877665544332211ULL, "blob session id should be encoded");
  assert_u32(rd_u32(&out[83]), 123456, "blob bytes sent should be encoded");
  assert_u32(rd_u32(&out[87]), 456789, "blob bytes total should be encoded");
}

static void test_sighting_payload_encoding(void) {
  wg_sighting_payload_t sighting = {
      .bssid = {0x7C, 0x9E, 0xBD, 0x11, 0x22, 0x33},
      .channel = 11,
      .rssi = -57,
      .auth_mode = WG_AUTH_WPA2_WPA3,
      .proto_flags = WG_SEC_PROTO_WPA2 | WG_SEC_PROTO_WPA3,
      .akm_flags = WG_SEC_AKM_PSK | WG_SEC_AKM_SAE,
      .cipher_flags = WG_SEC_CIPHER_CCMP_128,
      .ssid_len = 5,
      .ssid = {'h', 'e', 'l', 'l', 'o'},
      .flags = 0x01,
      .session_id = 0xAABBCCDDEEFF0011ULL,
      .record_seq = 9123,
      .node_id = 1,
      .source_flags = WG_SIGHTING_SOURCE_LIVE,
      .gps_valid = 1,
      .gps_source = 1,
      .gps_lat_e7 = 391333890,
      .gps_lon_e7 = 209626726,
      .gps_alt_mm = 22300,
      .gps_unix_time_s = 1711565786,
      .gps_accuracy_cm = 1200,
  };

  uint8_t out[96] = {0};
  size_t written = wg_build_sighting_payload(&sighting, out, sizeof(out));
  assert_true(written == 53, "sighting payload should encode with short SSID + metadata + GPS");
  assert_true(memcmp(out, sighting.bssid, 6) == 0, "bssid should be first field");
  assert_u8(out[6], 11, "channel should be encoded");
  assert_true((int8_t)out[7] == -57, "RSSI should be signed byte");
  assert_u8(out[9], WG_SEC_PROTO_WPA2 | WG_SEC_PROTO_WPA3, "proto flags should be encoded");
  assert_u8(out[10], WG_SEC_AKM_PSK | WG_SEC_AKM_SAE, "akm flags should be encoded");
  assert_u8(out[11], WG_SEC_CIPHER_CCMP_128, "cipher flags should be encoded");
  assert_u8(out[12], 5, "SSID len should be encoded");
  assert_true(memcmp(&out[13], "hello", 5) == 0, "SSID bytes should be encoded");
  assert_u8(out[18], 0x01, "flags should be encoded after SSID");
  assert_u64(rd_u64(&out[19]), 0xAABBCCDDEEFF0011ULL, "session id should encode");
  assert_u32(rd_u32(&out[27]), 9123, "record seq should encode");
  assert_u8(out[31], 1, "node id should encode");
  assert_u8(out[32], WG_SIGHTING_SOURCE_LIVE, "source flags should encode");
  assert_u8(out[33], 1, "gps valid should encode");
  assert_u8(out[34], 1, "gps source should encode");
  assert_true((int32_t)rd_u32(&out[35]) == 391333890, "gps lat should encode");
  assert_true((int32_t)rd_u32(&out[39]) == 209626726, "gps lon should encode");
  assert_true((int32_t)rd_u32(&out[43]) == 22300, "gps alt should encode");
  assert_u32(rd_u32(&out[47]), 1711565786U, "gps unix should encode");
  assert_u16(rd_u16(&out[51]), 1200, "gps accuracy should encode");
}

static void test_gps_payload_encoding(void) {
  wg_gps_payload_t gps = {
      .valid = true,
      .source = 2,
      .lat_e7 = 379876543,
      .lon_e7 = -1221234567,
      .alt_mm = 14500,
      .speed_mmps = 2230,
      .bearing_mdeg = 90250,
      .unix_time_s = 1711199944,
      .accuracy_cm = 430,
      .sat_count = 11,
      .hdop_centi = 92,
      .pdop_centi = 146,
  };

  uint8_t out[WG_GPS_PAYLOAD_SIZE] = {0};
  size_t written = wg_build_gps_payload(&gps, out, sizeof(out));
  assert_true(written == WG_GPS_PAYLOAD_SIZE, "gps payload should be fixed size");
  assert_u8(out[0], 1, "gps valid should be encoded");
  assert_u8(out[1], 2, "gps source should be encoded");
  assert_true((int32_t)rd_u32(&out[2]) == gps.lat_e7, "gps lat should encode");
  assert_true((int32_t)rd_u32(&out[6]) == gps.lon_e7, "gps lon should encode");
  assert_true((int32_t)rd_u32(&out[10]) == gps.alt_mm, "gps alt should encode");
  assert_true(rd_u32(&out[14]) == gps.speed_mmps, "gps speed should encode");
  assert_true(rd_u32(&out[18]) == gps.bearing_mdeg, "gps bearing should encode");
  assert_true(rd_u32(&out[22]) == gps.unix_time_s, "gps unix should encode");
  assert_u16(rd_u16(&out[26]), gps.accuracy_cm, "gps accuracy should encode");
  assert_u8(out[28], gps.sat_count, "gps sat_count should encode");
  assert_u16(rd_u16(&out[29]), gps.hdop_centi, "gps hdop should encode");
  assert_u16(rd_u16(&out[31]), gps.pdop_centi, "gps pdop should encode");
}

int main(void) {
  test_status_payload_encoding();
  test_sighting_payload_encoding();
  test_gps_payload_encoding();
  printf("PASS: %d tests\n", tests_run);
  return 0;
}
