#pragma once

#include <stdbool.h>
#include <stdint.h>

uint16_t wg_transport_rd_u16_le(const uint8_t *in);
void wg_transport_wr_u16_le(uint8_t *out, uint16_t value);
void wg_transport_wr_u32_le(uint8_t *out, uint32_t value);
void wg_transport_wr_u64_le(uint8_t *out, uint64_t value);

const char *wg_transport_ble_phy_name(uint8_t phy);

uint16_t wg_transport_ble_notify_payload_limit(uint16_t att_mtu, uint16_t frame_header_size,
                                               uint16_t payload_max, uint16_t data_len_octets,
                                               bool encrypted);
