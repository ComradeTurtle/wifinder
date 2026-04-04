#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wg_protocol.h"

static int tests_run = 0;

static void assert_true(bool condition, const char *message) {
  tests_run++;
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
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

static void assert_i32(int32_t actual, int32_t expected, const char *message) {
  tests_run++;
  if (actual != expected) {
    fprintf(stderr, "FAIL: %s (actual=%d expected=%d)\n", message, actual, expected);
    exit(1);
  }
}

static void test_frame_roundtrip(void) {
  uint8_t encoded[64] = {0};
  uint8_t payload[4] = {0x12, 0x34, 0x56, 0x78};

  wg_frame_t frame = {
      .version = WG_PROTOCOL_VERSION,
      .type = WG_MSG_STATUS,
      .seq = 42,
      .len = (uint16_t)sizeof(payload),
      .device_ms = 1234,
      .payload = payload,
  };

  size_t encoded_len = 0;
  assert_true(wg_frame_encode(&frame, encoded, sizeof(encoded), &encoded_len) == WG_OK,
              "encoding should succeed");

  wg_frame_t decoded = {0};
  assert_true(wg_frame_decode(encoded, encoded_len, &decoded) == WG_OK,
              "decoding should succeed");
  assert_u16(decoded.seq, 42, "sequence should survive roundtrip");
  assert_u16(decoded.len, (uint16_t)sizeof(payload), "payload len should roundtrip");
  assert_u32(decoded.device_ms, 1234, "device_ms should roundtrip");
  assert_true(memcmp(decoded.payload, payload, sizeof(payload)) == 0,
              "payload bytes must roundtrip");
}

static void test_command_decode(void) {
  uint8_t payload[] = {
      WG_CMD_SET_HOP_MS,
      0xF4,
      0x01,
  };  // 500 ms

  wg_command_t cmd = {0};
  assert_true(wg_command_decode(payload, sizeof(payload), &cmd) == WG_OK,
              "command decode should succeed");
  assert_true(cmd.id == WG_CMD_SET_HOP_MS, "command id should match");
  assert_u16(cmd.hop_ms, 500, "hop milliseconds should decode little-endian");
}

static void test_channel_mask_command_decode(void) {
  uint8_t payload[] = {
      WG_CMD_SET_CHANNEL_MASK,
      0x01,
      0x10,
  };  // channels 1 and 13

  wg_command_t cmd = {0};
  assert_true(wg_command_decode(payload, sizeof(payload), &cmd) == WG_OK,
              "channel mask decode should succeed");
  assert_u16(cmd.channel_mask, 0x1001, "channel mask should decode");
}

static void test_gps_fix_command_decode(void) {
  uint8_t payload[] = {
      WG_CMD_SET_GPS_FIX,
      WG_GPS_FLAG_VALID | WG_GPS_FLAG_HAS_ALT | WG_GPS_FLAG_HAS_SPEED | WG_GPS_FLAG_HAS_BEARING,
      0x80, 0x96, 0x98, 0x00,  // lat_e7 = 10000000
      0x7f, 0x69, 0x67, 0xff,  // lon_e7 = -10000001
      0x10, 0x27, 0x00, 0x00,  // alt_mm = 10000
      0xe8, 0x03, 0x00, 0x00,  // speed_mmps = 1000
      0x20, 0xa1, 0x07, 0x00,  // bearing_mdeg = 500000
      0x2d, 0x5b, 0xf4, 0x64,  // unix_time_s = 1693735725
      0x94, 0x11,              // accuracy_cm = 4500
  };

  wg_command_t cmd = {0};
  assert_true(wg_command_decode(payload, sizeof(payload), &cmd) == WG_OK,
              "gps fix decode should succeed");
  assert_true(cmd.id == WG_CMD_SET_GPS_FIX, "gps fix command id should match");
  assert_true(cmd.gps_flags == (WG_GPS_FLAG_VALID | WG_GPS_FLAG_HAS_ALT | WG_GPS_FLAG_HAS_SPEED |
                                WG_GPS_FLAG_HAS_BEARING),
              "gps flags should decode");
  assert_i32(cmd.gps_lat_e7, 10000000, "gps latitude should decode");
  assert_i32(cmd.gps_lon_e7, -10000001, "gps longitude should decode");
  assert_i32(cmd.gps_alt_mm, 10000, "gps altitude should decode");
  assert_u32(cmd.gps_speed_mmps, 1000, "gps speed should decode");
  assert_u32(cmd.gps_bearing_mdeg, 500000, "gps bearing should decode");
  assert_u32(cmd.gps_unix_time_s, 1693735725, "gps unix time should decode");
  assert_u16(cmd.gps_accuracy_cm, 4500, "gps accuracy should decode");
}

static void test_gps_fix_command_bad_length(void) {
  uint8_t payload[] = {
      WG_CMD_SET_GPS_FIX,
      0x01,
      0x00,
  };

  wg_command_t cmd = {0};
  assert_true(wg_command_decode(payload, sizeof(payload), &cmd) == WG_ERR_INVALID_FRAME,
              "gps fix command with wrong length should fail");
}

static void test_replay_ack_command_decode(void) {
  uint8_t payload[] = {
      WG_CMD_REPLAY_ACK,
      0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,  // session id (little-endian)
      0x2A, 0x00, 0x00, 0x00,                          // highest seq = 42
  };

  wg_command_t cmd = {0};
  assert_true(wg_command_decode(payload, sizeof(payload), &cmd) == WG_OK,
              "replay ack command decode should succeed");
  assert_true(cmd.id == WG_CMD_REPLAY_ACK, "replay ack command id should match");
  assert_true(cmd.replay_session_id == 0x1122334455667788ULL, "replay session id should decode");
  assert_u32(cmd.replay_highest_seq, 42, "replay highest seq should decode");
}

static void test_set_replay_command_decode(void) {
  uint8_t payload[] = {
      WG_CMD_SET_REPLAY,
      0x01,
  };

  wg_command_t cmd = {0};
  assert_true(wg_command_decode(payload, sizeof(payload), &cmd) == WG_OK,
              "set replay command decode should succeed");
  assert_true(cmd.id == WG_CMD_SET_REPLAY, "set replay command id should match");
  assert_true(cmd.replay_enable == 1, "set replay enable should decode");
}

static void test_set_replay_command_bad_length(void) {
  uint8_t payload[] = {
      WG_CMD_SET_REPLAY,
  };

  wg_command_t cmd = {0};
  assert_true(wg_command_decode(payload, sizeof(payload), &cmd) == WG_ERR_INVALID_FRAME,
              "set replay command with wrong length should fail");
}

static void test_clear_storage_command_decode(void) {
  uint8_t payload[] = {
      WG_CMD_CLEAR_STORAGE,
  };

  wg_command_t cmd = {0};
  assert_true(wg_command_decode(payload, sizeof(payload), &cmd) == WG_OK,
              "clear storage command decode should succeed");
  assert_true(cmd.id == WG_CMD_CLEAR_STORAGE, "clear storage command id should match");
}

static void test_set_backlog_blob_command_decode(void) {
  uint8_t payload[] = {
      WG_CMD_SET_BACKLOG_BLOB,
      0x01,
  };

  wg_command_t cmd = {0};
  assert_true(wg_command_decode(payload, sizeof(payload), &cmd) == WG_OK,
              "set backlog blob command decode should succeed");
  assert_true(cmd.id == WG_CMD_SET_BACKLOG_BLOB, "set backlog blob command id should match");
  assert_true(cmd.backlog_blob_enable == 1, "set backlog blob enable should decode");
}

static void test_debug_seed_storage_command_decode(void) {
  uint8_t payload[] = {
      WG_CMD_DEBUG_SEED_STORAGE,
      0x00, 0x00, 0x0C, 0x00,  // 786432 bytes
  };

  wg_command_t cmd = {0};
  assert_true(wg_command_decode(payload, sizeof(payload), &cmd) == WG_OK,
              "debug seed storage command decode should succeed");
  assert_true(cmd.id == WG_CMD_DEBUG_SEED_STORAGE, "debug seed storage command id should match");
  assert_u32(cmd.debug_seed_target_bytes, 786432U, "debug seed storage target bytes should decode");
}

int main(void) {
  test_frame_roundtrip();
  test_command_decode();
  test_channel_mask_command_decode();
  test_gps_fix_command_decode();
  test_gps_fix_command_bad_length();
  test_replay_ack_command_decode();
  test_set_replay_command_decode();
  test_set_replay_command_bad_length();
  test_clear_storage_command_decode();
  test_set_backlog_blob_command_decode();
  test_debug_seed_storage_command_decode();
  printf("PASS: %d tests\n", tests_run);
  return 0;
}
