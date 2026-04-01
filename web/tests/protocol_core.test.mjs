import test from "node:test";
import assert from "node:assert/strict";

import {
  COMMANDS,
  MESSAGE_TYPES,
  PROTOCOL_VERSION,
  decodeSightingPayload,
  decodeStatusPayload,
  decodeFrame,
  encodeCommandFrame,
} from "../src/protocol-core.js";

test("command frame roundtrip", () => {
  const frame = encodeCommandFrame({
    seq: 7,
    deviceMs: 1200,
    commandId: COMMANDS.SET_HOP_MS,
    payload: Uint8Array.of(0xf4, 0x01),
  });

  const decoded = decodeFrame(frame);
  assert.equal(decoded.version, PROTOCOL_VERSION);
  assert.equal(decoded.type, MESSAGE_TYPES.COMMAND);
  assert.equal(decoded.seq, 7);
  assert.equal(decoded.deviceMs, 1200);
  assert.deepEqual(
    Array.from(decoded.payload),
    [COMMANDS.SET_HOP_MS, 0xf4, 0x01],
    "command payload should include command id + args"
  );
});

test("sighting payload decodes detailed security labels", () => {
  const bssid = [0x7c, 0x9e, 0xbd, 0x11, 0x22, 0x33];
  const ssid = Array.from(new TextEncoder().encode("LabNet"));
  const payload = Uint8Array.from([
    ...bssid,
    6, // channel
    0xc7, // rssi (-57)
    4, // coarse auth: WPA2/WPA3
    0x06, // proto: WPA2|WPA3
    0x06, // akm: PSK|SAE
    0x02, // cipher: CCMP-128
    ssid.length,
    ...ssid,
    0x01, // flags
    0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // session id (LE u64)
    0x41, 0x01, 0x00, 0x00, // record seq 321
    0x01, // node id
    0x02, // source flags
  ]);

  const decoded = decodeSightingPayload(payload);
  assert.equal(decoded.bssid, "7C:9E:BD:11:22:33");
  assert.equal(decoded.rssi, -57);
  assert.equal(decoded.ssid, "LabNet");
  assert.equal(decoded.auth, "WPA2/WPA3-PSK/SAE-CCMP-128");
  assert.equal(decoded.sessionIdHex, "0x1122334455667788");
  assert.equal(decoded.recordSeq, 321);
  assert.equal(decoded.nodeId, 1);
  assert.equal(decoded.sourceFlags, 2);
});

test("status payload decodes optional gps fields", () => {
  const payload = Uint8Array.from([
    1, // scanning
    1, // bleEncrypted
    11, // channel
    0xfa, 0x00, // hopMs 250
    0x01, 0x10, // channel mask 0x1001
    0x4d, 0x00, // unique bssid 77
    0x78, 0x00, // packets/s 120
    0x02, 0x00, // drops 2
    0x01, // boot auto
    0x01, // gps valid
    0x07, 0x00, // gps age 7s
    0x0e, 0x00, // gps accuracy 14dm
    0x01, // node link up
    0x04, 0x00, // node age 4s
    0x21, 0x00, // node packets/s 33
    0x2c, 0x00, // node forwarded 44
    0x06, // node channel
    0xaa, 0x0a, // node mask 0x0AAA
    0x01, // session open
    0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // session id (LE u64)
    0xd2, 0x04, 0x00, 0x00, // queued records 1234
    0xd5, 0xdd, 0x00, 0x00, // queued bytes 56789
    0x01, // replay active
    0xbc, 0x01, 0x00, 0x00, // replay cursor 444
    0x01, // queue full
    0x7b, 0x00, 0x00, 0x00, // dropped flash full 123
    0x02, // node count
  ]);

  const decoded = decodeStatusPayload(payload);
  assert.equal(decoded.gpsValid, true);
  assert.equal(decoded.gpsAgeS, 7);
  assert.equal(decoded.gpsAccuracyDm, 14);
  assert.equal(decoded.nodeLinkUp, true);
  assert.equal(decoded.nodePacketsPerSec, 33);
  assert.equal(decoded.nodeChannelMask, 0x0aaa);
  assert.equal(decoded.sessionOpen, true);
  assert.equal(decoded.sessionIdHex, "0x1122334455667788");
  assert.equal(decoded.queuedRecords, 1234);
  assert.equal(decoded.queuedBytes, 56789);
  assert.equal(decoded.replayActive, true);
  assert.equal(decoded.replayCursor, 444);
  assert.equal(decoded.queueFull, true);
  assert.equal(decoded.droppedFlashFull, 123);
  assert.equal(decoded.nodeCount, 2);
});
