#pragma once

#include <stdbool.h>
#include <stdint.h>

bool wg_gps_parse_nmea_latlon(const char *value, const char *hemi, int32_t *out_e7);
uint16_t wg_gps_parse_nmea_dop_centi(const char *token);
bool wg_gps_nmea_sentence_has_type(const char *line, const char *type3);
uint16_t wg_gps_nmea_sentence_talker_key(const char *line);
bool wg_gps_talker_key_is_gn(uint16_t key);
