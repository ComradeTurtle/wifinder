package com.wifinder.android.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import com.wifinder.android.model.GpsFix
import com.wifinder.android.model.WgFrame
import com.wifinder.android.model.WgProtocol
import java.util.UUID
import java.util.concurrent.atomic.AtomicInteger

class WiFinderBleClient(
  context: Context,
  private val listener: Listener,
) {
  interface Listener {
    fun onConnectState(connected: Boolean)

    fun onFrame(frame: WgFrame)

    fun onInfo(message: String)

    fun onBleDeviceCandidates(candidates: List<BleDeviceCandidate>) {}

    fun onBleDeviceResolved(address: String, name: String?) {}
  }

  companion object {
    private val SERVICE_UUID: UUID = UUID.fromString("6ee2e690-d7fd-4a11-94a2-89528da43130")
    private val CONTROL_UUID: UUID = UUID.fromString("6ee2e690-d7fd-4a11-94a2-89528da43131")
    private val STATUS_UUID: UUID = UUID.fromString("6ee2e690-d7fd-4a11-94a2-89528da43132")
    private val SIGHTING_UUID: UUID = UUID.fromString("6ee2e690-d7fd-4a11-94a2-89528da43133")
    private val CONFIG_UUID: UUID = UUID.fromString("6ee2e690-d7fd-4a11-94a2-89528da43134")
    private val DEVICE_INFO_UUID: UUID = UUID.fromString("6ee2e690-d7fd-4a11-94a2-89528da43135")
    private val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    private const val TARGET_NAME_HINT = "WIFINDER"
    private const val SCAN_TIMEOUT_MS = 12_000L
    private const val SCAN_SETTLE_MS = 1_200L
    private const val TARGET_MTU = 517
  }

  private val appContext = context.applicationContext
  private val bluetoothManager =
    appContext.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
  private val mainHandler = Handler(Looper.getMainLooper())
  private val seq = AtomicInteger(1)

  private var gatt: BluetoothGatt? = null
  private var controlChar: BluetoothGattCharacteristic? = null
  private var statusChar: BluetoothGattCharacteristic? = null
  private var sightingChar: BluetoothGattCharacteristic? = null
  private var configChar: BluetoothGattCharacteristic? = null
  private var deviceInfoChar: BluetoothGattCharacteristic? = null
  private var scanCallback: ScanCallback? = null
  private var scanTimeoutRunnable: Runnable? = null
  private var scanSettleRunnable: Runnable? = null
  private var connecting = false
  private var ready = false
  private var preferredAddress: String? = null
  private var connectedAddress: String? = null
  private var connectedName: String? = null
  private val discoveredCandidates = linkedMapOf<String, BleDeviceCandidate>()
  private val discoveredDevices = linkedMapOf<String, BluetoothDevice>()
  private val notificationQueue = ArrayDeque<BluetoothGattCharacteristic>()

  fun isConnected(): Boolean = ready && gatt != null

  fun setPreferredDeviceAddress(address: String?) {
    preferredAddress = BleTargetingPolicy.normalizeAddress(address)
  }

  fun connect() {
    if (connecting || isConnected()) {
      return
    }

    val adapter = bluetoothManager.adapter
    if (adapter == null || !adapter.isEnabled) {
      listener.onInfo("Bluetooth adapter unavailable or disabled")
      return
    }

    val remembered = preferredAddress
    val known = remembered?.let { findBondedDeviceByAddress(adapter, it) }
    if (known != null) {
      connectToDevice(known)
      return
    }

    startScan(adapter, remembered)
  }

  fun connectToAddress(address: String): Boolean {
    if (connecting || isConnected()) {
      return false
    }
    val normalized = BleTargetingPolicy.normalizeAddress(address) ?: return false
    val adapter = bluetoothManager.adapter
    if (adapter == null || !adapter.isEnabled) {
      listener.onInfo("Bluetooth adapter unavailable or disabled")
      return false
    }
    val device = discoveredDevices[normalized] ?: runCatching {
      adapter.getRemoteDevice(normalized)
    }.getOrNull()
    if (device == null) {
      listener.onInfo("Selected device unavailable: $normalized")
      return false
    }
    connectToDevice(device)
    return true
  }

  @SuppressLint("MissingPermission")
  fun disconnect() {
    stopScan()
    clearScanScheduling()
    connecting = false
    ready = false
    connectedAddress = null
    connectedName = null
    controlChar = null
    statusChar = null
    sightingChar = null
    configChar = null
    deviceInfoChar = null
    notificationQueue.clear()
    discoveredCandidates.clear()
    discoveredDevices.clear()

    val current = gatt
    gatt = null
    current?.disconnect()
    current?.close()
    listener.onBleDeviceCandidates(emptyList())
    listener.onConnectState(false)
  }

  fun sendStart(): Boolean = sendCommand(WgProtocol.Command.START)

  fun sendStop(): Boolean = sendCommand(WgProtocol.Command.STOP)

  fun requestStatus(): Boolean = sendCommand(WgProtocol.Command.REQUEST_STATUS)

  fun requestSnapshot(): Boolean = sendCommand(WgProtocol.Command.REQUEST_SNAPSHOT)

  fun setHopMs(hopMs: Int): Boolean {
    val payload = byteArrayOf((hopMs and 0xFF).toByte(), ((hopMs ushr 8) and 0xFF).toByte())
    return sendCommand(WgProtocol.Command.SET_HOP_MS, payload)
  }

  fun setChannelMask(mask: Int): Boolean {
    val payload = byteArrayOf((mask and 0xFF).toByte(), ((mask ushr 8) and 0xFF).toByte())
    return sendCommand(WgProtocol.Command.SET_CHANNEL_MASK, payload)
  }

  fun setChannelPlan(localMask: Int, nodeMask24: Int, nodeMask5Ghz: Long): Boolean =
    sendCommand(
      WgProtocol.Command.SET_CHANNEL_PLAN,
      WgProtocol.encodeChannelPlanPayload(localMask, nodeMask24, nodeMask5Ghz),
    )

  fun setBootMode(mode: Int): Boolean =
    sendCommand(WgProtocol.Command.SET_BOOT_MODE, byteArrayOf((mode and 0xFF).toByte()))

  fun sendGpsFix(fix: GpsFix): Boolean =
    sendCommand(WgProtocol.Command.SET_GPS_FIX, WgProtocol.encodeGpsFixPayload(fix))

  fun sendReplayAck(sessionId: Long, highestSeq: Long): Boolean =
    sendCommand(
      WgProtocol.Command.REPLAY_ACK,
      WgProtocol.encodeReplayAckPayload(sessionId, highestSeq),
    )

  fun setReplayEnabled(enabled: Boolean): Boolean =
    sendCommand(
      WgProtocol.Command.SET_REPLAY,
      WgProtocol.encodeReplayTogglePayload(enabled),
    )

  fun setBacklogBlobEnabled(enabled: Boolean): Boolean =
    sendCommand(
      WgProtocol.Command.SET_BACKLOG_BLOB,
      WgProtocol.encodeBacklogBlobTogglePayload(enabled),
    )

  fun sendBacklogBlobChunkReply(
    sessionId: Long,
    chunkOffset: Long,
    chunkLen: Int,
    accepted: Boolean,
  ): Boolean =
    sendCommand(
      WgProtocol.Command.BACKLOG_BLOB_CHUNK_REPLY,
      WgProtocol.encodeBacklogBlobChunkReplyPayload(sessionId, chunkOffset, chunkLen, accepted),
    )

  fun setWifiDumpEnabled(enabled: Boolean): Boolean =
    sendCommand(
      WgProtocol.Command.SET_WIFI_DUMP,
      WgProtocol.encodeWifiDumpTogglePayload(enabled),
    )

  fun commitWifiDump(runId: Long): Boolean =
    sendCommand(
      WgProtocol.Command.COMMIT_WIFI_DUMP,
      WgProtocol.encodeWifiDumpCommitPayload(runId),
    )

  fun seedDebugStorage(targetBytes: Int): Boolean =
    sendCommand(
      WgProtocol.Command.DEBUG_SEED_STORAGE,
      WgProtocol.encodeDebugSeedStoragePayload(targetBytes),
    )

  fun clearStorageSessions(): Boolean = sendCommand(WgProtocol.Command.CLEAR_STORAGE)

  fun setGpsNavRateMode(mode: Int): Boolean =
    sendCommand(
      WgProtocol.Command.SET_GPS_NAV_RATE,
      byteArrayOf((mode and 0xFF).toByte()),
    )

  fun sendGpsClear(): Boolean =
    sendCommand(
      WgProtocol.Command.SET_GPS_FIX,
      WgProtocol.encodeGpsFixPayload(
        GpsFix(
          flags = 0,
          latE7 = 0,
          lonE7 = 0,
          altMm = 0,
          speedMmps = 0,
          bearingMdeg = 0,
          unixTimeS = 0,
          accuracyCm = 0,
        ),
      ),
    )

  private fun sendCommand(commandId: Int, payload: ByteArray = byteArrayOf()): Boolean {
    return sendCommand(commandId, payload, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE)
  }

  private fun sendCommand(
    commandId: Int,
    payload: ByteArray,
    writeType: Int,
  ): Boolean {
    if (!ready) {
      listener.onInfo("Command blocked: BLE link not ready")
      return false
    }

    val targetGatt = gatt ?: return false
    val targetControl = controlChar ?: return false
    val frame =
      WgProtocol.encodeCommandFrame(
        seq = seq.getAndIncrement(),
        deviceMs = System.currentTimeMillis(),
        commandId = commandId,
        payload = payload,
      )

    return writeCharacteristic(targetGatt, targetControl, frame, writeType)
  }

  @SuppressLint("MissingPermission")
  private fun startScan(adapter: BluetoothAdapter, rememberedAddress: String?) {
    val scanner = adapter.bluetoothLeScanner
    if (scanner == null) {
      listener.onInfo("BLE scanner unavailable")
      return
    }

    stopScan()
    clearScanScheduling()
    discoveredCandidates.clear()
    discoveredDevices.clear()
    connecting = true

    listener.onInfo(
      if (rememberedAddress != null) {
        "Scanning for $TARGET_NAME_HINT service (preferring $rememberedAddress)..."
      } else {
        "Scanning for $TARGET_NAME_HINT service..."
      },
    )

    val settings =
      ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
    val filters = listOf(
      ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE_UUID)).build(),
    )

    val callback =
      object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
          val device = result.device ?: return
          val normalizedAddress = BleTargetingPolicy.normalizeAddress(device.address) ?: return
          val scanRecord = result.scanRecord
          val hasServiceUuid = scanRecord?.serviceUuids?.any { it.uuid == SERVICE_UUID } ?: true
          val advertisedName = scanRecord?.deviceName ?: device.name
          if (!BleTargetingPolicy.isEligibleCandidate(hasServiceUuid, advertisedName)) {
            return
          }

          val previous = discoveredCandidates[normalizedAddress]
          val resolvedName = when {
            !advertisedName.isNullOrBlank() -> advertisedName
            previous != null && previous.name.isNotBlank() -> previous.name
            else -> normalizedAddress
          }
          val strongestRssi = maxOf(result.rssi, previous?.rssi ?: Int.MIN_VALUE)
          discoveredDevices[normalizedAddress] = device
          discoveredCandidates[normalizedAddress] =
            BleDeviceCandidate(
              address = normalizedAddress,
              name = resolvedName,
              rssi = strongestRssi,
            )

          if (rememberedAddress != null && normalizedAddress == rememberedAddress) {
            connectToDevice(device)
            return
          }

          scheduleScanSettle()
        }

        override fun onScanFailed(errorCode: Int) {
          listener.onInfo("BLE scan failed code=$errorCode")
          connecting = false
          stopScan()
        }
      }

    scanCallback = callback
    scanner.startScan(filters, settings, callback)

    val timeout = Runnable {
      finalizeScanDecision()
    }
    scanTimeoutRunnable = timeout
    mainHandler.postDelayed(timeout, SCAN_TIMEOUT_MS)
  }

  private fun scheduleScanSettle() {
    val existing = scanSettleRunnable
    if (existing != null) {
      mainHandler.removeCallbacks(existing)
    }
    val settle = Runnable {
      finalizeScanDecision()
    }
    scanSettleRunnable = settle
    mainHandler.postDelayed(settle, SCAN_SETTLE_MS)
  }

  @SuppressLint("MissingPermission")
  private fun finalizeScanDecision() {
    if (scanCallback == null) {
      return
    }
    stopScan()
    connecting = false

    val candidates = BleTargetingPolicy.sortCandidates(
      candidates = discoveredCandidates.values,
      preferredAddress = preferredAddress,
    )
    if (candidates.isEmpty()) {
      listener.onInfo("BLE scan timeout")
      return
    }

    val auto = BleTargetingPolicy.chooseAutoConnect(candidates, preferredAddress)
    if (auto != null) {
      val device = discoveredDevices[auto.address]
      if (device != null) {
        connectToDevice(device)
        return
      }
    }

    listener.onInfo(
      if (preferredAddress != null) {
        "Remembered device unavailable; select a WiFinder device"
      } else {
        "Multiple WiFinder devices found; select one"
      },
    )
    listener.onBleDeviceCandidates(candidates)
  }

  @SuppressLint("MissingPermission")
  private fun stopScan() {
    val adapter = bluetoothManager.adapter ?: return
    val scanner = adapter.bluetoothLeScanner ?: return
    val callback = scanCallback ?: return
    scanner.stopScan(callback)
    scanCallback = null
    clearScanScheduling()
  }

  private fun clearScanScheduling() {
    scanTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
    scanSettleRunnable?.let { mainHandler.removeCallbacks(it) }
    scanTimeoutRunnable = null
    scanSettleRunnable = null
  }

  private fun findBondedDeviceByAddress(adapter: BluetoothAdapter, address: String): BluetoothDevice? {
    val bonded = adapter.bondedDevices ?: return null
    return bonded.firstOrNull { device ->
      BleTargetingPolicy.normalizeAddress(device.address) == address &&
        BleTargetingPolicy.isNameCompatible(device.name)
    }
  }

  @SuppressLint("MissingPermission")
  private fun connectToDevice(device: BluetoothDevice) {
    if (gatt != null || isConnected()) {
      return
    }
    stopScan()
    clearScanScheduling()
    val normalizedAddress = BleTargetingPolicy.normalizeAddress(device.address) ?: device.address
    connectedAddress = normalizedAddress
    connectedName = device.name
    listener.onBleDeviceCandidates(emptyList())
    listener.onInfo("Connecting to ${device.name ?: normalizedAddress}...")
    connecting = true

    gatt?.close()
    gatt = null

    gatt =
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
        device.connectGatt(appContext, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
      } else {
        device.connectGatt(appContext, false, gattCallback)
      }
  }

  @SuppressLint("MissingPermission")
  private fun writeCharacteristic(
    targetGatt: BluetoothGatt,
    characteristic: BluetoothGattCharacteristic,
    bytes: ByteArray,
    writeType: Int,
  ): Boolean {
    characteristic.writeType = writeType
    return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      targetGatt.writeCharacteristic(
        characteristic,
        bytes,
        writeType,
      ) == BluetoothStatusCodes.SUCCESS
    } else {
      characteristic.value = bytes
      targetGatt.writeCharacteristic(characteristic)
    }
  }

  @SuppressLint("MissingPermission")
  private fun enableNextNotification() {
    val targetGatt = gatt ?: return
    val characteristic = notificationQueue.removeFirstOrNull()
    if (characteristic == null) {
      ready = true
      connecting = false
      listener.onConnectState(true)
      val address = connectedAddress ?: BleTargetingPolicy.normalizeAddress(targetGatt.device.address)
      if (address != null) {
        listener.onBleDeviceResolved(address, connectedName ?: targetGatt.device.name)
      }
      listener.onInfo("BLE ready")
      return
    }

    targetGatt.setCharacteristicNotification(characteristic, true)
    val cccd = characteristic.getDescriptor(CCCD_UUID)
    if (cccd == null) {
      enableNextNotification()
      return
    }

    val enableValue = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      targetGatt.writeDescriptor(cccd, enableValue)
    } else {
      cccd.value = enableValue
      targetGatt.writeDescriptor(cccd)
    }
  }

  private val gattCallback =
    object : BluetoothGattCallback() {
      @SuppressLint("MissingPermission")
      override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
        if (status != BluetoothGatt.GATT_SUCCESS) {
          listener.onInfo("BLE connection error status=$status")
          disconnect()
          return
        }

        if (newState == BluetoothGatt.STATE_CONNECTED) {
          listener.onInfo("Connected, discovering services...")
          if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            if (!gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)) {
              listener.onInfo("BLE connection priority request failed")
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
              gatt.setPreferredPhy(
                BluetoothDevice.PHY_LE_2M_MASK,
                BluetoothDevice.PHY_LE_2M_MASK,
                BluetoothDevice.PHY_OPTION_NO_PREFERRED,
              )
              listener.onInfo("Requested LE 2M PHY")
            }
            gatt.requestMtu(TARGET_MTU)
          }
          gatt.discoverServices()
        } else if (newState == BluetoothGatt.STATE_DISCONNECTED) {
          listener.onInfo("Disconnected")
          disconnect()
        }
      }

      override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
        if (status == BluetoothGatt.GATT_SUCCESS) {
          listener.onInfo("MTU=$mtu")
        }
      }

      override fun onPhyUpdate(gatt: BluetoothGatt, txPhy: Int, rxPhy: Int, status: Int) {
        listener.onInfo("PHY update tx=${phyName(txPhy)} rx=${phyName(rxPhy)} status=$status")
      }

      override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
        if (status != BluetoothGatt.GATT_SUCCESS) {
          listener.onInfo("Service discovery failed status=$status")
          disconnect()
          return
        }

        val service: BluetoothGattService? = gatt.getService(SERVICE_UUID)
        if (service == null) {
          listener.onInfo("WIFINDER service not found")
          disconnect()
          return
        }

        controlChar = service.getCharacteristic(CONTROL_UUID)
        statusChar = service.getCharacteristic(STATUS_UUID)
        sightingChar = service.getCharacteristic(SIGHTING_UUID)
        configChar = service.getCharacteristic(CONFIG_UUID)
        deviceInfoChar = service.getCharacteristic(DEVICE_INFO_UUID)

        if (controlChar == null || statusChar == null || sightingChar == null) {
          listener.onInfo("Required characteristics missing")
          disconnect()
          return
        }

        notificationQueue.clear()
        notificationQueue.add(statusChar!!)
        notificationQueue.add(sightingChar!!)
        enableNextNotification()
      }

      override fun onDescriptorWrite(
        gatt: BluetoothGatt,
        descriptor: BluetoothGattDescriptor,
        status: Int,
      ) {
        if (status != BluetoothGatt.GATT_SUCCESS) {
          listener.onInfo("Descriptor write failed status=$status")
          disconnect()
          return
        }
        enableNextNotification()
      }

      override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
        val value = characteristic.value ?: return
        handleIncomingValue(value)
      }

      override fun onCharacteristicChanged(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
        value: ByteArray,
      ) {
        handleIncomingValue(value)
      }
    }

  private fun handleIncomingValue(value: ByteArray) {
    try {
      val frame = WgProtocol.decodeFrame(value)
      listener.onFrame(frame)
    } catch (e: Exception) {
      listener.onInfo("Frame decode error (${value.size} bytes): ${e.message}")
    }
  }

  private fun phyName(phy: Int): String =
    when (phy) {
      BluetoothDevice.PHY_LE_1M -> "1M"
      BluetoothDevice.PHY_LE_2M -> "2M"
      BluetoothDevice.PHY_LE_CODED -> "CODED"
      else -> "unknown($phy)"
    }
}
