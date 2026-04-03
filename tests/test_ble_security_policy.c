#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "ble_security_policy.h"

enum {
  TEST_RC_EALREADY = 7,
  TEST_RC_EBUSY = 8,
};

static int tests_run = 0;

static void assert_true(bool condition, const char *message) {
  tests_run++;
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static void test_initiate_rc_classification(void) {
  assert_true(wg_ble_security_initiate_is_ok(0), "rc=0 should be treated as requested");
  assert_true(wg_ble_security_initiate_is_ok(TEST_RC_EALREADY),
              "rc=EALREADY should be treated as in-progress");
  assert_true(!wg_ble_security_initiate_is_ok(TEST_RC_EBUSY),
              "rc=EBUSY should not be treated as requested");
  assert_true(!wg_ble_security_initiate_is_ok(99), "unknown rc should not be treated as ok");

  assert_true(wg_ble_security_initiate_is_busy(TEST_RC_EBUSY),
              "rc=EBUSY should be recognized as busy");
  assert_true(!wg_ble_security_initiate_is_busy(0), "rc=0 should not be busy");
}

static void test_security_request_triggers(void) {
  assert_true(wg_ble_should_request_security_on_subscribe(true, false),
              "notify subscribe on unencrypted link should request security");
  assert_true(!wg_ble_should_request_security_on_subscribe(false, false),
              "unsubscribe should not request security");
  assert_true(!wg_ble_should_request_security_on_subscribe(true, true),
              "encrypted link should not re-request security on subscribe");

  assert_true(wg_ble_should_request_security_on_protected_write(false),
              "protected write on unencrypted link should request security");
  assert_true(!wg_ble_should_request_security_on_protected_write(true),
              "protected write on encrypted link should not request security");
}

int main(void) {
  test_initiate_rc_classification();
  test_security_request_triggers();
  printf("PASS: %d tests\n", tests_run);
  return 0;
}
