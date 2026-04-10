#include "scan_policy.h"

bool wg_scan_policy_should_auto_start(uint8_t boot_mode, bool auto_paused, bool scanning,
                                      bool prev_gps_valid, bool curr_gps_valid) {
  return boot_mode == WG_BOOT_AUTO && !auto_paused && !scanning && !prev_gps_valid &&
         curr_gps_valid;
}

void wg_scan_policy_on_manual_stop(uint8_t boot_mode, bool *auto_paused) {
  if (auto_paused == NULL) {
    return;
  }
  if (boot_mode == WG_BOOT_AUTO) {
    *auto_paused = true;
  }
}

void wg_scan_policy_on_explicit_start(bool *auto_paused) {
  if (auto_paused == NULL) {
    return;
  }
  *auto_paused = false;
}

void wg_scan_policy_on_boot_mode_set(uint8_t boot_mode, bool *auto_paused) {
  if (auto_paused == NULL) {
    return;
  }
  if (boot_mode == WG_BOOT_AUTO || boot_mode == WG_BOOT_MANUAL) {
    *auto_paused = false;
  }
}

