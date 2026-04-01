/* eslint-disable @typescript-eslint/no-explicit-any */
import * as core from "./protocol-core.js";

export const PROTOCOL_VERSION: number = core.PROTOCOL_VERSION;
export const MESSAGE_TYPES = core.MESSAGE_TYPES as {
  STATUS: number;
  SIGHTING: number;
  ACK: number;
  ERROR: number;
  SNAPSHOT_END: number;
  CONFIG: number;
  GPS: number;
  REPLAY_ACK: number;
  NODE_TABLE: number;
  COMMAND: number;
};
export const COMMANDS = core.COMMANDS as {
  START: number;
  STOP: number;
  SET_HOP_MS: number;
  SET_CHANNEL_MASK: number;
  SET_BOOT_MODE: number;
  REQUEST_STATUS: number;
  REQUEST_SNAPSHOT: number;
  SET_GPS_FIX: number;
  REPLAY_ACK: number;
  SET_REPLAY: number;
  CLEAR_STORAGE: number;
};

export const decodeFrame = core.decodeFrame as (input: Uint8Array | ArrayBuffer) => {
  version: number;
  type: number;
  seq: number;
  len: number;
  deviceMs: number;
  payload: Uint8Array;
};

export const encodeCommandFrame = core.encodeCommandFrame as (arg: {
  seq: number;
  deviceMs: number;
  commandId: number;
  payload?: Uint8Array;
}) => Uint8Array;

export const decodeStatusPayload = core.decodeStatusPayload as (payload: Uint8Array) => {
  scanning: boolean;
  bleEncrypted: boolean;
  currentChannel: number;
  hopMs: number;
  channelMask: number;
  uniqueBssids: number;
  packetsPerSec: number;
  droppedNotifies: number;
  bootMode: number;
  gpsValid: boolean;
  gpsAgeS: number;
  gpsAccuracyDm: number;
  nodeLinkUp: boolean;
  nodeLastSeenS: number;
  nodePacketsPerSec: number;
  nodeForwardedSightings: number;
  nodeChannel: number;
  nodeChannelMask: number;
  sessionOpen: boolean;
  sessionIdHex: string;
  queuedRecords: number;
  queuedBytes: number;
  replayActive: boolean;
  replayCursor: number;
  queueFull: boolean;
  droppedFlashFull: number;
  nodeCount: number;
};

export const decodeSightingPayload = core.decodeSightingPayload as (
  payload: Uint8Array
) => {
  bssid: string;
  channel: number;
  rssi: number;
  auth: string;
  authCode: number;
  ssid: string;
  flags: number;
  sessionIdHex: string;
  recordSeq: number;
  nodeId: number;
  sourceFlags: number;
};
