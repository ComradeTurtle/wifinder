#ifndef CHANNEL_PLAN_H
#define CHANNEL_PLAN_H

#include <stdbool.h>
#include <stdint.h>

bool wg_channel_mask_is_valid(uint16_t channel_mask);
bool wg_channel_next(uint16_t channel_mask, uint8_t current_channel, uint8_t *next_channel);

#endif
