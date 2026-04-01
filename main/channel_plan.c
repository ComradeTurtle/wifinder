#include "channel_plan.h"

bool wg_channel_mask_is_valid(uint16_t channel_mask) {
  const uint16_t supported_bits = 0x1FFF;  // channels 1..13
  return channel_mask != 0 && (channel_mask & ~supported_bits) == 0;
}

bool wg_channel_next(uint16_t channel_mask, uint8_t current_channel, uint8_t *next_channel) {
  if (next_channel == 0 || !wg_channel_mask_is_valid(channel_mask)) {
    return false;
  }

  uint8_t start = current_channel;
  if (start < 1 || start > 13) {
    start = 13;
  }

  for (uint8_t offset = 1; offset <= 13; ++offset) {
    uint8_t channel = (uint8_t)(((start + offset - 1) % 13) + 1);
    const uint16_t bit = (uint16_t)(1U << (channel - 1));
    if ((channel_mask & bit) != 0) {
      *next_channel = channel;
      return true;
    }
  }

  return false;
}
