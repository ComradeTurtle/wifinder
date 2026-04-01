import { applySighting, createState, pruneSightings, summarizeState, toRows } from "./state-core.js";

export type Sighting = {
  bssid: string;
  ssid: string;
  channel: number;
  rssi: number;
  auth: string;
  seenMs: number;
};

export type ConsoleState = ReturnType<typeof createState>;

export { applySighting, createState, pruneSightings, summarizeState, toRows };
