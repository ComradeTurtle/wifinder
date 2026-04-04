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

#ifndef BLE_HS_EAGAIN
#define BLE_HS_EAGAIN 0x1003
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

#ifndef BLE_HS_ETIMEOUT_HCI
#define BLE_HS_ETIMEOUT_HCI 0x1007
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

#ifndef BLE_HS_ERR_SM_US_BASE
#define BLE_HS_ERR_SM_US_BASE 0x400
#endif

#ifndef BLE_HS_ERR_SM_PEER_BASE
#define BLE_HS_ERR_SM_PEER_BASE 0x500
#endif

#ifndef BLE_HS_ERR_HW_BASE
#define BLE_HS_ERR_HW_BASE 0x600
#endif

bool wg_ble_security_initiate_is_ok(int rc) {
  return rc == 0 || rc == BLE_HS_EALREADY;
}

bool wg_ble_security_initiate_is_busy(int rc) {
  return rc == BLE_HS_EBUSY;
}

bool wg_ble_should_request_security_on_subscribe(bool cur_notify, bool encrypted) {
  (void)cur_notify;
  (void)encrypted;
  return false;
}

bool wg_ble_should_request_security_on_protected_write(bool encrypted) {
  return !encrypted;
}

bool wg_ble_enc_change_status_should_drop_bond(int status) {
  if (status == BLE_HS_EAUTHEN || status == BLE_HS_EENCRYPT ||
      status == BLE_HS_EENCRYPT_KEY_SZ) {
    return true;
  }
  if (status >= BLE_HS_ERR_SM_US_BASE && status < BLE_HS_ERR_HW_BASE) {
    return true;
  }
  return false;
}

bool wg_ble_enc_change_status_should_retry(int status) {
  if (status == BLE_HS_ENOTCONN || status == BLE_HS_ENOMEM || status == BLE_HS_ETIMEOUT ||
      status == BLE_HS_ETIMEOUT_HCI) {
    return false;
  }
  if (status == BLE_HS_EAGAIN || status == BLE_HS_EBUSY) {
    return true;
  }
  return wg_ble_enc_change_status_should_drop_bond(status);
}
