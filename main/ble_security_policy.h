#pragma once

#include <stdbool.h>

bool wg_ble_security_initiate_is_ok(int rc);
bool wg_ble_security_initiate_is_busy(int rc);
bool wg_ble_should_request_security_on_subscribe(bool cur_notify, bool encrypted);
bool wg_ble_should_request_security_on_protected_write(bool encrypted);
bool wg_ble_enc_change_status_should_drop_bond(int status);
bool wg_ble_enc_change_status_should_retry(int status);
