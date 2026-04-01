import test from "node:test";
import assert from "node:assert/strict";

import { applySighting, createState, pruneSightings, summarizeState, toRows } from "../src/state-core.js";

test("sighting map upsert and counters", () => {
  const state = createState();

  applySighting(state, {
    bssid: "7C:9E:BD:11:22:33",
    ssid: "LabNet",
    channel: 6,
    rssi: -51,
    auth: "WPA2/WPA3",
    seenMs: 1000,
  });

  applySighting(state, {
    bssid: "7C:9E:BD:11:22:33",
    ssid: "LabNet",
    channel: 6,
    rssi: -52,
    auth: "WPA2/WPA3",
    seenMs: 2000,
  });

  applySighting(state, {
    bssid: "AA:BB:CC:00:11:22",
    ssid: "Guest",
    channel: 11,
    rssi: -68,
    auth: "OPEN",
    seenMs: 3000,
  });

  const summary = summarizeState(state);
  assert.equal(summary.uniqueBssids, 2);
  assert.equal(summary.channelCounts.get(6), 1);
  assert.equal(summary.channelCounts.get(11), 1);
});

test("stale sightings are pruned by timeout", () => {
  const state = createState();
  const base = 1_710_000_000_000;
  const originalNow = Date.now;
  Date.now = () => base;
  try {
    applySighting(state, {
      bssid: "10:20:30:40:50:60",
      ssid: "Fresh",
      channel: 1,
      rssi: -45,
      auth: "OPEN",
      seenMs: base - 2_000,
    });

    applySighting(state, {
      bssid: "11:22:33:44:55:66",
      ssid: "Old",
      channel: 6,
      rssi: -72,
      auth: "WPA2",
      seenMs: base - 9_000,
    });
  } finally {
    Date.now = originalNow;
  }

  const removed = pruneSightings(state, base, 4_000);
  assert.equal(removed, 1);
  assert.equal(state.sightings.size, 1);
  assert.equal(state.sightings.has("10:20:30:40:50:60"), true);
});

test("row conversion applies age filter and ordering", () => {
  const state = createState();
  const base = 1_710_000_000_000;
  const originalNow = Date.now;
  Date.now = () => base;
  try {
    applySighting(state, {
      bssid: "AA:AA:AA:AA:AA:AA",
      ssid: "Near",
      channel: 3,
      rssi: -55,
      auth: "WPA2",
      seenMs: base - 1_000,
    });

    applySighting(state, {
      bssid: "BB:BB:BB:BB:BB:BB",
      ssid: "Far",
      channel: 9,
      rssi: -75,
      auth: "OPEN",
      seenMs: base - 8_000,
    });
  } finally {
    Date.now = originalNow;
  }

  const rows = toRows(state, base, 2_500);
  assert.equal(rows.length, 1);
  assert.equal(rows[0].bssid, "AA:AA:AA:AA:AA:AA");
  assert.equal(rows[0].ageMs, 1_000);
});

test("device-uptime timestamps are normalized for visibility windows", () => {
  const state = createState();
  const originalNow = Date.now;

  Date.now = () => 1_710_000_000_000;
  try {
    applySighting(state, {
      bssid: "CC:CC:CC:CC:CC:CC",
      ssid: "UptimeClock",
      channel: 4,
      rssi: -62,
      auth: "WPA2",
      seenMs: 12_345,
    });
  } finally {
    Date.now = originalNow;
  }

  const rows = toRows(state, 1_710_000_000_000, 30_000);
  assert.equal(rows.length, 1);
});

test("non-empty SSID is retained when later sightings report empty SSID", () => {
  const state = createState();

  applySighting(state, {
    bssid: "DE:AD:BE:EF:00:01",
    ssid: "OfficeWiFi",
    channel: 1,
    rssi: -60,
    auth: "WPA2-PSK-CCMP-128",
    seenMs: 1000,
  });

  applySighting(state, {
    bssid: "DE:AD:BE:EF:00:01",
    ssid: "",
    channel: 1,
    rssi: -62,
    auth: "WPA2-PSK-CCMP-128",
    seenMs: 2000,
  });

  const rows = toRows(state, 2000);
  assert.equal(rows.length, 1);
  assert.equal(rows[0].ssid, "OfficeWiFi");
});

test("new non-empty SSID replaces older SSID for same BSSID", () => {
  const state = createState();

  applySighting(state, {
    bssid: "40:ED:00:25:E0:02",
    ssid: "Red Dragon",
    channel: 1,
    rssi: -58,
    auth: "WPA2-PSK-CCMP-128",
    seenMs: 1000,
  });

  applySighting(state, {
    bssid: "40:ED:00:25:E0:02",
    ssid: "ed Dragon",
    channel: 1,
    rssi: -60,
    auth: "WPA2-PSK-CCMP-128",
    seenMs: 2000,
  });

  const rows = toRows(state, 2000);
  assert.equal(rows.length, 1);
  assert.equal(rows[0].ssid, "ed Dragon");
});
