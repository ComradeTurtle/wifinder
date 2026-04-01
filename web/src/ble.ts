import {
  COMMANDS,
  decodeFrame,
  encodeCommandFrame,
} from "./protocol";

const UUIDS = {
  service: "6ee2e690-d7fd-4a11-94a2-89528da43130",
  control: "6ee2e690-d7fd-4a11-94a2-89528da43131",
  status: "6ee2e690-d7fd-4a11-94a2-89528da43132",
  sighting: "6ee2e690-d7fd-4a11-94a2-89528da43133",
  config: "6ee2e690-d7fd-4a11-94a2-89528da43134",
  deviceInfo: "6ee2e690-d7fd-4a11-94a2-89528da43135",
} as const;

export type Frame = {
  version: number;
  type: number;
  seq: number;
  len: number;
  deviceMs: number;
  payload: Uint8Array;
};

export type BleCallbacks = {
  onFrame: (frame: Frame) => void;
  onConnectState: (connected: boolean) => void;
  onError: (message: string) => void;
  onInfo: (message: string) => void;
};

export class EspWigleBleClient {
  private device: BluetoothDevice | null = null;
  private server: BluetoothRemoteGATTServer | null = null;
  private controlChar: BluetoothRemoteGATTCharacteristic | null = null;
  private statusChar: BluetoothRemoteGATTCharacteristic | null = null;
  private sightingChar: BluetoothRemoteGATTCharacteristic | null = null;
  private configChar: BluetoothRemoteGATTCharacteristic | null = null;
  private deviceInfoChar: BluetoothRemoteGATTCharacteristic | null = null;
  private seq = 1;

  constructor(private readonly cb: BleCallbacks) {}

  private async pause(ms: number) {
    await new Promise((resolve) => setTimeout(resolve, ms));
  }

  private onDisconnect = () => {
    if (this.statusChar) {
      this.statusChar.removeEventListener("characteristicvaluechanged", this.onNotify);
    }
    if (this.sightingChar) {
      this.sightingChar.removeEventListener("characteristicvaluechanged", this.onNotify);
    }
    this.server = null;
    this.controlChar = null;
    this.statusChar = null;
    this.sightingChar = null;
    this.configChar = null;
    this.deviceInfoChar = null;
    this.cb.onConnectState(false);
    this.cb.onInfo("Disconnected");
  };

  private onNotify = (event: Event) => {
    const target = event.target as BluetoothRemoteGATTCharacteristic | null;
    if (!target?.value) return;
    try {
      const value = new Uint8Array(target.value.buffer.slice(0));
      const frame = decodeFrame(value) as Frame;
      this.cb.onFrame(frame);
    } catch (err) {
      this.cb.onError(`Frame decode error: ${(err as Error).message}`);
    }
  };

  private async bindCharacteristics(service: BluetoothRemoteGATTService) {
    this.controlChar = await service.getCharacteristic(UUIDS.control);
    this.statusChar = await service.getCharacteristic(UUIDS.status);
    this.sightingChar = await service.getCharacteristic(UUIDS.sighting);
    this.configChar = await service.getCharacteristic(UUIDS.config);
    this.deviceInfoChar = await service.getCharacteristic(UUIDS.deviceInfo);

    await this.statusChar.startNotifications();
    await this.sightingChar.startNotifications();
    this.statusChar.addEventListener("characteristicvaluechanged", this.onNotify);
    this.sightingChar.addEventListener("characteristicvaluechanged", this.onNotify);
  }

  private async establishConnection(device: BluetoothDevice) {
    this.device = device;
    this.device.removeEventListener("gattserverdisconnected", this.onDisconnect);
    this.device.addEventListener("gattserverdisconnected", this.onDisconnect);
    if (!device.gatt) throw new Error("GATT unavailable on selected device");

    let lastErr: Error | null = null;
    for (let attempt = 1; attempt <= 2; attempt += 1) {
      try {
        if (device.gatt.connected) {
          device.gatt.disconnect();
          await this.pause(150);
        }
        this.server = await device.gatt.connect();
        const service = await this.server.getPrimaryService(UUIDS.service);
        await this.bindCharacteristics(service);
        this.cb.onConnectState(true);

        if (this.deviceInfoChar) {
          const value = await this.deviceInfoChar.readValue();
          const text = new TextDecoder().decode(value.buffer);
          this.cb.onInfo(`Connected to ${device.name ?? "ESP"} (${text.trim()})`);
        }
        return;
      } catch (err) {
        lastErr = err as Error;
        this.cb.onInfo(`BLE connect attempt ${attempt} failed: ${lastErr.message}`);
        try {
          device.gatt.disconnect();
        } catch {
          // Best effort cleanup only.
        }
        this.onDisconnect();
        await this.pause(250);
      }
    }

    throw lastErr ?? new Error("Failed to establish BLE connection");
  }

  async reconnectKnown(): Promise<boolean> {
    if (!("bluetooth" in navigator) || !navigator.bluetooth.getDevices) return false;
    const devices = await navigator.bluetooth.getDevices();
    const match = devices.find(
      (d) => d.name?.includes("ESPWIGLE") || d.name?.includes("WIGLE")
    );
    if (!match) return false;
    await this.establishConnection(match);
    return true;
  }

  async requestAndConnect() {
    const device = await navigator.bluetooth.requestDevice({
      // Linux/BlueZ can be unreliable with strict 128-bit service filters during scan.
      // Name-prefix matching keeps the chooser targeted while remaining more compatible.
      filters: [{ namePrefix: "ESPWIGLE" }, { namePrefix: "ESP" }, { services: [UUIDS.service] }],
      optionalServices: [UUIDS.service],
    });
    await this.establishConnection(device);
  }

  async disconnect() {
    if (this.device?.gatt?.connected) {
      this.device.gatt.disconnect();
    }
  }

  isConnected() {
    return !!this.server?.connected;
  }

  private async writeCommand(commandId: number, payload: Uint8Array = new Uint8Array(0)) {
    if (!this.server?.connected || !this.controlChar) {
      throw new Error("Not connected");
    }
    const frame = encodeCommandFrame({
      seq: this.seq++,
      deviceMs: Date.now() >>> 0,
      commandId,
      payload,
    });
    await this.controlChar.writeValueWithoutResponse(frame);
  }

  async sendStart() {
    await this.writeCommand(COMMANDS.START);
  }

  async sendStop() {
    await this.writeCommand(COMMANDS.STOP);
  }

  async requestStatus() {
    await this.writeCommand(COMMANDS.REQUEST_STATUS);
  }

  async requestSnapshot() {
    await this.writeCommand(COMMANDS.REQUEST_SNAPSHOT);
  }

  async setHopMs(hopMs: number) {
    const payload = new Uint8Array([hopMs & 0xff, (hopMs >> 8) & 0xff]);
    await this.writeCommand(COMMANDS.SET_HOP_MS, payload);
  }

  async setChannelMask(mask: number) {
    const payload = new Uint8Array([mask & 0xff, (mask >> 8) & 0xff]);
    await this.writeCommand(COMMANDS.SET_CHANNEL_MASK, payload);
  }

  async setBootMode(mode: number) {
    await this.writeCommand(COMMANDS.SET_BOOT_MODE, new Uint8Array([mode]));
  }

  async readConfigFrame() {
    if (!this.configChar) throw new Error("Not connected");
    const value = await this.configChar.readValue();
    return decodeFrame(new Uint8Array(value.buffer.slice(0))) as Frame;
  }
}

export const BLE_UUIDS = UUIDS;
