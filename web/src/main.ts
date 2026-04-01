import "./styles.css";

import { EspWigleBleClient } from "./ble";
import { decodeSightingPayload, decodeStatusPayload, MESSAGE_TYPES } from "./protocol";
import { applySighting, createState, pruneSightings, summarizeState, toRows } from "./state";

type StatusView = {
  connected: boolean;
  scanning: boolean;
  bleEncrypted: boolean;
  currentChannel: number;
  hopMs: number;
  channelMask: number;
  uniqueBssids: number;
  packetsPerSec: number;
  droppedNotifies: number;
  bootMode: number;
};

const status: StatusView = {
  connected: false,
  scanning: false,
  bleEncrypted: false,
  currentChannel: 1,
  hopMs: 250,
  channelMask: 0x1fff,
  uniqueBssids: 0,
  packetsPerSec: 0,
  droppedNotifies: 0,
  bootMode: 0,
};

const appState = createState();
const ppsHistory: number[] = [];
const DEFAULT_VISIBLE_TIMEOUT_SEC = 25;
const MIN_VISIBLE_TIMEOUT_SEC = 5;
const MAX_VISIBLE_TIMEOUT_SEC = 300;
let visibleTimeoutSec = DEFAULT_VISIBLE_TIMEOUT_SEC;

function el<T extends HTMLElement>(id: string): T {
  const node = document.getElementById(id);
  if (!node) throw new Error(`Missing element #${id}`);
  return node as T;
}

function pushLog(message: string) {
  const now = new Date().toLocaleTimeString();
  const logEl = el<HTMLDivElement>("log");
  logEl.textContent = `[${now}] ${message}\n${logEl.textContent}`.slice(0, 5000);
}

function controlLinkReady() {
  return status.connected && status.bleEncrypted;
}

function requireControlLink(action: string) {
  if (controlLinkReady()) return true;
  pushLog(`${action} blocked: waiting for encrypted BLE link`);
  return false;
}

async function waitForEncryptedLink(timeoutMs: number) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (controlLinkReady()) {
      return true;
    }
    await new Promise((resolve) => setTimeout(resolve, 200));
  }
  return controlLinkReady();
}

function channelMaskFromUI() {
  const checks = document.querySelectorAll<HTMLInputElement>(".channel-check");
  let mask = 0;
  checks.forEach((check) => {
    if (!check.checked) return;
    const channel = Number(check.value);
    if (channel >= 1 && channel <= 13) {
      mask |= 1 << (channel - 1);
    }
  });
  return mask;
}

function applyMaskToUI(mask: number) {
  const checks = document.querySelectorAll<HTMLInputElement>(".channel-check");
  checks.forEach((check) => {
    const channel = Number(check.value);
    const bit = 1 << (channel - 1);
    check.checked = (mask & bit) !== 0;
  });
}

function renderStatus() {
  el("connState").textContent = status.connected ? "Connected" : "Disconnected";
  el("scanState").textContent = status.scanning ? "Running" : "Stopped";
  el("secureState").textContent = status.bleEncrypted ? "Encrypted" : "Unencrypted";
  el("chanState").textContent = String(status.currentChannel);
  el("hopState").textContent = `${status.hopMs} ms`;
  el("uniqueState").textContent = String(status.uniqueBssids);
  el("ppsState").textContent = String(status.packetsPerSec);
  el("dropState").textContent = String(status.droppedNotifies);
  el("bootState").textContent = status.bootMode === 1 ? "Auto" : "Manual";

  const hopInput = el<HTMLInputElement>("hopInput");
  const hopRange = el<HTMLInputElement>("hopRange");
  if (document.activeElement !== hopInput) hopInput.value = String(status.hopMs);
  if (document.activeElement !== hopRange) hopRange.value = String(status.hopMs);
}

function renderTable() {
  const nowMs = Date.now();
  pruneSightings(appState, nowMs, visibleTimeoutSec * 1000);
  const rows = toRows(appState, nowMs, visibleTimeoutSec * 1000);
  el("visibleNowState").textContent = `${rows.length} now | ${visibleTimeoutSec}s timeout`;
  const tbody = el<HTMLTableSectionElement>("sightingsBody");
  tbody.innerHTML = rows
    .slice(0, 400)
    .map(
      (row) => `<tr>
  <td class="mono">${row.bssid}</td>
  <td>${row.ssid || "<hidden>"}</td>
  <td>${row.auth}</td>
  <td>${row.channel}</td>
  <td>${row.rssi}</td>
  <td class="mono">${(row.ageMs / 1000).toFixed(1)}s</td>
</tr>`
    )
    .join("");
}

function renderBars(containerId: string, entries: Array<{ label: string; value: number }>) {
  const max = entries.reduce((m, e) => Math.max(m, e.value), 1);
  const container = el<HTMLDivElement>(containerId);
  container.innerHTML = entries
    .map((entry) => {
      const pct = Math.round((entry.value / max) * 100);
      return `<div class="bar-row">
  <span class="bar-label">${entry.label}</span>
  <div class="bar-track"><div class="bar-fill" style="width:${pct}%"></div></div>
  <span class="bar-value">${entry.value}</span>
</div>`;
    })
    .join("");
}

function renderSparkline() {
  const canvas = el<HTMLCanvasElement>("ppsSpark");
  const ctx = canvas.getContext("2d");
  if (!ctx) return;
  const w = canvas.width;
  const h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = "#11201f";
  ctx.fillRect(0, 0, w, h);
  ctx.strokeStyle = "#2ec4a6";
  ctx.lineWidth = 2;
  if (ppsHistory.length < 2) return;
  const max = Math.max(...ppsHistory, 1);
  ctx.beginPath();
  ppsHistory.forEach((value, idx) => {
    const x = (idx / (ppsHistory.length - 1)) * (w - 1);
    const y = h - (value / max) * (h - 4) - 2;
    if (idx === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.stroke();
}

function renderOps() {
  pruneSightings(appState, Date.now(), visibleTimeoutSec * 1000);
  const summary = summarizeState(appState);
  const channels = Array.from(summary.channelCounts.entries())
    .sort((a, b) => a[0] - b[0])
    .map(([channel, value]) => ({ label: `Ch ${channel}`, value }));
  renderBars("channelBars", channels);

  const rssiOrder = ["-50..0", "-60..-51", "-70..-61", "-80..-71", "<=-81"];
  const rssiEntries = rssiOrder.map((label) => ({
    label,
    value: summary.rssiHistogram.get(label) ?? 0,
  }));
  renderBars("rssiBars", rssiEntries);

  ppsHistory.push(status.packetsPerSec);
  while (ppsHistory.length > 60) ppsHistory.shift();
  renderSparkline();
}

function setupUI() {
  const app = el<HTMLDivElement>("app");
  app.innerHTML = `
<main class="layout">
  <header class="hero">
    <div>
      <h1>ESPWIGLE Console</h1>
      <p>BLE control + live wardriving telemetry</p>
    </div>
    <button id="connectBtn" class="accent">Connect</button>
  </header>

  <section class="grid two">
    <article class="panel">
      <h2>Status</h2>
      <div class="kv">
        <span>BLE</span><strong id="connState">Disconnected</strong>
        <span>Scanner</span><strong id="scanState">Stopped</strong>
        <span>Link</span><strong id="secureState">Unencrypted</strong>
        <span>Channel</span><strong id="chanState">1</strong>
        <span>Hop</span><strong id="hopState">250 ms</strong>
        <span>Unique BSSID</span><strong id="uniqueState">0</strong>
        <span>Packets/s</span><strong id="ppsState">0</strong>
        <span>Notify Drops</span><strong id="dropState">0</strong>
        <span>Boot Mode</span><strong id="bootState">Manual</strong>
      </div>
    </article>

	    <article class="panel">
	      <h2>Controls</h2>
	      <div class="control-row">
	        <button id="startBtn" class="accent">Start</button>
        <button id="stopBtn">Stop</button>
        <button id="clearBtn">Clear View</button>
      </div>
      <label>Hop interval</label>
      <div class="control-row">
        <input id="hopRange" type="range" min="50" max="2000" value="250" />
        <input id="hopInput" type="number" min="50" max="2000" value="250" />
        <button id="applyHopBtn">Apply</button>
      </div>
	      <label>Boot mode</label>
	      <div class="control-row">
	        <select id="bootMode">
	          <option value="0">Manual</option>
	          <option value="1">Auto</option>
	        </select>
	        <button id="applyBootBtn">Apply</button>
	      </div>
	      <label>Visible timeout (seconds)</label>
	      <div class="control-row">
	        <input id="visibleTimeoutInput" type="number" min="5" max="300" value="25" />
	        <button id="applyVisibleTimeoutBtn">Apply</button>
	      </div>
	      <label>Channels (1-13)</label>
	      <div id="channelGrid" class="channel-grid"></div>
	      <button id="applyChannelsBtn">Apply Channel Set</button>
    </article>
  </section>

  <section class="grid two">
    <article class="panel">
      <h2>Ops</h2>
      <h3>Packets/s Trend</h3>
      <canvas id="ppsSpark" width="280" height="80"></canvas>
      <h3>Channel Occupancy</h3>
      <div id="channelBars" class="bars"></div>
      <h3>RSSI Histogram</h3>
      <div id="rssiBars" class="bars"></div>
    </article>
    <article class="panel">
      <h2>Event Log</h2>
      <div id="log" class="log"></div>
    </article>
  </section>

	  <section class="panel">
	    <h2>Visible (B)SSIDs <span id="visibleNowState" class="inline-state mono">0 now | 25s timeout</span></h2>
	    <div class="table-wrap">
	      <table>
	        <thead>
          <tr>
            <th>BSSID</th>
            <th>SSID</th>
	            <th>Auth</th>
	            <th>Ch</th>
	            <th>RSSI</th>
	            <th>Age</th>
	          </tr>
	        </thead>
	        <tbody id="sightingsBody"></tbody>
      </table>
    </div>
  </section>
</main>`;

  const grid = el<HTMLDivElement>("channelGrid");
  for (let c = 1; c <= 13; c += 1) {
    const id = `ch-${c}`;
    const wrapper = document.createElement("label");
    wrapper.className = "channel-item";
    wrapper.innerHTML = `<input class="channel-check" id="${id}" type="checkbox" value="${c}" checked />${c}`;
    grid.appendChild(wrapper);
  }
}

setupUI();
renderStatus();
renderTable();
renderOps();

const client = new EspWigleBleClient({
  onFrame: (frame) => {
    if (frame.type === MESSAGE_TYPES.STATUS) {
      const payload = decodeStatusPayload(frame.payload);
      status.scanning = payload.scanning;
      status.bleEncrypted = payload.bleEncrypted;
      status.currentChannel = payload.currentChannel;
      status.hopMs = payload.hopMs;
      status.channelMask = payload.channelMask;
      status.uniqueBssids = payload.uniqueBssids;
      status.packetsPerSec = payload.packetsPerSec;
      status.droppedNotifies = payload.droppedNotifies;
      status.bootMode = payload.bootMode;
      applyMaskToUI(payload.channelMask);
      el<HTMLSelectElement>("bootMode").value = String(payload.bootMode);
      renderStatus();
      renderOps();
      return;
    }

    if (frame.type === MESSAGE_TYPES.SIGHTING) {
      const s = decodeSightingPayload(frame.payload);
      applySighting(appState, {
        bssid: s.bssid,
        ssid: s.ssid,
        channel: s.channel,
        rssi: s.rssi,
        auth: s.auth,
        seenMs: frame.deviceMs,
      });
      renderTable();
      renderOps();
      return;
    }

    if (frame.type === MESSAGE_TYPES.ACK) {
      const cmd = frame.payload[0] ?? 0;
      pushLog(`ACK cmd=0x${cmd.toString(16).padStart(2, "0")}`);
      return;
    }
    if (frame.type === MESSAGE_TYPES.ERROR) {
      const cmd = frame.payload[0] ?? 0;
      const code = frame.payload[1] ?? 0;
      pushLog(`ERROR cmd=0x${cmd.toString(16).padStart(2, "0")} code=${code}`);
      return;
    }
    if (frame.type === MESSAGE_TYPES.SNAPSHOT_END) {
      pushLog("Snapshot complete");
    }
  },
  onConnectState: (connected) => {
    status.connected = connected;
    if (!connected) {
      status.bleEncrypted = false;
    }
    el<HTMLButtonElement>("connectBtn").textContent = connected ? "Disconnect" : "Connect";
    renderStatus();
  },
  onError: (message) => pushLog(`ERR: ${message}`),
  onInfo: (message) => pushLog(message),
});

el<HTMLButtonElement>("connectBtn").addEventListener("click", async () => {
  try {
    if (client.isConnected()) {
      await client.disconnect();
      return;
    }
    await client.requestAndConnect();
    pushLog("GATT connected; waiting for encrypted session...");
    const secure = await waitForEncryptedLink(8000);
    if (!secure) {
      pushLog("Encrypted session not ready. Forget/re-pair the ESP device, then reconnect.");
      return;
    }
    await client.requestStatus();
    await client.requestSnapshot();
  } catch (err) {
    pushLog(`Connect failed: ${(err as Error).message}`);
  }
});

el<HTMLButtonElement>("startBtn").addEventListener("click", async () => {
  if (!requireControlLink("Start")) return;
  try {
    await client.sendStart();
  } catch (err) {
    pushLog(`Start failed: ${(err as Error).message}`);
  }
});

el<HTMLButtonElement>("stopBtn").addEventListener("click", async () => {
  if (!requireControlLink("Stop")) return;
  try {
    await client.sendStop();
  } catch (err) {
    pushLog(`Stop failed: ${(err as Error).message}`);
  }
});

el<HTMLInputElement>("hopRange").addEventListener("input", (ev) => {
  const value = Number((ev.target as HTMLInputElement).value);
  el<HTMLInputElement>("hopInput").value = String(value);
});

el<HTMLInputElement>("hopInput").addEventListener("input", (ev) => {
  const value = Number((ev.target as HTMLInputElement).value);
  if (Number.isFinite(value)) {
    el<HTMLInputElement>("hopRange").value = String(Math.max(50, Math.min(2000, value)));
  }
});

el<HTMLButtonElement>("applyHopBtn").addEventListener("click", async () => {
  if (!requireControlLink("Set hop")) return;
  const hop = Number(el<HTMLInputElement>("hopInput").value);
  if (!Number.isFinite(hop) || hop < 50 || hop > 2000) {
    pushLog("Hop ms must be 50..2000");
    return;
  }
  try {
    await client.setHopMs(hop);
  } catch (err) {
    pushLog(`Set hop failed: ${(err as Error).message}`);
  }
});

el<HTMLButtonElement>("applyChannelsBtn").addEventListener("click", async () => {
  if (!requireControlLink("Set channels")) return;
  const mask = channelMaskFromUI();
  if (mask === 0) {
    pushLog("Select at least one channel");
    return;
  }
  try {
    await client.setChannelMask(mask);
  } catch (err) {
    pushLog(`Set channels failed: ${(err as Error).message}`);
  }
});

el<HTMLButtonElement>("applyBootBtn").addEventListener("click", async () => {
  if (!requireControlLink("Set boot mode")) return;
  const bootMode = Number(el<HTMLSelectElement>("bootMode").value);
  try {
    await client.setBootMode(bootMode);
  } catch (err) {
    pushLog(`Set boot mode failed: ${(err as Error).message}`);
  }
});

el<HTMLButtonElement>("applyVisibleTimeoutBtn").addEventListener("click", () => {
  const timeoutSec = Number(el<HTMLInputElement>("visibleTimeoutInput").value);
  if (
    !Number.isFinite(timeoutSec) ||
    timeoutSec < MIN_VISIBLE_TIMEOUT_SEC ||
    timeoutSec > MAX_VISIBLE_TIMEOUT_SEC
  ) {
    pushLog(`Visible timeout must be ${MIN_VISIBLE_TIMEOUT_SEC}..${MAX_VISIBLE_TIMEOUT_SEC} seconds`);
    return;
  }
  visibleTimeoutSec = Math.round(timeoutSec);
  el<HTMLInputElement>("visibleTimeoutInput").value = String(visibleTimeoutSec);
  renderTable();
  renderOps();
  pushLog(`Visible timeout set to ${visibleTimeoutSec}s`);
});

el<HTMLButtonElement>("clearBtn").addEventListener("click", () => {
  appState.sightings.clear();
  renderTable();
  renderOps();
  pushLog("Live view cleared");
});

if ("bluetooth" in navigator) {
  client
    .reconnectKnown()
    .then(async (connected) => {
      if (connected) {
        const secure = await waitForEncryptedLink(8000);
        if (secure) {
          await client.requestStatus();
          await client.requestSnapshot();
        } else {
          pushLog("Auto reconnect is up but link is not encrypted yet");
        }
      } else {
        pushLog("No known BLE device to auto-reconnect");
      }
    })
    .catch((err) => {
      pushLog(`Auto reconnect skipped: ${(err as Error).message}`);
    });
} else {
  pushLog("Web Bluetooth unavailable. Use Chromium over HTTPS/localhost.");
}

setInterval(() => {
  renderTable();
  renderOps();
}, 1500);
