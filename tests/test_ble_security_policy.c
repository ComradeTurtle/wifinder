#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "ble_security_policy.h"

#if defined(__has_include)
#if __has_include("host/ble_hs.h")
#include "host/ble_hs.h"
#endif
#endif

#ifndef BLE_HS_EALREADY
#define BLE_HS_EALREADY 0x1001
#endif

#ifndef BLE_HS_EBUSY
#define BLE_HS_EBUSY 0x1002
#endif

#ifndef BLE_HS_EAUTHEN
#define BLE_HS_EAUTHEN 0x1008
#endif

#ifndef BLE_HS_EENCRYPT
#define BLE_HS_EENCRYPT 0x1009
#endif

#ifndef BLE_HS_EENCRYPT_KEY_SZ
#define BLE_HS_EENCRYPT_KEY_SZ 0x100A
#endif

#ifndef BLE_HS_ENOTCONN
#define BLE_HS_ENOTCONN 0x1004
#endif

#ifndef BLE_HS_ENOMEM
#define BLE_HS_ENOMEM 0x1005
#endif

#ifndef BLE_HS_ETIMEOUT
#define BLE_HS_ETIMEOUT 0x1006
#endif

#ifndef BLE_HS_ERR_SM_PEER_BASE
#define BLE_HS_ERR_SM_PEER_BASE 0x500
#endif

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
  assert_true(wg_ble_security_initiate_is_ok(BLE_HS_EALREADY),
              "rc=EALREADY should be treated as in-progress");
  assert_true(!wg_ble_security_initiate_is_ok(BLE_HS_EBUSY),
              "rc=EBUSY should not be treated as requested");
  assert_true(!wg_ble_security_initiate_is_ok(99), "unknown rc should not be treated as ok");

  assert_true(wg_ble_security_initiate_is_busy(BLE_HS_EBUSY),
              "rc=EBUSY should be recognized as busy");
  assert_true(!wg_ble_security_initiate_is_busy(0), "rc=0 should not be busy");
}

static void test_security_request_triggers(void) {
  assert_true(!wg_ble_should_request_security_on_subscribe(true, false),
              "notify subscribe should not trigger security initiation");
  assert_true(!wg_ble_should_request_security_on_subscribe(false, false),
              "unsubscribe should not request security");
  assert_true(!wg_ble_should_request_security_on_subscribe(true, true),
              "encrypted link should not request security on subscribe");

  assert_true(wg_ble_should_request_security_on_protected_write(false),
              "protected write on unencrypted link should request security");
  assert_true(!wg_ble_should_request_security_on_protected_write(true),
              "protected write on encrypted link should not request security");
}

static void test_enc_change_policy(void) {
  assert_true(!wg_ble_enc_change_status_should_drop_bond(BLE_HS_ENOTCONN),
              "disconnect should not trigger bond deletion");
  assert_true(!wg_ble_enc_change_status_should_drop_bond(BLE_HS_ENOMEM),
              "memory pressure should not trigger bond deletion");
  assert_true(wg_ble_enc_change_status_should_drop_bond(BLE_HS_EAUTHEN),
              "authentication failures should trigger bond deletion");
  assert_true(wg_ble_enc_change_status_should_drop_bond(BLE_HS_EENCRYPT),
              "encryption failures should trigger bond deletion");
  assert_true(wg_ble_enc_change_status_should_drop_bond(BLE_HS_EENCRYPT_KEY_SZ),
              "key-size failures should trigger bond deletion");
  assert_true(wg_ble_enc_change_status_should_drop_bond(BLE_HS_ERR_SM_PEER_BASE + 4),
              "peer SM failures should trigger bond deletion");

  assert_true(!wg_ble_enc_change_status_should_retry(BLE_HS_ENOTCONN),
              "disconnect should not retry security");
  assert_true(!wg_ble_enc_change_status_should_retry(BLE_HS_ENOMEM),
              "ENOMEM should not retry security immediately");
  assert_true(!wg_ble_enc_change_status_should_retry(BLE_HS_ETIMEOUT),
              "timeout should not retry security immediately");
  assert_true(wg_ble_enc_change_status_should_retry(BLE_HS_EBUSY),
              "busy should retry security");
  assert_true(wg_ble_enc_change_status_should_retry(BLE_HS_EAUTHEN),
              "auth failure should retry after bond reset");
}

int main(void) {
  test_initiate_rc_classification();
  test_security_request_triggers();
  test_enc_change_policy();
  printf("PASS: %d tests\n", tests_run);
  return 0;
}
