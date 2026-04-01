export const PROTOCOL_VERSION = 2;
const MAGIC0 = 0x57;
const MAGIC1 = 0x47;
const HEADER_SIZE = 12;

export const MESSAGE_TYPES = Object.freeze({
  STATUS: 0x01,
  SIGHTING: 0x02,
  ACK: 0x03,
  ERROR: 0x04,
  SNAPSHOT_END: 0x05,
  CONFIG: 0x06,
  GPS: 0x07,
  REPLAY_ACK: 0x08,
  NODE_TABLE: 0x09,
  COMMAND: 0x81,
});

export const COMMANDS = Object.freeze({
  START: 0x01,
  STOP: 0x02,
  SET_HOP_MS: 0x03,
  SET_CHANNEL_MASK: 0x04,
  SET_BOOT_MODE: 0x05,
  REQUEST_STATUS: 0x06,
  REQUEST_SNAPSHOT: 0x07,
  SET_GPS_FIX: 0x08,
  REPLAY_ACK: 0x09,
  SET_REPLAY: 0x0A,
  CLEAR_STORAGE: 0x0B,
});

export const AUTH_BY_CODE = Object.freeze({
  0: "UNKNOWN",
  1: "OPEN",
  2: "WEP?",
  3: "WPA",
  4: "WPA2/WPA3",
});

const SEC_PROTO = Object.freeze({
  WPA: 1 << 0,
  WPA2: 1 << 1,
  WPA3: 1 << 2,
});

const SEC_AKM = Object.freeze({
  EAP: 1 << 0,
  PSK: 1 << 1,
  SAE: 1 << 2,
  OWE: 1 << 3,
});

const SEC_CIPHER = Object.freeze({
  TKIP: 1 << 0,
  CCMP_128: 1 << 1,
  GCMP_128: 1 << 2,
  GCMP_256: 1 << 3,
  CCMP_256: 1 << 4,
  WEP: 1 << 5,
});

function formatSecurity(authCode, protoFlags, akmFlags, cipherFlags) {
  const parts = [];

  if ((protoFlags & (SEC_PROTO.WPA2 | SEC_PROTO.WPA3)) === (SEC_PROTO.WPA2 | SEC_PROTO.WPA3)) {
    parts.push("WPA2/WPA3");
  } else if (protoFlags & SEC_PROTO.WPA3) {
    parts.push("WPA3");
  } else if (protoFlags & SEC_PROTO.WPA2) {
    parts.push("WPA2");
  } else if (protoFlags & SEC_PROTO.WPA) {
    parts.push("WPA");
  } else if (authCode === 2 || (cipherFlags & SEC_CIPHER.WEP)) {
    parts.push("WEP");
  } else if (authCode === 1) {
    parts.push("OPEN");
  } else {
    return AUTH_BY_CODE[authCode] ?? "UNKNOWN";
  }

  if ((akmFlags & (SEC_AKM.PSK | SEC_AKM.SAE)) === (SEC_AKM.PSK | SEC_AKM.SAE)) {
    parts.push("PSK/SAE");
  } else if (akmFlags & SEC_AKM.SAE) {
    parts.push("SAE");
  } else if (akmFlags & SEC_AKM.PSK) {
    parts.push("PSK");
  } else if (akmFlags & SEC_AKM.EAP) {
    parts.push("EAP");
  } else if (akmFlags & SEC_AKM.OWE) {
    parts.push("OWE");
  }

  if (cipherFlags & SEC_CIPHER.CCMP_256) {
    parts.push("CCMP-256");
  } else if (cipherFlags & SEC_CIPHER.GCMP_256) {
    parts.push("GCMP-256");
  } else if (cipherFlags & SEC_CIPHER.GCMP_128) {
    parts.push("GCMP-128");
  } else if (cipherFlags & SEC_CIPHER.CCMP_128) {
    parts.push("CCMP-128");
  } else if (cipherFlags & SEC_CIPHER.TKIP) {
    parts.push("TKIP");
  } else if (cipherFlags & SEC_CIPHER.WEP) {
    parts.push("WEP");
  }

  return parts.join("-");
}

function toBytes(input) {
  if (input instanceof Uint8Array) return input;
  if (input instanceof ArrayBuffer) return new Uint8Array(input);
  throw new Error("Expected Uint8Array or ArrayBuffer");
}

function readU16(view, offset) {
  return view.getUint16(offset, true);
}

function readU32(view, offset) {
  return view.getUint32(offset, true);
}

function readU64Hex(view, offset) {
  const lo = readU32(view, offset);
  const hi = readU32(view, offset + 4);
  return `0x${hi.toString(16).padStart(8, "0")}${lo.toString(16).padStart(8, "0")}`;
}

function writeU16(view, offset, value) {
  view.setUint16(offset, value, true);
}

function writeU32(view, offset, value) {
  view.setUint32(offset, value, true);
}

export function encodeFrame({ type, seq, deviceMs, payload = new Uint8Array(0) }) {
  const payloadBytes = toBytes(payload);
  const out = new Uint8Array(HEADER_SIZE + payloadBytes.length);
  const view = new DataView(out.buffer);
  out[0] = MAGIC0;
  out[1] = MAGIC1;
  out[2] = PROTOCOL_VERSION;
  out[3] = type;
  writeU16(view, 4, seq & 0xffff);
  writeU16(view, 6, payloadBytes.length);
  writeU32(view, 8, deviceMs >>> 0);
  out.set(payloadBytes, HEADER_SIZE);
  return out;
}

export function decodeFrame(input) {
  const bytes = toBytes(input);
  if (bytes.length < HEADER_SIZE) throw new Error("Frame too short");
  if (bytes[0] !== MAGIC0 || bytes[1] !== MAGIC1) throw new Error("Bad frame magic");
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const version = bytes[2];
  const type = bytes[3];
  const seq = readU16(view, 4);
  const len = readU16(view, 6);
  const deviceMs = readU32(view, 8);
  if (bytes.length < HEADER_SIZE + len) throw new Error("Truncated frame");
  return {
    version,
    type,
    seq,
    len,
    deviceMs,
    payload: bytes.slice(HEADER_SIZE, HEADER_SIZE + len),
  };
}

export function encodeCommandFrame({ seq, deviceMs, commandId, payload = new Uint8Array(0) }) {
  const payloadBytes = toBytes(payload);
  const cmdPayload = new Uint8Array(1 + payloadBytes.length);
  cmdPayload[0] = commandId;
  cmdPayload.set(payloadBytes, 1);
  return encodeFrame({
    type: MESSAGE_TYPES.COMMAND,
    seq,
    deviceMs,
    payload: cmdPayload,
  });
}

export function decodeStatusPayload(payload) {
  const bytes = toBytes(payload);
  if (bytes.length < 14) throw new Error("Bad status payload");
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const gpsValid = bytes.length >= 15 ? bytes[14] === 1 : false;
  const gpsAgeS = bytes.length >= 17 ? readU16(view, 15) : 0;
  const gpsAccuracyDm = bytes.length >= 19 ? readU16(view, 17) : 0;
  const nodeLinkUp = bytes.length >= 20 ? bytes[19] === 1 : false;
  const nodeLastSeenS = bytes.length >= 22 ? readU16(view, 20) : 0;
  const nodePacketsPerSec = bytes.length >= 24 ? readU16(view, 22) : 0;
  const nodeForwardedSightings = bytes.length >= 26 ? readU16(view, 24) : 0;
  const nodeChannel = bytes.length >= 27 ? bytes[26] : 0;
  const nodeChannelMask = bytes.length >= 29 ? readU16(view, 27) : 0;
  const sessionOpen = bytes.length >= 30 ? bytes[29] === 1 : false;
  const sessionIdHex = bytes.length >= 38 ? readU64Hex(view, 30) : "0x0000000000000000";
  const queuedRecords = bytes.length >= 42 ? readU32(view, 38) : 0;
  const queuedBytes = bytes.length >= 46 ? readU32(view, 42) : 0;
  const replayActive = bytes.length >= 47 ? bytes[46] === 1 : false;
  const replayCursor = bytes.length >= 51 ? readU32(view, 47) : 0;
  const queueFull = bytes.length >= 52 ? bytes[51] === 1 : false;
  const droppedFlashFull = bytes.length >= 56 ? readU32(view, 52) : 0;
  const nodeCount = bytes.length >= 57 ? bytes[56] : 0;
  return {
    scanning: bytes[0] === 1,
    bleEncrypted: bytes[1] === 1,
    currentChannel: bytes[2],
    hopMs: readU16(view, 3),
    channelMask: readU16(view, 5),
    uniqueBssids: readU16(view, 7),
    packetsPerSec: readU16(view, 9),
    droppedNotifies: readU16(view, 11),
    bootMode: bytes[13],
    gpsValid,
    gpsAgeS,
    gpsAccuracyDm,
    nodeLinkUp,
    nodeLastSeenS,
    nodePacketsPerSec,
    nodeForwardedSightings,
    nodeChannel,
    nodeChannelMask,
    sessionOpen,
    sessionIdHex,
    queuedRecords,
    queuedBytes,
    replayActive,
    replayCursor,
    queueFull,
    droppedFlashFull,
    nodeCount,
  };
}

export function bssidBytesToString(bytes) {
  return Array.from(bytes)
    .map((b) => b.toString(16).padStart(2, "0").toUpperCase())
    .join(":");
}

export function decodeSightingPayload(payload) {
  const bytes = toBytes(payload);
  if (bytes.length < 14) throw new Error("Bad sighting payload");
  const bssid = bssidBytesToString(bytes.slice(0, 6));
  const channel = bytes[6];
  const rssi = bytes[7] > 127 ? bytes[7] - 256 : bytes[7];
  const authCode = bytes[8];
  const protoFlags = bytes[9];
  const akmFlags = bytes[10];
  const cipherFlags = bytes[11];
  const ssidLen = Math.min(bytes[12], 32);
  const dataLen = 14 + ssidLen;
  if (bytes.length < dataLen) throw new Error("Truncated sighting payload");
  const ssidBytes = bytes.slice(13, 13 + ssidLen);
  const flags = bytes[13 + ssidLen];
  const ssid = new TextDecoder().decode(ssidBytes);
  let sessionIdHex = "0x0000000000000000";
  let recordSeq = 0;
  let nodeId = 0;
  let sourceFlags = 0;
  if (bytes.length >= dataLen + 14) {
    const trailer = new DataView(bytes.buffer, bytes.byteOffset + dataLen, 14);
    sessionIdHex = readU64Hex(trailer, 0);
    recordSeq = readU32(trailer, 8);
    nodeId = trailer.getUint8(12);
    sourceFlags = trailer.getUint8(13);
  }
  return {
    bssid,
    channel,
    rssi,
    auth: formatSecurity(authCode, protoFlags, akmFlags, cipherFlags),
    authCode,
    protoFlags,
    akmFlags,
    cipherFlags,
    ssid,
    flags,
    sessionIdHex,
    recordSeq,
    nodeId,
    sourceFlags,
  };
}
