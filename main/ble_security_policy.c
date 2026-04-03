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

bool wg_ble_security_initiate_is_ok(int rc) {
  return rc == 0 || rc == BLE_HS_EALREADY;
}

bool wg_ble_security_initiate_is_busy(int rc) {
  return rc == BLE_HS_EBUSY;
}

bool wg_ble_should_request_security_on_subscribe(bool cur_notify, bool encrypted) {
  return cur_notify && !encrypted;
}

bool wg_ble_should_request_security_on_protected_write(bool encrypted) {
  return !encrypted;
}
