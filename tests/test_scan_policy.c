#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "scan_policy.h"
#include "wg_payload.h"

static int tests_run = 0;

static void assert_true(bool condition, const char *message) {
  tests_run++;
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static void test_auto_start_requires_invalid_to_valid_transition(void) {
  assert_true(wg_scan_policy_should_auto_start(WG_BOOT_AUTO, false, false, false, true),
              "auto start should fire on invalid->valid transition in auto mode");
  assert_true(!wg_scan_policy_should_auto_start(WG_BOOT_AUTO, false, false, true, true),
              "auto start should not fire when gps already valid");
  assert_true(!wg_scan_policy_should_auto_start(WG_BOOT_MANUAL, false, false, false, true),
              "auto start should not fire in manual mode");
}

static void test_manual_stop_pauses_auto_restart_until_explicit_start(void) {
  bool paused = false;
  wg_scan_policy_on_manual_stop(WG_BOOT_AUTO, &paused);
  assert_true(paused, "manual stop in auto mode should pause auto-start");
  assert_true(!wg_scan_policy_should_auto_start(WG_BOOT_AUTO, paused, false, false, true),
              "paused auto mode should not auto-start");

  wg_scan_policy_on_explicit_start(&paused);
  assert_true(!paused, "explicit start should clear pause");
  assert_true(wg_scan_policy_should_auto_start(WG_BOOT_AUTO, paused, false, false, true),
              "auto-start should work again after explicit start");
}

int main(void) {
  test_auto_start_requires_invalid_to_valid_transition();
  test_manual_stop_pauses_auto_restart_until_explicit_start();
  printf("PASS: %d tests\n", tests_run);
  return 0;
}

