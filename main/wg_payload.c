#include "wg_payload.h"

#include <string.h>

static void wr_u16(uint8_t *out, uint16_t value) {
  out[0] = (uint8_t)(value & 0xFF);
  out[1] = (uint8_t)((value >> 8) & 0xFF);
}

static void wr_u32(uint8_t *out, uint32_t value) {
  out[0] = (uint8_t)(value & 0xFF);
  out[1] = (uint8_t)((value >> 8) & 0xFF);
  out[2] = (uint8_t)((value >> 16) & 0xFF);
  out[3] = (uint8_t)((value >> 24) & 0xFF);
}

static void wr_u64(uint8_t *out, uint64_t value) {
  wr_u32(out, (uint32_t)(value & 0xFFFFFFFFULL));
  wr_u32(out + 4, (uint32_t)((value >> 32) & 0xFFFFFFFFULL));
}

size_t wg_build_status_payload(const wg_status_payload_t *status, uint8_t *out, size_t out_size) {
  if (status == 0 || out == 0 || out_size < WG_STATUS_PAYLOAD_SIZE) {
    return 0;
  }

  out[0] = status->scanning ? 1 : 0;
  out[1] = status->ble_encrypted ? 1 : 0;
  out[2] = status->current_channel;
  wr_u16(&out[3], status->hop_ms);
  wr_u16(&out[5], status->channel_mask);
  const uint16_t unique_legacy =
      status->unique_bssids_estimate > UINT16_MAX ? UINT16_MAX
                                                  : (uint16_t)status->unique_bssids_estimate;
  wr_u16(&out[7], unique_legacy);
  wr_u16(&out[9], status->packets_per_sec);
  wr_u16(&out[11], status->dropped_notifies);
  out[13] = status->boot_mode;
  out[14] = status->gps_valid ? 1 : 0;
  wr_u16(&out[15], status->gps_age_s);
  wr_u16(&out[17], status->gps_accuracy_dm);
  out[19] = status->node_link_up ? 1 : 0;
  wr_u16(&out[20], status->node_last_seen_s);
  wr_u16(&out[22], status->node_packets_per_sec);
  wr_u16(&out[24], status->node_forwarded_sightings);
  out[26] = status->node_channel;
  wr_u16(&out[27], status->node_channel_mask);
  out[29] = status->session_open ? 1 : 0;
  wr_u64(&out[30], status->session_id);
  wr_u32(&out[38], status->queued_records);
  wr_u32(&out[42], status->queued_bytes);
  out[46] = status->replay_active ? 1 : 0;
  wr_u32(&out[47], status->replay_cursor);
  out[51] = status->queue_full ? 1 : 0;
  wr_u32(&out[52], status->dropped_flash_full);
  out[56] = status->node_count;
  wr_u32(&out[57], status->unique_bssids_estimate);
  out[61] = status->gps_nav_applied_hz;
  wr_u32(&out[62], status->spiffs_total_bytes);
  wr_u32(&out[66], status->spiffs_used_bytes);
  wr_u32(&out[70], status->spiffs_free_bytes);
  wr_u64(&out[91], status->storage_total_bytes);
  wr_u64(&out[99], status->storage_used_bytes);
  wr_u64(&out[107], status->storage_free_bytes);
  wr_u16(&out[115], status->local_channel_mask);
  wr_u16(&out[117], status->node_channel_mask_24);
  wr_u64(&out[119], status->node_channel_mask_5ghz);
  wr_u16(&out[127], (uint16_t)status->die_temp_centi);
  out[74] = status->blob_active ? 1 : 0;
  wr_u64(&out[75], status->blob_session_id);
  wr_u32(&out[83], status->blob_bytes_sent);
  wr_u32(&out[87], status->blob_bytes_total);

  return WG_STATUS_PAYLOAD_SIZE;
}

size_t wg_build_sighting_payload(const wg_sighting_payload_t *sighting, uint8_t *out,
                                 size_t out_size) {
  if (sighting == 0 || out == 0) {
    return 0;
  }

  uint8_t ssid_len = sighting->ssid_len;
  if (ssid_len > 32) {
    ssid_len = 32;
  }

  const size_t total = 48 + ssid_len;
  if (out_size < total) {
    return 0;
  }

  memcpy(out, sighting->bssid, 6);
  out[6] = sighting->channel;
  out[7] = (uint8_t)sighting->rssi;
  out[8] = sighting->auth_mode;
  out[9] = sighting->proto_flags;
  out[10] = sighting->akm_flags;
  out[11] = sighting->cipher_flags;
  out[12] = ssid_len;
  if (ssid_len > 0) {
    memcpy(&out[13], sighting->ssid, ssid_len);
  }
  const size_t flags_idx = (size_t)13 + ssid_len;
  out[flags_idx] = sighting->flags;
  wr_u64(&out[flags_idx + 1], sighting->session_id);
  wr_u32(&out[flags_idx + 9], sighting->record_seq);
  out[flags_idx + 13] = sighting->node_id;
  out[flags_idx + 14] = sighting->source_flags;
  out[flags_idx + 15] = sighting->gps_valid ? 1 : 0;
  out[flags_idx + 16] = sighting->gps_source;
  wr_u32(&out[flags_idx + 17], (uint32_t)sighting->gps_lat_e7);
  wr_u32(&out[flags_idx + 21], (uint32_t)sighting->gps_lon_e7);
  wr_u32(&out[flags_idx + 25], (uint32_t)sighting->gps_alt_mm);
  wr_u32(&out[flags_idx + 29], sighting->gps_unix_time_s);
  wr_u16(&out[flags_idx + 33], sighting->gps_accuracy_cm);

  return total;
}

size_t wg_build_gps_payload(const wg_gps_payload_t *gps, uint8_t *out, size_t out_size) {
  if (gps == 0 || out == 0 || out_size < WG_GPS_PAYLOAD_SIZE) {
    return 0;
  }
  out[0] = gps->valid ? 1 : 0;
  out[1] = gps->source;
  wr_u32(&out[2], (uint32_t)gps->lat_e7);
  wr_u32(&out[6], (uint32_t)gps->lon_e7);
  wr_u32(&out[10], (uint32_t)gps->alt_mm);
  wr_u32(&out[14], gps->speed_mmps);
  wr_u32(&out[18], gps->bearing_mdeg);
  wr_u32(&out[22], gps->unix_time_s);
  wr_u16(&out[26], gps->accuracy_cm);
  out[28] = gps->sat_count;
  wr_u16(&out[29], gps->hdop_centi);
  wr_u16(&out[31], gps->pdop_centi);
  return WG_GPS_PAYLOAD_SIZE;
}
