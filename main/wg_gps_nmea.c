#include "wg_gps_nmea.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static uint16_t clamp_u16(uint32_t value) {
  if (value > UINT16_MAX) {
    return UINT16_MAX;
  }
  return (uint16_t)value;
}

static size_t bounded_strlen(const char *s, size_t max_len) {
  if (s == NULL) {
    return 0;
  }
  size_t n = 0;
  while (n < max_len && s[n] != '\0') {
    ++n;
  }
  return n;
}

bool wg_gps_parse_nmea_latlon(const char *value, const char *hemi, int32_t *out_e7) {
  if (value == NULL || hemi == NULL || out_e7 == NULL || value[0] == '\0' || hemi[0] == '\0') {
    return false;
  }
  char *end = NULL;
  double raw = strtod(value, &end);
  if (end == value || raw <= 0.0) {
    return false;
  }
  int degrees = (int)(raw / 100.0);
  double minutes = raw - (double)degrees * 100.0;
  double dec = (double)degrees + minutes / 60.0;
  if (hemi[0] == 'S' || hemi[0] == 'W') {
    dec = -dec;
  } else if (hemi[0] != 'N' && hemi[0] != 'E') {
    return false;
  }
  *out_e7 = (int32_t)llround(dec * 10000000.0);
  return true;
}

uint16_t wg_gps_parse_nmea_dop_centi(const char *token) {
  if (token == NULL || token[0] == '\0') {
    return 0;
  }
  char *end = NULL;
  double dop = strtod(token, &end);
  if (end == token || !isfinite(dop) || dop <= 0.0) {
    return 0;
  }
  if (dop > 999.99) {
    dop = 999.99;
  }
  return clamp_u16((uint32_t)llround(dop * 100.0));
}

bool wg_gps_nmea_sentence_has_type(const char *line, const char *type3) {
  if (line == NULL || type3 == NULL) {
    return false;
  }
  size_t len = bounded_strlen(line, 16);
  if (len < 6) {
    return false;
  }
  return line[0] == '$' && strncmp(&line[3], type3, 3) == 0;
}

uint16_t wg_gps_nmea_sentence_talker_key(const char *line) {
  if (line == NULL) {
    return 0;
  }
  size_t len = bounded_strlen(line, 8);
  if (len < 3 || line[0] != '$') {
    return 0;
  }
  return (((uint16_t)(uint8_t)line[1]) << 8) | (uint16_t)(uint8_t)line[2];
}

bool wg_gps_talker_key_is_gn(uint16_t key) {
  return key == ((((uint16_t)'G') << 8) | (uint16_t)'N');
}
