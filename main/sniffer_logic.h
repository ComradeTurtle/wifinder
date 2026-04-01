#ifndef SNIFFER_LOGIC_H
#define SNIFFER_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint8_t wardrive_next_channel(uint8_t current, uint8_t min_channel,
                              uint8_t max_channel);
bool wardrive_is_interesting_mgmt(uint8_t frame_control_byte0);
bool wardrive_format_bssid(const uint8_t bssid[6], char *out, size_t out_size);
size_t wardrive_mgmt_header_len(const uint8_t *payload, size_t payload_len);

#endif
