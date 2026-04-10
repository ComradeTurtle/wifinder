#ifndef SCAN_POLICY_H
#define SCAN_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "wg_payload.h"

bool wg_scan_policy_should_auto_start(uint8_t boot_mode, bool auto_paused, bool scanning,
                                      bool prev_gps_valid, bool curr_gps_valid);
void wg_scan_policy_on_manual_stop(uint8_t boot_mode, bool *auto_paused);
void wg_scan_policy_on_explicit_start(bool *auto_paused);
void wg_scan_policy_on_boot_mode_set(uint8_t boot_mode, bool *auto_paused);

#endif

