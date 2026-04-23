#pragma once

#include <stdbool.h>
#include <stdint.h>

bool wg_storage_parse_meta_filename(const char *name, const char *prefix,
                                    uint64_t *session_id_out);

uint32_t wg_storage_manifest_generation_get(uint16_t reserved0, const uint8_t reserved1[3],
                                            uint8_t generation_magic, bool *valid_out);

void wg_storage_manifest_generation_set(uint16_t *reserved0, uint8_t reserved1[3],
                                        uint8_t generation_magic, uint32_t generation);

int wg_storage_manifest_generation_cmp(uint32_t lhs, uint32_t rhs);
