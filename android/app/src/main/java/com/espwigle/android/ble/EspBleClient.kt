package com.espwigle.android.ble

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
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import com.espwigle.android.model.GpsFix
import com.espwigle.android.model.WgFrame
import com.espwigle.android.model.WgProtocol
import java.util.UUID
import java.util.concurrent.atomic.AtomicInteger

class EspBleClient(
  context: Context,
  private val listener: Listener,
) {
  interface Listener {
    fun onConnectState(connected: Boolean)

    fun onFrame(frame: WgFrame)

    fun onInfo(message: String)
  }

  companion object {
    private val SERVICE_UUID: UUID = UUID.fromString("6ee2e690-d7fd-4a11-94a2-89528da43130")
    private val CONTROL_UUID: UUID = UUID.fromString("6ee2e690-d7fd-4a11-94a2-89528da43131")
    private val STATUS_UUID: UUID = UUID.fromString("6ee2e690-d7fd-4a11-94a2-89528da43132")
    private val SIGHTING_UUID: UUID = UUID.fromString("6ee2e690-d7fd-4a11-94a2-89528da43133")
    private val CONFIG_UUID: UUID = UUID.fromString("6ee2e690-d7fd-4a11-94a2-89528da43134")
    private val DEVICE_INFO_UUID: UUID = UUID.fromString("6ee2e690-d7fd-4a11-94a2-89528da43135")
    private val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    private const val TARGET_DEVICE_NAME = "ESPWIGLE-C6"
    private const val TARGET_DEVICE_ADDRESS = "A0:85:E3:DB:04:92"
    private const val SCAN_TIMEOUT_MS = 12_000L
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
  private var connecting = false
  private var ready = false
  private val notificationQueue = ArrayDeque<BluetoothGattCharacteristic>()

  fun isConnected(): Boolean = ready && gatt != null

  fun connect() {
    if (connecting || isConnected()) {
      return
    }

    val adapter = bluetoothManager.adapter
    if (adapter == null || !adapter.isEnabled) {
      listener.onInfo("Bluetooth adapter unavailable or disabled")
      return
    }

    val known = findKnownDevice(adapter)
    if (known != null) {
      connectToDevice(known)
      return
    }

    startScan(adapter)
  }

  @SuppressLint("MissingPermission")
  fun disconnect() {
    stopScan()
    connecting = false
    ready = false
    controlChar = null
    statusChar = null
    sightingChar = null
    configChar = null
    deviceInfoChar = null
    notificationQueue.clear()

    val current = gatt
    gatt = null
    current?.disconnect()
    current?.close()
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

    return writeCharacteristicNoResponse(targetGatt, targetControl, frame)
  }

  @SuppressLint("MissingPermission")
  private fun startScan(adapter: BluetoothAdapter) {
    val scanner = adapter.bluetoothLeScanner
    if (scanner == null) {
      listener.onInfo("BLE scanner unavailable")
      return
    }

    connecting = true
    listener.onInfo("Scanning for $TARGET_DEVICE_NAME ($TARGET_DEVICE_ADDRESS)...")

    val settings =
      ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()

    val callback =
      object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
          val device = result.device ?: return
          val matchesAddress = device.address.equals(TARGET_DEVICE_ADDRESS, ignoreCase = true)
          if (!matchesAddress) {
            return
          }
          connectToDevice(device)
        }

        override fun onScanFailed(errorCode: Int) {
          listener.onInfo("BLE scan failed code=$errorCode")
          connecting = false
          stopScan()
        }
      }

    scanCallback = callback
    scanner.startScan(emptyList(), settings, callback)

    mainHandler.postDelayed(
      {
        if (scanCallback != null) {
          listener.onInfo("BLE scan timeout")
          stopScan()
          connecting = false
        }
      },
      SCAN_TIMEOUT_MS,
    )
  }

  @SuppressLint("MissingPermission")
  private fun stopScan() {
    val adapter = bluetoothManager.adapter ?: return
    val scanner = adapter.bluetoothLeScanner ?: return
    val callback = scanCallback ?: return
    scanner.stopScan(callback)
    scanCallback = null
  }

  private fun findKnownDevice(adapter: BluetoothAdapter): BluetoothDevice? {
    val bonded = adapter.bondedDevices ?: return null
    return bonded.firstOrNull { device ->
      device.address.equals(TARGET_DEVICE_ADDRESS, ignoreCase = true)
    }
  }

  @SuppressLint("MissingPermission")
  private fun connectToDevice(device: BluetoothDevice) {
    if (gatt != null || connecting && scanCallback == null) {
      return
    }
    stopScan()
    listener.onInfo("Connecting to ${device.name ?: device.address}...")
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
  private fun writeCharacteristicNoResponse(
    targetGatt: BluetoothGatt,
    characteristic: BluetoothGattCharacteristic,
    bytes: ByteArray,
  ): Boolean {
    characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
    return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      targetGatt.writeCharacteristic(
        characteristic,
        bytes,
        BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE,
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
            gatt.requestMtu(185)
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

      override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
        if (status != BluetoothGatt.GATT_SUCCESS) {
          listener.onInfo("Service discovery failed status=$status")
          disconnect()
          return
        }

        val service: BluetoothGattService? = gatt.getService(SERVICE_UUID)
        if (service == null) {
          listener.onInfo("ESPWIGLE service not found")
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
      listener.onInfo("Frame decode error: ${e.message}")
    }
  }
}
