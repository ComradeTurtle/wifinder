function rssiBucket(rssi) {
  if (rssi >= -50) return "-50..0";
  if (rssi >= -60) return "-60..-51";
  if (rssi >= -70) return "-70..-61";
  if (rssi >= -80) return "-80..-71";
  return "<=-81";
}

function normalizeSeenMs(seenMs, nowMs) {
  if (!Number.isFinite(seenMs) || seenMs <= 0) {
    return nowMs;
  }
  const maxClockSkewMs = 24 * 60 * 60 * 1000;
  if (Math.abs(nowMs - seenMs) > maxClockSkewMs) {
    return nowMs;
  }
  return seenMs;
}

export function createState() {
  return {
    sightings: new Map(),
    ssidMemory: new Map(),
  };
}

export function applySighting(state, sighting) {
  const normalizedSeenMs = normalizeSeenMs(sighting.seenMs, Date.now());
  const existing = state.sightings.get(sighting.bssid);
  const previousSsid = existing?.ssid || state.ssidMemory.get(sighting.bssid) || "";
  if (sighting.ssid && sighting.ssid.length > 0) {
    state.ssidMemory.set(sighting.bssid, sighting.ssid);
  }
  const rememberedSsid = state.ssidMemory.get(sighting.bssid) ?? "";
  const nextSsid = rememberedSsid || previousSsid || sighting.ssid || "";
  state.sightings.set(sighting.bssid, {
    bssid: sighting.bssid,
    ssid: nextSsid,
    channel: sighting.channel,
    rssi: sighting.rssi,
    auth: sighting.auth,
    seenMs: normalizedSeenMs,
    firstSeenMs: existing?.firstSeenMs ?? normalizedSeenMs,
    seenCount: (existing?.seenCount ?? 0) + 1,
  });
}

export function pruneSightings(state, nowMs, maxAgeMs) {
  let removed = 0;
  for (const [bssid, row] of state.sightings.entries()) {
    if (nowMs - row.seenMs > maxAgeMs) {
      state.sightings.delete(bssid);
      removed += 1;
    }
  }
  return removed;
}

export function summarizeState(state) {
  const channelCounts = new Map();
  const rssiHistogram = new Map();

  for (const row of state.sightings.values()) {
    channelCounts.set(row.channel, (channelCounts.get(row.channel) ?? 0) + 1);
    const bucket = rssiBucket(row.rssi);
    rssiHistogram.set(bucket, (rssiHistogram.get(bucket) ?? 0) + 1);
  }

  return {
    uniqueBssids: state.sightings.size,
    channelCounts,
    rssiHistogram,
  };
}

export function toRows(state, nowMs, maxAgeMs = Number.POSITIVE_INFINITY) {
  return Array.from(state.sightings.values())
    .filter((row) => nowMs - row.seenMs <= maxAgeMs)
    .map((row) => ({
      ...row,
      ageMs: Math.max(0, nowMs - row.seenMs),
    }))
    .sort((a, b) => b.seenMs - a.seenMs);
}
