#include "wg_storage_backlog.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

bool wg_storage_parse_meta_filename(const char *name, const char *prefix,
                                    uint64_t *session_id_out) {
  if (name == NULL || prefix == NULL || session_id_out == NULL) {
    return false;
  }

  const size_t prefix_len = strlen(prefix);
  const size_t n = strlen(name);
  if (n != (prefix_len + 16U + 5U) || strncmp(name, prefix, prefix_len) != 0 ||
      strcmp(name + prefix_len + 16U, ".meta") != 0) {
    return false;
  }

  char hex[17] = {0};
  memcpy(hex, name + prefix_len, 16U);
  errno = 0;
  char *end = NULL;
  unsigned long long parsed = strtoull(hex, &end, 16);
  if (errno != 0 || end == NULL || *end != '\0') {
    return false;
  }
  *session_id_out = (uint64_t)parsed;
  return true;
}

uint32_t wg_storage_manifest_generation_get(uint16_t reserved0, const uint8_t reserved1[3],
                                            uint8_t generation_magic, bool *valid_out) {
  bool valid = (reserved1 != NULL && reserved1[2] == generation_magic);
  if (valid_out != NULL) {
    *valid_out = valid;
  }
  if (!valid) {
    return 0;
  }

  return (uint32_t)reserved0 | ((uint32_t)reserved1[0] << 16) | ((uint32_t)reserved1[1] << 24);
}

void wg_storage_manifest_generation_set(uint16_t *reserved0, uint8_t reserved1[3],
                                        uint8_t generation_magic, uint32_t generation) {
  if (reserved0 == NULL || reserved1 == NULL) {
    return;
  }
  *reserved0 = (uint16_t)(generation & 0xFFFFU);
  reserved1[0] = (uint8_t)((generation >> 16) & 0xFFU);
  reserved1[1] = (uint8_t)((generation >> 24) & 0xFFU);
  reserved1[2] = generation_magic;
}

int wg_storage_manifest_generation_cmp(uint32_t lhs, uint32_t rhs) {
  if (lhs == rhs) {
    return 0;
  }
  uint32_t delta = lhs - rhs;
  return (delta < 0x80000000U) ? 1 : -1;
}
