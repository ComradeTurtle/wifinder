#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "channel_plan.h"

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

static void test_mask_validation(void) {
  assert_true(wg_channel_mask_is_valid(0x0001), "bit 0 (channel 1) should be valid");
  assert_true(wg_channel_mask_is_valid(0x1000), "bit 12 (channel 13) should be valid");
  assert_true(!wg_channel_mask_is_valid(0x0000), "zero mask should be invalid");
  assert_true(!wg_channel_mask_is_valid(0x2000), "bit 13 should be invalid");
}

static void test_next_channel_wrap(void) {
  uint8_t next = 0;
  assert_true(wg_channel_next(0x1001, 1, &next), "next channel should resolve");
  assert_u8(next, 13, "mask with ch1+13 should jump to 13 from current=1");

  assert_true(wg_channel_next(0x1001, 13, &next), "next channel should wrap");
  assert_u8(next, 1, "mask with ch1+13 should wrap back to 1");
}

int main(void) {
  test_mask_validation();
  test_next_channel_wrap();
  printf("PASS: %d tests\n", tests_run);
  return 0;
}
