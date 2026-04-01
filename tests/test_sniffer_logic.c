#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sniffer_logic.h"

static int tests_run = 0;

static void assert_true(bool condition, const char *message) {
  tests_run++;
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static void test_channel_hop_wraps(void) {
  assert_true(wardrive_next_channel(1, 1, 13) == 2,
              "channel hop should advance by one");
  assert_true(wardrive_next_channel(13, 1, 13) == 1,
              "channel hop should wrap to minimum");
}

static void test_frame_filter(void) {
  assert_true(wardrive_is_interesting_mgmt(0x80),
              "beacon frames should be accepted");
  assert_true(wardrive_is_interesting_mgmt(0x50),
              "probe responses should be accepted");
  assert_true(!wardrive_is_interesting_mgmt(0x88),
              "data QoS frame with same subtype nibble should be rejected");
  assert_true(!wardrive_is_interesting_mgmt(0x00),
              "association request should be rejected");
}

static void test_bssid_formatting(void) {
  uint8_t bssid[6] = {0x7c, 0x9e, 0xbd, 0x11, 0x22, 0x33};
  char out[18] = {0};

  assert_true(wardrive_format_bssid(bssid, out, sizeof(out)),
              "formatting should succeed");
  assert_true(strcmp(out, "7C:9E:BD:11:22:33") == 0,
              "formatted BSSID must be uppercase colon-separated hex");
}

static void test_mgmt_header_length(void) {
  uint8_t regular[64] = {0};
  regular[0] = 0x80;  // beacon subtype
  regular[1] = 0x00;  // no HT-Control / no order bit
  assert_true(wardrive_mgmt_header_len(regular, sizeof(regular)) == 24,
              "regular management header should be 24 bytes");

  uint8_t with_htc[64] = {0};
  with_htc[0] = 0x80;  // beacon subtype
  with_htc[1] = 0x80;  // order bit set -> HT-Control present
  assert_true(wardrive_mgmt_header_len(with_htc, sizeof(with_htc)) == 28,
              "management header with order bit should include HT-Control");

  uint8_t short_frame[20] = {0};
  assert_true(wardrive_mgmt_header_len(short_frame, sizeof(short_frame)) == 0,
              "short management frame should be rejected");
}

int main(void) {
  test_channel_hop_wraps();
  test_frame_filter();
  test_bssid_formatting();
  test_mgmt_header_length();

  printf("PASS: %d tests\n", tests_run);
  return 0;
}
