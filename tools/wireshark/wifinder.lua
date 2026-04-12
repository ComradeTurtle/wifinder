-- WIFINDER BLE ATT post-dissector for Wireshark.
--
-- Decodes the custom WG frame format carried in btatt.value:
--   magic[2] = 0x57 0x47 ("WG")
--   version[1]
--   type[1]
--   seq[2] (LE)
--   len[2] (LE)
--   device_ms[4] (LE)
--   payload[len]

if rawget(_G, "__wifinder_dissector_loaded") then
  return
end
_G.__wifinder_dissector_loaded = true

local wifinder = Proto("wifinder", "WIFINDER")

local MSG_NAMES = {
  [0x01] = "STATUS",
  [0x02] = "SIGHTING",
  [0x03] = "ACK",
  [0x04] = "ERROR",
  [0x05] = "SNAPSHOT_END",
  [0x06] = "CONFIG",
  [0x07] = "GPS",
  [0x08] = "REPLAY_ACK",
  [0x09] = "NODE_TABLE",
  [0x81] = "COMMAND",
}

local CMD_NAMES = {
  [0x01] = "START",
  [0x02] = "STOP",
  [0x03] = "SET_HOP_MS",
  [0x04] = "SET_CHANNEL_MASK",
  [0x05] = "SET_BOOT_MODE",
  [0x06] = "REQUEST_STATUS",
  [0x07] = "REQUEST_SNAPSHOT",
  [0x08] = "SET_GPS_FIX",
  [0x09] = "REPLAY_ACK",
  [0x0A] = "SET_REPLAY",
  [0x0B] = "CLEAR_STORAGE",
}

local AUTH_NAMES = {
  [0] = "UNKNOWN",
  [1] = "OPEN",
  [2] = "WEP",
  [3] = "WPA",
  [4] = "WPA2/WPA3",
}

local f_att_value
local f_att_opcode
local f_att_handle

local function safe_field(name)
  local ok, field = pcall(Field.new, name)
  if ok then
    return field
  end
  return nil
end

local function first_field(extractor)
  if extractor == nil then
    return nil
  end
  local ok, values = pcall(function()
    return { extractor() }
  end)
  if not ok or #values == 0 then
    return nil
  end
  return values[1]
end

local function fi_to_uint(fi)
  if fi == nil then
    return nil
  end
  local direct = tonumber(fi)
  if direct ~= nil then
    return direct
  end
  local ok, value = pcall(function()
    return fi.value
  end)
  if not ok then
    return nil
  end
  return tonumber(value)
end

local function le_u16(bytes, offset)
  return bytes:get_index(offset) + bytes:get_index(offset + 1) * 256
end

local function le_u32(bytes, offset)
  return bytes:get_index(offset) +
      bytes:get_index(offset + 1) * 256 +
      bytes:get_index(offset + 2) * 65536 +
      bytes:get_index(offset + 3) * 16777216
end

local function le_i32(bytes, offset)
  local u = le_u32(bytes, offset)
  if u >= 2147483648 then
    return u - 4294967296
  end
  return u
end

local function le_u64_hex(bytes, offset)
  local lo = le_u32(bytes, offset)
  local hi = le_u32(bytes, offset + 4)
  return string.format("0x%08X%08X", hi, lo)
end

local function to_signed_i8(v)
  if v > 127 then
    return v - 256
  end
  return v
end

local function bytes_to_bssid(bytes, offset)
  local parts = {}
  for i = 0, 5 do
    parts[#parts + 1] = string.format("%02X", bytes:get_index(offset + i))
  end
  return table.concat(parts, ":")
end

local function bytes_to_text(bytes, offset, size)
  local out = {}
  for i = 0, size - 1 do
    local b = bytes:get_index(offset + i)
    if b >= 32 and b <= 126 then
      out[#out + 1] = string.char(b)
    else
      out[#out + 1] = "."
    end
  end
  return table.concat(out, "")
end

local pf_att_opcode = ProtoField.uint8("wifinder.att_opcode", "ATT Opcode", base.HEX)
local pf_att_handle = ProtoField.uint16("wifinder.att_handle", "ATT Handle", base.HEX)

local pf_magic = ProtoField.string("wifinder.magic", "Magic")
local pf_version = ProtoField.uint8("wifinder.version", "Version", base.DEC)
local pf_type = ProtoField.uint8("wifinder.type", "Type", base.HEX, MSG_NAMES)
local pf_seq = ProtoField.uint16("wifinder.seq", "Sequence", base.DEC)
local pf_len = ProtoField.uint16("wifinder.len", "Payload Length", base.DEC)
local pf_device_ms = ProtoField.uint32("wifinder.device_ms", "Device Milliseconds", base.DEC)
local pf_payload_note = ProtoField.string("wifinder.payload_note", "Payload")
local pf_parse_error = ProtoField.string("wifinder.parse_error", "Parse Error")

local pf_status_scanning = ProtoField.uint8("wifinder.status.scanning", "Status Scanning", base.DEC,
  { [0] = "false", [1] = "true" })
local pf_status_ble_encrypted = ProtoField.uint8("wifinder.status.ble_encrypted", "Status BLE Encrypted", base.DEC,
  { [0] = "false", [1] = "true" })
local pf_status_channel = ProtoField.uint8("wifinder.status.channel", "Status Current Channel", base.DEC)
local pf_status_hop_ms = ProtoField.uint16("wifinder.status.hop_ms", "Status Hop Interval (ms)", base.DEC)
local pf_status_channel_mask = ProtoField.uint16("wifinder.status.channel_mask", "Status Channel Mask", base.HEX)
local pf_status_unique = ProtoField.uint16("wifinder.status.unique_bssids", "Status Unique BSSIDs", base.DEC)
local pf_status_pps = ProtoField.uint16("wifinder.status.packets_per_sec", "Status Packets/s", base.DEC)
local pf_status_drops = ProtoField.uint16("wifinder.status.dropped_notifies", "Status Dropped Notifies", base.DEC)
local pf_status_boot = ProtoField.uint8("wifinder.status.boot_mode", "Status Boot Mode", base.DEC,
  { [0] = "manual", [1] = "auto" })
local pf_status_gps_valid = ProtoField.uint8("wifinder.status.gps_valid", "Status GPS Valid", base.DEC,
  { [0] = "false", [1] = "true" })
local pf_status_gps_age_s = ProtoField.uint16("wifinder.status.gps_age_s", "Status GPS Age (s)", base.DEC)
local pf_status_gps_acc_dm = ProtoField.uint16("wifinder.status.gps_accuracy_dm", "Status GPS Accuracy (dm)",
  base.DEC)
local pf_status_node_link_up = ProtoField.uint8("wifinder.status.node_link_up", "Status Node Link Up", base.DEC,
  { [0] = "false", [1] = "true" })
local pf_status_node_last_seen_s = ProtoField.uint16("wifinder.status.node_last_seen_s", "Status Node Last Seen (s)",
  base.DEC)
local pf_status_node_pps = ProtoField.uint16("wifinder.status.node_packets_per_sec", "Status Node Packets/s", base.DEC)
local pf_status_node_forwarded = ProtoField.uint16("wifinder.status.node_forwarded_sightings",
  "Status Node Forwarded Sightings", base.DEC)
local pf_status_node_channel = ProtoField.uint8("wifinder.status.node_channel", "Status Node Channel", base.DEC)
local pf_status_node_channel_mask = ProtoField.uint16("wifinder.status.node_channel_mask", "Status Node Channel Mask",
  base.HEX)
local pf_status_session_open = ProtoField.uint8("wifinder.status.session_open", "Status Session Open", base.DEC,
  { [0] = "false", [1] = "true" })
local pf_status_session_id = ProtoField.string("wifinder.status.session_id", "Status Session ID")
local pf_status_queued_records = ProtoField.uint32("wifinder.status.queued_records", "Status Queued Records", base.DEC)
local pf_status_queued_bytes = ProtoField.uint32("wifinder.status.queued_bytes", "Status Queued Bytes", base.DEC)
local pf_status_replay_active = ProtoField.uint8("wifinder.status.replay_active", "Status Replay Active", base.DEC,
  { [0] = "false", [1] = "true" })
local pf_status_replay_cursor = ProtoField.uint32("wifinder.status.replay_cursor", "Status Replay Cursor Seq", base.DEC)
local pf_status_queue_full = ProtoField.uint8("wifinder.status.queue_full", "Status Queue Full", base.DEC,
  { [0] = "false", [1] = "true" })
local pf_status_dropped_flash_full = ProtoField.uint32("wifinder.status.dropped_flash_full",
  "Status Dropped (Flash Full)", base.DEC)
local pf_status_node_count = ProtoField.uint8("wifinder.status.node_count", "Status Node Count", base.DEC)

local pf_gps_valid = ProtoField.uint8("wifinder.gps.valid", "GPS Valid", base.DEC,
  { [0] = "false", [1] = "true" })
local pf_gps_source = ProtoField.uint8("wifinder.gps.source", "GPS Source", base.DEC,
  { [0] = "none", [1] = "uart", [2] = "phone" })
local pf_gps_lat = ProtoField.int32("wifinder.gps.lat_e7", "GPS Latitude (e7)", base.DEC)
local pf_gps_lon = ProtoField.int32("wifinder.gps.lon_e7", "GPS Longitude (e7)", base.DEC)
local pf_gps_alt = ProtoField.int32("wifinder.gps.alt_mm", "GPS Altitude (mm)", base.DEC)
local pf_gps_speed = ProtoField.uint32("wifinder.gps.speed_mmps", "GPS Speed (mm/s)", base.DEC)
local pf_gps_bearing = ProtoField.uint32("wifinder.gps.bearing_mdeg", "GPS Bearing (mdeg)", base.DEC)
local pf_gps_time = ProtoField.uint32("wifinder.gps.unix_time_s", "GPS Unix Time (s)", base.DEC)
local pf_gps_accuracy = ProtoField.uint16("wifinder.gps.accuracy_cm", "GPS Accuracy (cm)", base.DEC)
local pf_gps_sat_count = ProtoField.uint8("wifinder.gps.sat_count", "GPS Satellites", base.DEC)
local pf_gps_hdop_centi = ProtoField.uint16("wifinder.gps.hdop_centi", "GPS HDOP (x100)", base.DEC)
local pf_gps_pdop_centi = ProtoField.uint16("wifinder.gps.pdop_centi", "GPS PDOP (x100)", base.DEC)

local pf_sighting_bssid = ProtoField.string("wifinder.sighting.bssid", "Sighting BSSID")
local pf_sighting_channel = ProtoField.uint8("wifinder.sighting.channel", "Sighting Channel", base.DEC)
local pf_sighting_rssi = ProtoField.int8("wifinder.sighting.rssi", "Sighting RSSI", base.DEC)
local pf_sighting_auth = ProtoField.uint8("wifinder.sighting.auth", "Sighting Auth", base.DEC, AUTH_NAMES)
local pf_sighting_proto = ProtoField.uint8("wifinder.sighting.proto_flags", "Sighting Protocol Flags", base.HEX)
local pf_sighting_akm = ProtoField.uint8("wifinder.sighting.akm_flags", "Sighting AKM Flags", base.HEX)
local pf_sighting_cipher = ProtoField.uint8("wifinder.sighting.cipher_flags", "Sighting Cipher Flags", base.HEX)
local pf_sighting_ssid_len = ProtoField.uint8("wifinder.sighting.ssid_len", "Sighting SSID Length", base.DEC)
local pf_sighting_ssid = ProtoField.string("wifinder.sighting.ssid", "Sighting SSID")
local pf_sighting_flags = ProtoField.uint8("wifinder.sighting.flags", "Sighting Flags", base.HEX)
local pf_sighting_session_id = ProtoField.string("wifinder.sighting.session_id", "Sighting Session ID")
local pf_sighting_record_seq = ProtoField.uint32("wifinder.sighting.record_seq", "Sighting Record Seq", base.DEC)
local pf_sighting_node_id = ProtoField.uint8("wifinder.sighting.node_id", "Sighting Node ID", base.DEC)
local pf_sighting_source_flags = ProtoField.uint8("wifinder.sighting.source_flags", "Sighting Source Flags", base.HEX)
local pf_sighting_gps_valid = ProtoField.uint8("wifinder.sighting.gps_valid", "Sighting GPS Valid", base.DEC,
  { [0] = "false", [1] = "true" })
local pf_sighting_gps_source = ProtoField.uint8("wifinder.sighting.gps_source", "Sighting GPS Source", base.DEC,
  { [0] = "none", [1] = "uart", [2] = "phone" })
local pf_sighting_gps_lat = ProtoField.int32("wifinder.sighting.gps_lat_e7", "Sighting GPS Latitude (e7)", base.DEC)
local pf_sighting_gps_lon = ProtoField.int32("wifinder.sighting.gps_lon_e7", "Sighting GPS Longitude (e7)", base.DEC)
local pf_sighting_gps_alt = ProtoField.int32("wifinder.sighting.gps_alt_mm", "Sighting GPS Altitude (mm)", base.DEC)
local pf_sighting_gps_time = ProtoField.uint32("wifinder.sighting.gps_unix_time_s", "Sighting GPS Unix Time (s)",
  base.DEC)
local pf_sighting_gps_accuracy = ProtoField.uint16("wifinder.sighting.gps_accuracy_cm",
  "Sighting GPS Accuracy (cm)", base.DEC)

local pf_ack_cmd = ProtoField.uint8("wifinder.ack.command", "ACK Command", base.HEX, CMD_NAMES)
local pf_error_cmd = ProtoField.uint8("wifinder.error.command", "Error Command", base.HEX, CMD_NAMES)
local pf_error_code = ProtoField.uint8("wifinder.error.code", "Error Code", base.DEC)

local pf_cfg_hop = ProtoField.uint16("wifinder.config.hop_ms", "Config Hop Interval (ms)", base.DEC)
local pf_cfg_mask = ProtoField.uint16("wifinder.config.channel_mask", "Config Channel Mask", base.HEX)
local pf_cfg_boot = ProtoField.uint8("wifinder.config.boot_mode", "Config Boot Mode", base.DEC,
  { [0] = "manual", [1] = "auto" })

local pf_cmd_id = ProtoField.uint8("wifinder.command.id", "Command ID", base.HEX, CMD_NAMES)
local pf_cmd_hop = ProtoField.uint16("wifinder.command.hop_ms", "Command Hop Interval (ms)", base.DEC)
local pf_cmd_mask = ProtoField.uint16("wifinder.command.channel_mask", "Command Channel Mask", base.HEX)
local pf_cmd_boot = ProtoField.uint8("wifinder.command.boot_mode", "Command Boot Mode", base.DEC,
  { [0] = "manual", [1] = "auto" })
local pf_cmd_gps_flags = ProtoField.uint8("wifinder.command.gps_flags", "Command GPS Flags", base.HEX)
local pf_cmd_gps_lat = ProtoField.int32("wifinder.command.gps_lat_e7", "Command GPS Latitude (e7)", base.DEC)
local pf_cmd_gps_lon = ProtoField.int32("wifinder.command.gps_lon_e7", "Command GPS Longitude (e7)", base.DEC)
local pf_cmd_gps_alt = ProtoField.int32("wifinder.command.gps_alt_mm", "Command GPS Altitude (mm)", base.DEC)
local pf_cmd_gps_speed = ProtoField.uint32("wifinder.command.gps_speed_mmps", "Command GPS Speed (mm/s)", base.DEC)
local pf_cmd_gps_bearing = ProtoField.uint32("wifinder.command.gps_bearing_mdeg", "Command GPS Bearing (mdeg)",
  base.DEC)
local pf_cmd_gps_time = ProtoField.uint32("wifinder.command.gps_unix_time_s", "Command GPS Unix Time (s)", base.DEC)
local pf_cmd_gps_accuracy = ProtoField.uint16("wifinder.command.gps_accuracy_cm", "Command GPS Accuracy (cm)",
  base.DEC)
local pf_cmd_replay_sid = ProtoField.string("wifinder.command.replay_session_id", "Command Replay Session ID")
local pf_cmd_replay_seq = ProtoField.uint32("wifinder.command.replay_highest_seq", "Command Replay Highest Seq",
  base.DEC)
local pf_cmd_replay_enable = ProtoField.uint8("wifinder.command.replay_enable", "Command Replay Enable",
  base.DEC, { [0] = "disable", [1] = "enable" })

wifinder.fields = {
  pf_att_opcode,
  pf_att_handle,
  pf_magic,
  pf_version,
  pf_type,
  pf_seq,
  pf_len,
  pf_device_ms,
  pf_payload_note,
  pf_parse_error,
  pf_status_scanning,
  pf_status_ble_encrypted,
  pf_status_channel,
  pf_status_hop_ms,
  pf_status_channel_mask,
  pf_status_unique,
  pf_status_pps,
  pf_status_drops,
  pf_status_boot,
  pf_status_gps_valid,
  pf_status_gps_age_s,
  pf_status_gps_acc_dm,
  pf_status_node_link_up,
  pf_status_node_last_seen_s,
  pf_status_node_pps,
  pf_status_node_forwarded,
  pf_status_node_channel,
  pf_status_node_channel_mask,
  pf_status_session_open,
  pf_status_session_id,
  pf_status_queued_records,
  pf_status_queued_bytes,
  pf_status_replay_active,
  pf_status_replay_cursor,
  pf_status_queue_full,
  pf_status_dropped_flash_full,
  pf_status_node_count,
  pf_gps_valid,
  pf_gps_source,
  pf_gps_lat,
  pf_gps_lon,
  pf_gps_alt,
  pf_gps_speed,
  pf_gps_bearing,
  pf_gps_time,
  pf_gps_accuracy,
  pf_gps_sat_count,
  pf_gps_hdop_centi,
  pf_gps_pdop_centi,
  pf_sighting_bssid,
  pf_sighting_channel,
  pf_sighting_rssi,
  pf_sighting_auth,
  pf_sighting_proto,
  pf_sighting_akm,
  pf_sighting_cipher,
  pf_sighting_ssid_len,
  pf_sighting_ssid,
  pf_sighting_flags,
  pf_sighting_session_id,
  pf_sighting_record_seq,
  pf_sighting_node_id,
  pf_sighting_source_flags,
  pf_sighting_gps_valid,
  pf_sighting_gps_source,
  pf_sighting_gps_lat,
  pf_sighting_gps_lon,
  pf_sighting_gps_alt,
  pf_sighting_gps_time,
  pf_sighting_gps_accuracy,
  pf_ack_cmd,
  pf_error_cmd,
  pf_error_code,
  pf_cfg_hop,
  pf_cfg_mask,
  pf_cfg_boot,
  pf_cmd_id,
  pf_cmd_hop,
  pf_cmd_mask,
  pf_cmd_boot,
  pf_cmd_gps_flags,
  pf_cmd_gps_lat,
  pf_cmd_gps_lon,
  pf_cmd_gps_alt,
  pf_cmd_gps_speed,
  pf_cmd_gps_bearing,
  pf_cmd_gps_time,
  pf_cmd_gps_accuracy,
  pf_cmd_replay_sid,
  pf_cmd_replay_seq,
  pf_cmd_replay_enable,
}

local function decode_status_fields(subtree, bytes, payload_offset, payload_len)
  if payload_len < 14 then
    subtree:add(pf_parse_error, "STATUS payload too short (need 14 bytes)")
    return
  end
  subtree:add(pf_status_scanning, bytes:get_index(payload_offset + 0))
  subtree:add(pf_status_ble_encrypted, bytes:get_index(payload_offset + 1))
  subtree:add(pf_status_channel, bytes:get_index(payload_offset + 2))
  subtree:add(pf_status_hop_ms, le_u16(bytes, payload_offset + 3))
  subtree:add(pf_status_channel_mask, le_u16(bytes, payload_offset + 5))
  subtree:add(pf_status_unique, le_u16(bytes, payload_offset + 7))
  subtree:add(pf_status_pps, le_u16(bytes, payload_offset + 9))
  subtree:add(pf_status_drops, le_u16(bytes, payload_offset + 11))
  subtree:add(pf_status_boot, bytes:get_index(payload_offset + 13))
  if payload_len >= 19 then
    subtree:add(pf_status_gps_valid, bytes:get_index(payload_offset + 14))
    subtree:add(pf_status_gps_age_s, le_u16(bytes, payload_offset + 15))
    subtree:add(pf_status_gps_acc_dm, le_u16(bytes, payload_offset + 17))
  end
  if payload_len >= 29 then
    subtree:add(pf_status_node_link_up, bytes:get_index(payload_offset + 19))
    subtree:add(pf_status_node_last_seen_s, le_u16(bytes, payload_offset + 20))
    subtree:add(pf_status_node_pps, le_u16(bytes, payload_offset + 22))
    subtree:add(pf_status_node_forwarded, le_u16(bytes, payload_offset + 24))
    subtree:add(pf_status_node_channel, bytes:get_index(payload_offset + 26))
    subtree:add(pf_status_node_channel_mask, le_u16(bytes, payload_offset + 27))
  end
  if payload_len >= 57 then
    subtree:add(pf_status_session_open, bytes:get_index(payload_offset + 29))
    subtree:add(pf_status_session_id, le_u64_hex(bytes, payload_offset + 30))
    subtree:add(pf_status_queued_records, le_u32(bytes, payload_offset + 38))
    subtree:add(pf_status_queued_bytes, le_u32(bytes, payload_offset + 42))
    subtree:add(pf_status_replay_active, bytes:get_index(payload_offset + 46))
    subtree:add(pf_status_replay_cursor, le_u32(bytes, payload_offset + 47))
    subtree:add(pf_status_queue_full, bytes:get_index(payload_offset + 51))
    subtree:add(pf_status_dropped_flash_full, le_u32(bytes, payload_offset + 52))
    subtree:add(pf_status_node_count, bytes:get_index(payload_offset + 56))
  end
end

local function decode_gps_fields(subtree, bytes, payload_offset, payload_len)
  if payload_len < 28 then
    subtree:add(pf_parse_error, "GPS payload too short (need 28 bytes)")
    return
  end
  subtree:add(pf_gps_valid, bytes:get_index(payload_offset + 0))
  subtree:add(pf_gps_source, bytes:get_index(payload_offset + 1))
  subtree:add(pf_gps_lat, le_i32(bytes, payload_offset + 2))
  subtree:add(pf_gps_lon, le_i32(bytes, payload_offset + 6))
  subtree:add(pf_gps_alt, le_i32(bytes, payload_offset + 10))
  subtree:add(pf_gps_speed, le_u32(bytes, payload_offset + 14))
  subtree:add(pf_gps_bearing, le_u32(bytes, payload_offset + 18))
  subtree:add(pf_gps_time, le_u32(bytes, payload_offset + 22))
  subtree:add(pf_gps_accuracy, le_u16(bytes, payload_offset + 26))
  if payload_len >= 29 then
    subtree:add(pf_gps_sat_count, bytes:get_index(payload_offset + 28))
  end
  if payload_len >= 31 then
    subtree:add(pf_gps_hdop_centi, le_u16(bytes, payload_offset + 29))
  end
  if payload_len >= 33 then
    subtree:add(pf_gps_pdop_centi, le_u16(bytes, payload_offset + 31))
  end
end

local function decode_sighting_fields(subtree, bytes, payload_offset, payload_len)
  if payload_len < 14 then
    subtree:add(pf_parse_error, "SIGHTING payload too short (need >=14 bytes)")
    return
  end

  local raw_ssid_len = bytes:get_index(payload_offset + 12)
  local ssid_len = raw_ssid_len
  if ssid_len > 32 then
    ssid_len = 32
  end

  local needed = 14 + ssid_len
  if payload_len < needed then
    subtree:add(pf_parse_error, string.format(
      "SIGHTING payload truncated (need %d bytes, got %d)", needed, payload_len))
    return
  end

  local bssid = bytes_to_bssid(bytes, payload_offset + 0)
  subtree:add(pf_sighting_bssid, bssid)
  subtree:add(pf_sighting_channel, bytes:get_index(payload_offset + 6))
  subtree:add(pf_sighting_rssi, to_signed_i8(bytes:get_index(payload_offset + 7)))
  subtree:add(pf_sighting_auth, bytes:get_index(payload_offset + 8))
  subtree:add(pf_sighting_proto, bytes:get_index(payload_offset + 9))
  subtree:add(pf_sighting_akm, bytes:get_index(payload_offset + 10))
  subtree:add(pf_sighting_cipher, bytes:get_index(payload_offset + 11))
  subtree:add(pf_sighting_ssid_len, raw_ssid_len)
  subtree:add(pf_sighting_ssid, bytes_to_text(bytes, payload_offset + 13, ssid_len))
  subtree:add(pf_sighting_flags, bytes:get_index(payload_offset + 13 + ssid_len))
  if payload_len >= (needed + 14) then
    local trailer = payload_offset + needed
    subtree:add(pf_sighting_session_id, le_u64_hex(bytes, trailer + 0))
    subtree:add(pf_sighting_record_seq, le_u32(bytes, trailer + 8))
    subtree:add(pf_sighting_node_id, bytes:get_index(trailer + 12))
    subtree:add(pf_sighting_source_flags, bytes:get_index(trailer + 13))
    if payload_len >= (needed + 34) then
      local gps = trailer + 14
      subtree:add(pf_sighting_gps_valid, bytes:get_index(gps + 0))
      subtree:add(pf_sighting_gps_source, bytes:get_index(gps + 1))
      subtree:add(pf_sighting_gps_lat, le_i32(bytes, gps + 2))
      subtree:add(pf_sighting_gps_lon, le_i32(bytes, gps + 6))
      subtree:add(pf_sighting_gps_alt, le_i32(bytes, gps + 10))
      subtree:add(pf_sighting_gps_time, le_u32(bytes, gps + 14))
      subtree:add(pf_sighting_gps_accuracy, le_u16(bytes, gps + 18))
    end
  end
end

local function decode_ack_fields(subtree, bytes, payload_offset, payload_len)
  if payload_len < 1 then
    subtree:add(pf_parse_error, "ACK payload too short (need >=1 byte)")
    return
  end
  subtree:add(pf_ack_cmd, bytes:get_index(payload_offset + 0))
end

local function decode_error_fields(subtree, bytes, payload_offset, payload_len)
  if payload_len < 2 then
    subtree:add(pf_parse_error, "ERROR payload too short (need >=2 bytes)")
    return
  end
  subtree:add(pf_error_cmd, bytes:get_index(payload_offset + 0))
  subtree:add(pf_error_code, bytes:get_index(payload_offset + 1))
end

local function decode_config_fields(subtree, bytes, payload_offset, payload_len)
  if payload_len < 5 then
    subtree:add(pf_parse_error, "CONFIG payload too short (need 5 bytes)")
    return
  end
  subtree:add(pf_cfg_hop, le_u16(bytes, payload_offset + 0))
  subtree:add(pf_cfg_mask, le_u16(bytes, payload_offset + 2))
  subtree:add(pf_cfg_boot, bytes:get_index(payload_offset + 4))
end

local function decode_command_fields(subtree, bytes, payload_offset, payload_len)
  if payload_len < 1 then
    subtree:add(pf_parse_error, "COMMAND payload too short (need >=1 byte)")
    return
  end

  local cmd = bytes:get_index(payload_offset + 0)
  subtree:add(pf_cmd_id, cmd)

  if cmd == 0x03 and payload_len >= 3 then
    subtree:add(pf_cmd_hop, le_u16(bytes, payload_offset + 1))
  elseif cmd == 0x04 and payload_len >= 3 then
    subtree:add(pf_cmd_mask, le_u16(bytes, payload_offset + 1))
  elseif cmd == 0x05 and payload_len >= 2 then
    subtree:add(pf_cmd_boot, bytes:get_index(payload_offset + 1))
  elseif cmd == 0x08 and payload_len >= 28 then
    subtree:add(pf_cmd_gps_flags, bytes:get_index(payload_offset + 1))
    subtree:add(pf_cmd_gps_lat, le_i32(bytes, payload_offset + 2))
    subtree:add(pf_cmd_gps_lon, le_i32(bytes, payload_offset + 6))
    subtree:add(pf_cmd_gps_alt, le_i32(bytes, payload_offset + 10))
    subtree:add(pf_cmd_gps_speed, le_u32(bytes, payload_offset + 14))
    subtree:add(pf_cmd_gps_bearing, le_u32(bytes, payload_offset + 18))
    subtree:add(pf_cmd_gps_time, le_u32(bytes, payload_offset + 22))
    subtree:add(pf_cmd_gps_accuracy, le_u16(bytes, payload_offset + 26))
  elseif cmd == 0x09 and payload_len >= 13 then
    subtree:add(pf_cmd_replay_sid, le_u64_hex(bytes, payload_offset + 1))
    subtree:add(pf_cmd_replay_seq, le_u32(bytes, payload_offset + 9))
  elseif cmd == 0x0A and payload_len >= 2 then
    subtree:add(pf_cmd_replay_enable, bytes:get_index(payload_offset + 1))
  end
end

function wifinder.dissector(_, pinfo, tree)
  local value_fi = first_field(f_att_value)
  if value_fi == nil or value_fi.range == nil then
    return
  end

  local bytes = value_fi.range:bytes()
  local total_len = bytes:len()
  if total_len < 12 then
    return
  end

  if bytes:get_index(0) ~= 0x57 or bytes:get_index(1) ~= 0x47 then
    return
  end

  local version = bytes:get_index(2)
  if version ~= 1 and version ~= 2 then
    return
  end

  local msg_type = bytes:get_index(3)
  local seq = le_u16(bytes, 4)
  local payload_len = le_u16(bytes, 6)
  local device_ms = le_u32(bytes, 8)
  local payload_available = total_len - 12

  if payload_len > payload_available then
    return
  end

  local root = tree:add(wifinder, value_fi.range, "WIFINDER Frame")

  local opcode_num = fi_to_uint(first_field(f_att_opcode))
  if opcode_num ~= nil then
    root:add(pf_att_opcode, opcode_num)
  end

  local handle_num = fi_to_uint(first_field(f_att_handle))
  if handle_num ~= nil then
    root:add(pf_att_handle, handle_num)
  end

  root:add(pf_magic, "WG (0x57 0x47)")
  root:add(pf_version, version)
  root:add(pf_type, msg_type)
  root:add(pf_seq, seq)
  root:add(pf_len, payload_len)
  root:add(pf_device_ms, device_ms)

  local payload_tree = root:add(pf_payload_note, string.format("%d bytes", payload_len))
  local payload_offset = 12

  if msg_type == 0x01 then
    decode_status_fields(payload_tree, bytes, payload_offset, payload_len)
  elseif msg_type == 0x02 then
    decode_sighting_fields(payload_tree, bytes, payload_offset, payload_len)
  elseif msg_type == 0x03 then
    decode_ack_fields(payload_tree, bytes, payload_offset, payload_len)
  elseif msg_type == 0x04 then
    decode_error_fields(payload_tree, bytes, payload_offset, payload_len)
  elseif msg_type == 0x05 then
    -- SNAPSHOT_END has no payload.
  elseif msg_type == 0x06 then
    decode_config_fields(payload_tree, bytes, payload_offset, payload_len)
  elseif msg_type == 0x07 then
    decode_gps_fields(payload_tree, bytes, payload_offset, payload_len)
  elseif msg_type == 0x81 then
    decode_command_fields(payload_tree, bytes, payload_offset, payload_len)
  end

  local type_name = MSG_NAMES[msg_type]
  if type_name == nil then
    type_name = string.format("0x%02X", msg_type)
  end
  pinfo.cols.info:append(string.format(" | WG %s seq=%u", type_name, seq))
end

f_att_value = safe_field("btatt.value")
f_att_opcode = safe_field("btatt.opcode.method")
f_att_handle = safe_field("btatt.handle")

register_postdissector(wifinder)
