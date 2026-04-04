package com.espwigle.android.service

import android.annotation.SuppressLint
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.ServiceInfo
import android.location.Location
import android.os.Binder
import android.os.Build
import android.os.IBinder
import com.espwigle.android.ble.EspBleClient
import com.espwigle.android.gps.GpsTracker
import com.espwigle.android.logging.WigleCsvLogger
import com.espwigle.android.model.EspGpsPayload
import com.espwigle.android.model.GpsFix
import com.espwigle.android.model.BacklogBlobChunkPayload
import com.espwigle.android.model.BacklogBlobDonePayload
import com.espwigle.android.model.BacklogBlobMetaPayload
import com.espwigle.android.model.SightingPayload
import com.espwigle.android.model.StatusPayload
import com.espwigle.android.model.WgFrame
import com.espwigle.android.model.WgProtocol
import com.espwigle.android.model.WigleWifiRow
import com.espwigle.android.ui.SightingUi
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TreeSet
import java.util.zip.CRC32
import kotlin.math.abs
import kotlin.math.roundToInt
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

/**
 * Shared state exposed by the service to the UI layer via binding.
 */
data class ServiceState(
  val connected: Boolean = false,
  val scanning: Boolean = false,
  val bleEncrypted: Boolean = false,
  val currentChannel: Int = 1,
  val hopMs: Int = 250,
  val channelMask: Int = 0x1FFF,
  val uniqueBssids: Int = 0,
  val packetsPerSec: Int = 0,
  val droppedNotifies: Int = 0,
  val bootMode: Int = 0,
  val gpsValid: Boolean = false,
  val gpsSource: Int = 0,
  val gpsAgeS: Int = 0,
  val gpsAccuracyDm: Int = 0,
  val gpsSatCount: Int = 0,
  val gpsHdopCenti: Int = 0,
  val gpsPdopCenti: Int = 0,
  val nodeLinkUp: Boolean = false,
  val nodeLastSeenS: Int = 0,
  val nodePacketsPerSec: Int = 0,
  val nodeForwardedSightings: Int = 0,
  val nodeChannel: Int = 0,
  val nodeChannelMask: Int = 0,
  val sessionOpen: Boolean = false,
  val sessionId: Long = 0L,
  val queuedRecords: Long = 0L,
  val queuedBytes: Long = 0L,
  val replayActive: Boolean = false,
  val replayCursor: Long = 0L,
  val queueFull: Boolean = false,
  val droppedFlashFull: Long = 0L,
  val nodeCount: Int = 0,
  val phoneGpsAgeS: Int = -1,
  val phoneGpsAccuracyM: Float = 0f,
  val phoneSpeedKmh: Float = 0f,
  val visibleTimeoutSec: Int = 25,
  val autoHopEnabled: Boolean = false,
  val autoHopBaseMs: Int = 250,
  val autoHopAppliedMs: Int = 250,
  val gpsNavMode: Int = 0,
  val gpsNavAppliedHz: Int = 0,
  val spiffsTotalBytes: Long = 0L,
  val spiffsUsedBytes: Long = 0L,
  val spiffsFreeBytes: Long = 0L,
  val blobActive: Boolean = false,
  val blobSessionId: Long = 0L,
  val blobBytesSent: Long = 0L,
  val blobBytesTotal: Long = 0L,
  val phoneGpsPushActive: Boolean = false,
  val downloadBacklogActive: Boolean = false,
  val loggingEnabled: Boolean = false,
  val csvPath: String = "",
  val sightings: List<SightingUi> = emptyList(),
  val logs: List<String> = emptyList(),
)

class ScannerService : Service(), EspBleClient.Listener {

  companion object {
    const val ACTION_STOP = "com.espwigle.android.STOP_SERVICE"
    private const val MIN_HOP_MS = 50
    private const val MAX_HOP_MS = 2000
    private const val AUTO_HOP_MIN_MS = 70
    private const val AUTO_HOP_SPEED_CAP_MMPS = 33_333  // ~120 km/h
    private const val AUTO_HOP_MAX_REDUCTION_PCT = 60
    private const val AUTO_HOP_MIN_APPLY_INTERVAL_MS = 1200L
    private const val AUTO_HOP_MIN_DELTA_MS = 10
    private const val AUTO_HOP_LOG_INTERVAL_MS = 10_000L
    private const val MAX_LOG_LINES = 300
    private const val MIN_LOG_INTERVAL_PER_BSSID_MS = 2000L
    private const val MAX_ESP_GPS_AGE_MS_FOR_LOGGING = 5_000L
    private const val MAX_PHONE_GPS_AGE_MS_FOR_LOGGING = 3_000L
    private const val NO_GPS_LOG_THROTTLE_MS = 10_000L
    private const val REPLAY_ACK_BATCH_SIZE = 25L
    private const val REPLAY_ACK_MIN_INTERVAL_MS = 2_000L
    private const val PREFS_NAME = "espwigle_settings"
    private const val PREF_VISIBLE_TIMEOUT_SEC = "visible_timeout_sec"
    private const val PREF_AUTO_HOP_ENABLED = "auto_hop_enabled"
    private const val PREF_AUTO_HOP_BASE_MS = "auto_hop_base_ms"
    private const val PREF_GPS_NAV_MODE = "gps_nav_mode"
    private const val BLOB_BLOCK_SIZE = 1024
    private const val BLOB_BLOCK_MAGIC = 0x314B4257L
    private const val BLOB_BLOCK_VERSION = 1
    private const val BLOB_BLOCK_HEADER_SIZE = 28
    private const val BLOB_RECORD_HEADER_SIZE = 2
    private const val DEBUG_SEED_TARGET_BYTES = 768 * 1024
    private const val DEBUG_SEED_MIN_BYTES = 512 * 1024
    private const val DEBUG_SEED_MAX_BYTES = 1024 * 1024

    internal fun normalizeGpsNavMode(mode: Int): Int = when (mode) {
      0, 1, 2, 4 -> mode
      else -> 0
    }

    internal fun gpsNavModeLabel(mode: Int): String = when (normalizeGpsNavMode(mode)) {
      1 -> "Force 1 Hz"
      2 -> "Force 2 Hz"
      4 -> "Force 4 Hz"
      else -> "Auto"
    }

    internal fun computeAutoHopMs(baseHopMs: Int, speedMmps: Int): Int {
      val base = baseHopMs.coerceIn(MIN_HOP_MS, MAX_HOP_MS)
      val boundedSpeed = speedMmps.coerceIn(0, AUTO_HOP_SPEED_CAP_MMPS)
      val reductionPct = ((boundedSpeed * AUTO_HOP_MAX_REDUCTION_PCT) / AUTO_HOP_SPEED_CAP_MMPS)
        .coerceIn(0, AUTO_HOP_MAX_REDUCTION_PCT)
      val scaled = ((base * (100 - reductionPct) + 50) / 100).coerceAtMost(base)
      val dynamicMin = minOf(base, AUTO_HOP_MIN_MS)
      return scaled.coerceIn(dynamicMin, base)
    }

    internal fun speedMmpsToKmh(speedMmps: Int): Float = speedMmps.coerceAtLeast(0) * 0.0036f
  }

  private data class SessionAckTracker(
    var highestContiguous: Long = 0L,
    var lastSent: Long = 0L,
    var lastSentMs: Long = 0L,
    val gaps: TreeSet<Long> = TreeSet(),
  )

  private data class BacklogBlobReceiver(
    val sessionId: Long,
    val totalBytes: Long,
    val writtenSeq: Long,
    val file: File,
    val out: BufferedOutputStream,
    var receivedBytes: Long = 0L,
  )

  private data class BacklogImportResult(
    val records: Int,
    val highestSeq: Long,
    val badBlocks: Int,
  )

  inner class LocalBinder : Binder() {
    val service: ScannerService get() = this@ScannerService
  }

  private val binder = LocalBinder()
  private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

  private val _state = MutableStateFlow(ServiceState())
  val state: StateFlow<ServiceState> = _state.asStateFlow()

  private val sightings = linkedMapOf<String, SightingUi>()
  private val ssidMemory = hashMapOf<String, String>()
  private val lastCsvWriteByBssid = hashMapOf<String, Long>()
  private val sightingsMutex = Mutex()
  private val ackLock = Any()
  private val sessionAcks = hashMapOf<Long, SessionAckTracker>()

  private val logTime = SimpleDateFormat("HH:mm:ss", Locale.US)

  private lateinit var bleClient: EspBleClient
  private lateinit var csvLogger: WigleCsvLogger
  private lateinit var gpsTracker: GpsTracker
  private lateinit var prefs: SharedPreferences

  @Volatile private var lastPhoneLocation: Location? = null
  @Volatile private var lastPhoneFixMs: Long = -1L
  @Volatile private var lastPhoneSpeedMmps: Int = 0
  @Volatile private var lastEspGps: EspGpsPayload? = null
  @Volatile private var lastEspFixMs: Long = -1L
  @Volatile private var lastNoGpsLogMs: Long = 0L
  @Volatile private var lastAutoHopApplyMs: Long = 0L
  @Volatile private var lastAutoHopLogMs: Long = 0L
  @Volatile private var pendingManualHopApply: Boolean = false
  @Volatile private var pendingGpsNavModeApply: Boolean = false
  @Volatile private var gpsNavMode: Int = 0
  @Volatile private var phoneGpsPushActive: Boolean = false
  @Volatile private var downloadBacklogAutoStop: Boolean = false
  @Volatile private var visibleTimeoutSec: Int = 25
  @Volatile private var backlogBlobRx: BacklogBlobReceiver? = null

  override fun onCreate() {
    super.onCreate()
    NotificationHelper.createChannel(this)
    prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
    bleClient = EspBleClient(applicationContext, this)
    csvLogger = WigleCsvLogger(applicationContext)
    gpsTracker = GpsTracker(
      context = applicationContext,
      onLocation = ::onPhoneLocation,
      onError = ::appendLog,
    )
    loadLocalSettings()

    // Periodic sighting pruning + age refresh + notification update
    scope.launch {
      while (isActive) {
        val snapshot = _state.value
        val idle = !snapshot.connected && snapshot.sightings.isEmpty() && !snapshot.downloadBacklogActive
        delay(if (idle) 3000L else 1000L)
        pruneSightingsAndRefreshAges()
      }
    }

    // Notification updater
    scope.launch {
      while (isActive) {
        delay(2000L)
        updateNotification()
      }
    }
  }

  @SuppressLint("ForegroundServiceType")
  override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
    if (intent?.action == ACTION_STOP) {
      disconnectAndStop()
      return START_NOT_STICKY
    }

    val notification = NotificationHelper.build(
      context = this,
      connected = false,
      scanning = false,
      apCount = 0,
      csvActive = false,
    )
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
      startForeground(
        NotificationHelper.NOTIFICATION_ID,
        notification,
        ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE or
          ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION,
      )
    } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
      startForeground(
        NotificationHelper.NOTIFICATION_ID,
        notification,
        ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE or
          ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION,
      )
    } else {
      startForeground(NotificationHelper.NOTIFICATION_ID, notification)
    }

    return START_STICKY
  }

  override fun onBind(intent: Intent?): IBinder = binder

  override fun onDestroy() {
    super.onDestroy()
    scope.cancel()
    try { bleClient.sendGpsClear() } catch (_: Exception) {}
    resetBacklogBlobReceiver(discardFile = true)
    bleClient.disconnect()
    gpsTracker.stop()
    csvLogger.stopSession()
  }

  private fun loadLocalSettings() {
    visibleTimeoutSec = prefs.getInt(PREF_VISIBLE_TIMEOUT_SEC, 25).coerceIn(5, 300)
    val autoHopBase = prefs.getInt(PREF_AUTO_HOP_BASE_MS, 250).coerceIn(MIN_HOP_MS, MAX_HOP_MS)
    val autoHopEnabled = prefs.getBoolean(PREF_AUTO_HOP_ENABLED, false)
    gpsNavMode = normalizeGpsNavMode(prefs.getInt(PREF_GPS_NAV_MODE, 0))
    _state.update {
      it.copy(
        visibleTimeoutSec = visibleTimeoutSec,
        autoHopEnabled = autoHopEnabled,
        autoHopBaseMs = autoHopBase,
        autoHopAppliedMs = autoHopBase,
        gpsNavMode = gpsNavMode,
      )
    }
  }

  private fun persistVisibleTimeoutSetting(timeoutSec: Int) {
    prefs.edit().putInt(PREF_VISIBLE_TIMEOUT_SEC, timeoutSec).apply()
  }

  private fun persistAutoHopSettings(enabled: Boolean, baseHopMs: Int) {
    prefs.edit()
      .putBoolean(PREF_AUTO_HOP_ENABLED, enabled)
      .putInt(PREF_AUTO_HOP_BASE_MS, baseHopMs)
      .apply()
  }

  private fun persistGpsNavModeSetting(mode: Int) {
    prefs.edit().putInt(PREF_GPS_NAV_MODE, normalizeGpsNavMode(mode)).apply()
  }

  // ── Public API (called by ViewModel via binder) ─────────────

  fun doConnect() {
    bleClient.connect()
  }

  fun doDisconnect() {
    phoneGpsPushActive = false
    if (controlLinkReadySilent()) {
      bleClient.setBacklogBlobEnabled(false)
    }
    val shouldAutoStopCsv = downloadBacklogAutoStop
    downloadBacklogAutoStop = false
    bleClient.sendGpsClear()
    resetBacklogBlobReceiver(discardFile = true)
    bleClient.disconnect()
    gpsTracker.stop()
    synchronized(ackLock) {
      sessionAcks.clear()
    }
    if (shouldAutoStopCsv && csvLogger.isActive) {
      stopLogging()
    }
    _state.update { it.copy(phoneGpsPushActive = false, downloadBacklogActive = false) }
  }

  fun disconnectAndStop() {
    doDisconnect()
    csvLogger.stopSession()
    _state.update { it.copy(loggingEnabled = false) }
    stopForeground(STOP_FOREGROUND_REMOVE)
    stopSelf()
  }

  fun sendStart(): Boolean {
    if (!controlLinkReady("Start")) return false
    val ok = bleClient.sendStart()
    if (ok) appendLog("Start command sent")
    return ok
  }

  fun sendStop(): Boolean {
    if (!controlLinkReady("Stop")) return false
    val ok = bleClient.sendStop()
    if (ok) appendLog("Stop command sent")
    return ok
  }

  fun requestStatus(): Boolean {
    if (!controlLinkReady("Request status")) return false
    val ok = bleClient.requestStatus()
    if (ok) appendLog("Status requested")
    return ok
  }

  fun requestSnapshot(): Boolean {
    if (!controlLinkReady("Request snapshot")) return false
    val ok = bleClient.requestSnapshot()
    if (ok) appendLog("Snapshot requested")
    return ok
  }

  fun clearSightings() {
    scope.launch {
      sightingsMutex.withLock {
        sightings.clear()
        ssidMemory.clear()
        lastCsvWriteByBssid.clear()
      }
      _state.update { it.copy(sightings = emptyList()) }
      appendLog("Cleared visible sightings")
    }
  }

  fun clearStorageSessions(): Boolean {
    if (!controlLinkReady("Clear ESP storage")) return false
    if (_state.value.scanning) {
      appendLog("Storage clear blocked: stop scan first")
      return false
    }
    if (!bleClient.clearStorageSessions()) {
      appendLog("Storage clear command failed")
      return false
    }
    synchronized(ackLock) {
      sessionAcks.clear()
    }
    downloadBacklogAutoStop = false
    _state.update {
      it.copy(
        downloadBacklogActive = false,
        queuedRecords = 0L,
        queuedBytes = 0L,
        replayActive = false,
        replayCursor = 0L,
        queueFull = false,
        spiffsTotalBytes = 0L,
        spiffsUsedBytes = 0L,
        spiffsFreeBytes = 0L,
        blobActive = false,
        blobSessionId = 0L,
        blobBytesSent = 0L,
        blobBytesTotal = 0L,
      )
    }
    appendLog("ESP storage clear requested")
    bleClient.requestStatus()
    return true
  }

  fun pushPhoneGpsNow(): Boolean {
    if (!controlLinkReady("Push phone GPS")) return false
    if (phoneGpsPushActive) {
      phoneGpsPushActive = false
      gpsTracker.stop()
      bleClient.sendGpsClear()
      _state.update { it.copy(phoneGpsPushActive = false) }
      appendLog("Phone GPS streaming stopped")
      return true
    }
    phoneGpsPushActive = true
    _state.update { it.copy(phoneGpsPushActive = true) }
    gpsTracker.start()
    appendLog("Phone GPS streaming started")
    return true
  }

  fun seedDebugBacklog(targetBytes: Int = DEBUG_SEED_TARGET_BYTES): Boolean {
    if (!controlLinkReady("Seed synthetic backlog")) return false
    if (_state.value.scanning) {
      appendLog("Synthetic backlog seed blocked: stop scan first")
      return false
    }
    if (_state.value.downloadBacklogActive) {
      appendLog("Synthetic backlog seed blocked: stop backlog download first")
      return false
    }
    val clamped = targetBytes.coerceIn(DEBUG_SEED_MIN_BYTES, DEBUG_SEED_MAX_BYTES)
    val ok = bleClient.seedDebugStorage(clamped)
    if (ok) {
      appendLog("Synthetic backlog seed requested (${clamped / 1024} KB)")
      bleClient.requestStatus()
    } else {
      appendLog("Synthetic backlog seed command failed")
    }
    return ok
  }

  fun downloadBacklogToCsv(): Boolean {
    if (!controlLinkReady("Download backlog")) return false
    if (_state.value.scanning) {
      appendLog("Backlog download blocked: stop scan first")
      return false
    }
    if (_state.value.downloadBacklogActive) {
      val ok = bleClient.setBacklogBlobEnabled(false)
      if (!ok) {
        appendLog("Backlog stop command failed")
        return false
      }
      resetBacklogBlobReceiver(discardFile = true)
      _state.update { it.copy(downloadBacklogActive = false) }
      appendLog("Backlog download stopped")
      val shouldAutoStopCsv = downloadBacklogAutoStop
      downloadBacklogAutoStop = false
      if (shouldAutoStopCsv && csvLogger.isActive) {
        stopLogging()
      }
      return true
    }
    downloadBacklogAutoStop = !csvLogger.isActive
    if (downloadBacklogAutoStop) {
      startLogging()
    }
    synchronized(ackLock) {
      sessionAcks.clear()
    }
    val okBlob = bleClient.setBacklogBlobEnabled(true)
    if (!okBlob) {
      _state.update { it.copy(downloadBacklogActive = false) }
      appendLog("Backlog download request failed")
      if (downloadBacklogAutoStop && csvLogger.isActive) {
        stopLogging()
      }
      downloadBacklogAutoStop = false
      return false
    }
    _state.update { it.copy(downloadBacklogActive = true) }
    appendLog("Backlog download started")
    bleClient.requestStatus()
    return true
  }

  fun setHopMs(hopMs: Int): Boolean {
    if (!controlLinkReady("Set hop")) return false
    val clamped = hopMs.coerceIn(MIN_HOP_MS, MAX_HOP_MS)
    val autoEnabled = _state.value.autoHopEnabled
    val ok = bleClient.setHopMs(clamped)
    if (ok) {
      pendingManualHopApply = false
      _state.update {
        it.copy(
          autoHopBaseMs = clamped,
          autoHopAppliedMs = clamped,
        )
      }
      persistAutoHopSettings(autoEnabled, clamped)
      appendLog("Set hop interval to ${clamped}ms")
    }
    return ok
  }

  fun setAutoHopEnabled(enabled: Boolean, baseHopMs: Int): Boolean {
    val base = baseHopMs.coerceIn(MIN_HOP_MS, MAX_HOP_MS)
    _state.update { current ->
      current.copy(
        autoHopEnabled = enabled,
        autoHopBaseMs = base,
        autoHopAppliedMs = if (enabled) current.autoHopAppliedMs else base,
      )
    }
    persistAutoHopSettings(enabled, base)

    if (enabled) {
      pendingManualHopApply = false
      appendLog("Auto hop enabled (base ${base}ms)")
      if (!controlLinkReadySilent()) {
        appendLog("Auto hop pending: waiting for encrypted BLE link")
        return true
      }
      return maybeApplyAutoHop(force = true, reason = "enabled")
    }

    appendLog("Auto hop disabled")
    if (!controlLinkReadySilent()) {
      pendingManualHopApply = true
      appendLog("Manual hop ${base}ms pending: waiting for encrypted BLE link")
      return true
    }
    val ok = bleClient.setHopMs(base)
    if (ok) {
      pendingManualHopApply = false
      appendLog("Restored manual hop to ${base}ms")
    }
    return ok
  }

  fun setAutoHopBaseMs(baseHopMs: Int): Boolean {
    val base = baseHopMs.coerceIn(MIN_HOP_MS, MAX_HOP_MS)
    _state.update { current ->
      current.copy(
        autoHopBaseMs = base,
        autoHopAppliedMs = if (current.autoHopEnabled) current.autoHopAppliedMs else base,
      )
    }
    persistAutoHopSettings(_state.value.autoHopEnabled, base)

    val autoEnabled = _state.value.autoHopEnabled
    if (!controlLinkReadySilent()) {
      appendLog(
        if (autoEnabled) {
          "Auto-hop base ${base}ms saved (will apply when BLE is encrypted)"
        } else {
          "Manual hop ${base}ms pending: waiting for encrypted BLE link"
        },
      )
      if (!autoEnabled) {
        pendingManualHopApply = true
      }
      return true
    }

    if (autoEnabled) {
      appendLog("Auto-hop base updated to ${base}ms")
      return maybeApplyAutoHop(force = true, reason = "base-update")
    }

    val ok = bleClient.setHopMs(base)
    if (ok) {
      pendingManualHopApply = false
      appendLog("Set hop interval to ${base}ms")
    }
    return ok
  }

  fun setChannelMask(mask: Int): Boolean {
    if (!controlLinkReady("Set channel mask")) return false
    val clamped = mask and 0x1FFF
    if (clamped == 0) {
      appendLog("Channel set rejected: choose at least one channel")
      return false
    }
    val ok = bleClient.setChannelMask(clamped)
    if (ok) appendLog("Applied channel mask 0x${clamped.toString(16).uppercase(Locale.US)}")
    return ok
  }

  fun setBootMode(mode: Int): Boolean {
    if (!controlLinkReady("Set boot mode")) return false
    val normalized = if (mode == 1) 1 else 0
    val ok = bleClient.setBootMode(normalized)
    if (ok) appendLog("Set boot mode to ${if (normalized == 1) "Auto" else "Manual"}")
    return ok
  }

  fun setGpsNavMode(mode: Int): Boolean {
    val normalized = normalizeGpsNavMode(mode)
    gpsNavMode = normalized
    persistGpsNavModeSetting(normalized)
    _state.update { it.copy(gpsNavMode = normalized) }

    if (!controlLinkReadySilent()) {
      pendingGpsNavModeApply = true
      appendLog("GPS nav mode saved (${gpsNavModeLabel(normalized)}), pending BLE link")
      return true
    }

    val ok = bleClient.setGpsNavRateMode(normalized)
    if (ok) {
      pendingGpsNavModeApply = false
      appendLog("GPS nav mode set to ${gpsNavModeLabel(normalized)}")
    } else {
      pendingGpsNavModeApply = true
      appendLog("GPS nav mode apply failed, will retry")
    }
    return ok
  }

  fun setVisibleTimeout(sec: Int) {
    visibleTimeoutSec = sec.coerceIn(5, 300)
    _state.update { it.copy(visibleTimeoutSec = visibleTimeoutSec) }
    persistVisibleTimeoutSetting(visibleTimeoutSec)
  }

  fun startLogging() {
    try {
      val session = csvLogger.startSession()
      _state.update { it.copy(loggingEnabled = true, csvPath = session.displayPath) }
      appendLog("CSV logging started: ${session.displayPath}")
      if (lastEspGps == null && lastPhoneLocation == null) {
        appendLog("Waiting for GPS fix (ESP or phone) before writing CSV rows")
      }
    } catch (e: Exception) {
      appendLog("Failed to start CSV logging: ${e.message}")
    }
  }

  fun stopLogging() {
    downloadBacklogAutoStop = false
    csvLogger.stopSession()
    _state.update { it.copy(loggingEnabled = false, downloadBacklogActive = false) }
    appendLog("CSV logging stopped")
  }

  // ── EspBleClient.Listener ──────────────────────────────────

  override fun onConnectState(connected: Boolean) {
    if (!connected) {
      lastEspGps = null
      lastEspFixMs = -1L
      phoneGpsPushActive = false
      gpsTracker.stop()
      synchronized(ackLock) {
        sessionAcks.clear()
      }
      resetBacklogBlobReceiver(discardFile = true)
      val shouldAutoStopCsv = downloadBacklogAutoStop
      downloadBacklogAutoStop = false
      if (shouldAutoStopCsv && csvLogger.isActive) {
        stopLogging()
      }
    }
    _state.update { current ->
      current.copy(
        connected = connected,
        scanning = if (!connected) false else current.scanning,
        bleEncrypted = if (!connected) false else current.bleEncrypted,
        phoneSpeedKmh = if (!connected) 0f else current.phoneSpeedKmh,
        gpsValid = if (!connected) false else current.gpsValid,
        gpsSource = if (!connected) 0 else current.gpsSource,
        gpsAgeS = if (!connected) 0 else current.gpsAgeS,
        gpsAccuracyDm = if (!connected) 0 else current.gpsAccuracyDm,
        gpsSatCount = if (!connected) 0 else current.gpsSatCount,
        gpsHdopCenti = if (!connected) 0 else current.gpsHdopCenti,
        gpsPdopCenti = if (!connected) 0 else current.gpsPdopCenti,
        nodeLinkUp = if (!connected) false else current.nodeLinkUp,
        nodeLastSeenS = if (!connected) 0 else current.nodeLastSeenS,
        nodePacketsPerSec = if (!connected) 0 else current.nodePacketsPerSec,
        nodeForwardedSightings = if (!connected) 0 else current.nodeForwardedSightings,
        nodeChannel = if (!connected) 0 else current.nodeChannel,
        nodeChannelMask = if (!connected) 0 else current.nodeChannelMask,
        sessionOpen = if (!connected) false else current.sessionOpen,
        sessionId = if (!connected) 0L else current.sessionId,
        queuedRecords = if (!connected) 0L else current.queuedRecords,
        queuedBytes = if (!connected) 0L else current.queuedBytes,
        replayActive = if (!connected) false else current.replayActive,
        replayCursor = if (!connected) 0L else current.replayCursor,
        queueFull = if (!connected) false else current.queueFull,
        droppedFlashFull = if (!connected) 0L else current.droppedFlashFull,
        nodeCount = if (!connected) 0 else current.nodeCount,
        gpsNavAppliedHz = if (!connected) 0 else current.gpsNavAppliedHz,
        spiffsTotalBytes = if (!connected) 0L else current.spiffsTotalBytes,
        spiffsUsedBytes = if (!connected) 0L else current.spiffsUsedBytes,
        spiffsFreeBytes = if (!connected) 0L else current.spiffsFreeBytes,
        blobActive = if (!connected) false else current.blobActive,
        blobSessionId = if (!connected) 0L else current.blobSessionId,
        blobBytesSent = if (!connected) 0L else current.blobBytesSent,
        blobBytesTotal = if (!connected) 0L else current.blobBytesTotal,
        phoneGpsPushActive = if (!connected) false else current.phoneGpsPushActive,
        downloadBacklogActive = if (!connected) false else current.downloadBacklogActive,
      )
    }
    appendLog(if (connected) "Connected" else "Disconnected")
    updateNotification()
  }

  override fun onFrame(frame: WgFrame) {
    when (frame.type) {
      WgProtocol.MessageType.STATUS -> handleStatusFrame(frame.payload)
      WgProtocol.MessageType.GPS -> handleGpsFrame(frame.payload)
      WgProtocol.MessageType.SIGHTING -> handleSightingFrame(frame.payload)
      WgProtocol.MessageType.REPLAY_BATCH -> handleReplayBatchFrame(frame.payload)
      WgProtocol.MessageType.BACKLOG_BLOB_META -> handleBacklogBlobMetaFrame(frame.payload)
      WgProtocol.MessageType.BACKLOG_BLOB_CHUNK -> handleBacklogBlobChunkFrame(frame.payload)
      WgProtocol.MessageType.BACKLOG_BLOB_DONE -> handleBacklogBlobDoneFrame(frame.payload)
      WgProtocol.MessageType.ACK -> handleAckFrame(frame.payload)
      WgProtocol.MessageType.ERROR -> handleErrorFrame(frame.payload)
      WgProtocol.MessageType.SNAPSHOT_END -> appendLog("Snapshot complete")
      WgProtocol.MessageType.CONFIG -> appendLog("Config frame received (${frame.payload.size} bytes)")
      WgProtocol.MessageType.NODE_TABLE -> {}
      else -> appendLog("Unknown frame type 0x${frame.type.toString(16)}")
    }
  }

  override fun onInfo(message: String) {
    appendLog(message)
  }

  // ── Internal ───────────────────────────────────────────────

  private fun handleStatusFrame(payload: ByteArray) {
    val wasEncrypted = _state.value.bleEncrypted
    val wasReplayActive = _state.value.replayActive
    val status: StatusPayload = try {
      WgProtocol.decodeStatusPayload(payload)
    } catch (e: Exception) {
      appendLog("Status decode error: ${e.message}")
      return
    }

    _state.update { current ->
      current.copy(
        scanning = status.scanning,
        bleEncrypted = status.bleEncrypted,
        currentChannel = status.currentChannel,
        hopMs = status.hopMs,
        channelMask = status.channelMask,
        uniqueBssids = status.uniqueBssids,
        packetsPerSec = status.packetsPerSec,
        droppedNotifies = status.droppedNotifies,
        bootMode = status.bootMode,
        gpsValid = status.gpsValid,
        gpsSource = current.gpsSource,
        gpsAgeS = status.gpsAgeS,
        gpsAccuracyDm = status.gpsAccuracyDm,
        nodeLinkUp = status.nodeLinkUp,
        nodeLastSeenS = status.nodeLastSeenS,
        nodePacketsPerSec = status.nodePacketsPerSec,
        nodeForwardedSightings = status.nodeForwardedSightings,
        nodeChannel = status.nodeChannel,
        nodeChannelMask = status.nodeChannelMask,
        sessionOpen = status.sessionOpen,
        sessionId = status.sessionId,
        queuedRecords = status.queuedRecords,
        queuedBytes = status.queuedBytes,
        replayActive = status.replayActive,
        replayCursor = status.replayCursor,
        queueFull = status.queueFull,
        droppedFlashFull = status.droppedFlashFull,
        nodeCount = status.nodeCount,
        autoHopAppliedMs = status.hopMs,
        gpsNavAppliedHz = status.gpsNavAppliedHz,
        spiffsTotalBytes = status.spiffsTotalBytes,
        spiffsUsedBytes = status.spiffsUsedBytes,
        spiffsFreeBytes = status.spiffsFreeBytes,
        blobActive = status.blobActive,
        blobSessionId = status.blobSessionId,
        blobBytesSent = status.blobBytesSent,
        blobBytesTotal = status.blobBytesTotal,
      )
    }

    if (_state.value.downloadBacklogActive && status.scanning) {
      _state.update { it.copy(downloadBacklogActive = false) }
      appendLog("Backlog download stopped: scanner is active")
      val shouldAutoStopCsv = downloadBacklogAutoStop
      downloadBacklogAutoStop = false
      if (shouldAutoStopCsv && csvLogger.isActive) {
        stopLogging()
      }
    } else if (_state.value.downloadBacklogActive &&
      (
        (status.queuedRecords == 0L && !status.replayActive) ||
          (status.blobBytesTotal > 0L && !status.blobActive && status.blobBytesSent >= status.blobBytesTotal)
      )) {
      _state.update { it.copy(downloadBacklogActive = false) }
      if (controlLinkReadySilent()) {
        bleClient.setBacklogBlobEnabled(false)
      }
      resetBacklogBlobReceiver(discardFile = true)
      appendLog("Backlog download complete")
      val shouldAutoStopCsv = downloadBacklogAutoStop
      downloadBacklogAutoStop = false
      if (shouldAutoStopCsv && csvLogger.isActive) {
        stopLogging()
      }
    }

    if (!status.bleEncrypted) return
    if (pendingGpsNavModeApply || !wasEncrypted) {
      if (bleClient.setGpsNavRateMode(gpsNavMode)) {
        pendingGpsNavModeApply = false
        if (!wasEncrypted) {
          appendLog("GPS nav mode synced: ${gpsNavModeLabel(gpsNavMode)}")
        }
      } else {
        pendingGpsNavModeApply = true
      }
    }

    val replayJustWentIdle = wasReplayActive && !status.replayActive
    flushReplayAcks(force = replayJustWentIdle)

    if (_state.value.autoHopEnabled) {
      maybeApplyAutoHop(force = !wasEncrypted, reason = "status-sync")
      return
    }

    if (pendingManualHopApply) {
      val desiredHop = _state.value.autoHopBaseMs.coerceIn(MIN_HOP_MS, MAX_HOP_MS)
      val ok = bleClient.setHopMs(desiredHop)
      if (ok) {
        pendingManualHopApply = false
        appendLog("Applied pending manual hop ${desiredHop}ms")
      }
    }
  }

  private fun handleGpsFrame(payload: ByteArray) {
    val gps: EspGpsPayload = try {
      WgProtocol.decodeGpsPayload(payload)
    } catch (e: Exception) {
      appendLog("GPS decode error: ${e.message}")
      return
    }

    val now = System.currentTimeMillis()
    if (gps.valid) {
      lastEspGps = gps
      lastEspFixMs = now
    } else {
      lastEspGps = null
      lastEspFixMs = -1L
    }

    _state.update { current ->
      current.copy(
        gpsSource = gps.source,
        gpsSatCount = gps.satCount,
        gpsHdopCenti = gps.hdopCenti,
        gpsPdopCenti = gps.pdopCenti,
      )
    }

    if (_state.value.autoHopEnabled) {
      maybeApplyAutoHop(force = false, reason = "gps-frame")
    }
  }

  private fun handleSightingFrame(payload: ByteArray) {
    val sighting: SightingPayload = try {
      WgProtocol.decodeSightingPayload(payload)
    } catch (e: Exception) {
      appendLog("Sighting decode error: ${e.message}")
      return
    }
    val isReplay = (sighting.sourceFlags and WgProtocol.SIGHTING_SOURCE_REPLAY) != 0
    if (isReplay) {
      if (!trackReplayRecord(sighting.sessionId, sighting.recordSeq)) {
        return
      }
      if (_state.value.downloadBacklogActive) {
        appendCsvFromBacklogSighting(sighting, System.currentTimeMillis())
        return
      }
    }

    val now = System.currentTimeMillis()
    scope.launch {
      sightingsMutex.withLock {
        val existing = sightings[sighting.bssid]
        if (sighting.ssid.isNotBlank()) {
          ssidMemory[sighting.bssid] = sighting.ssid
        }
        val rememberedSsid = ssidMemory[sighting.bssid]
        val chosenSsid = rememberedSsid ?: existing?.ssid ?: sighting.ssid

        val next = SightingUi(
          bssid = sighting.bssid,
          ssid = chosenSsid,
          auth = sighting.auth,
          channel = sighting.channel,
          rssi = sighting.rssi,
          lastSeenMs = now,
          seenCount = (existing?.seenCount ?: 0) + 1,
        )
        sightings[sighting.bssid] = next
        maybeAppendCsvRow(sighting, next, now)
        publishSightingsLocked()
      }
    }
  }

  private fun handleReplayBatchFrame(payload: ByteArray) {
    val records: List<ByteArray> = try {
      WgProtocol.decodeReplayBatchPayload(payload)
    } catch (e: Exception) {
      appendLog("Replay batch decode error: ${e.message}")
      return
    }
    for (record in records) {
      handleSightingFrame(record)
    }
  }

  @Synchronized
  private fun resetBacklogBlobReceiver(discardFile: Boolean) {
    val rx = backlogBlobRx ?: return
    try {
      rx.out.flush()
    } catch (_: Exception) {}
    try {
      rx.out.close()
    } catch (_: Exception) {}
    if (discardFile) {
      try {
        rx.file.delete()
      } catch (_: Exception) {}
    }
    backlogBlobRx = null
  }

  private fun handleBacklogBlobMetaFrame(payload: ByteArray) {
    val meta: BacklogBlobMetaPayload = try {
      WgProtocol.decodeBacklogBlobMetaPayload(payload)
    } catch (e: Exception) {
      appendLog("Backlog meta decode error: ${e.message}")
      return
    }
    if (meta.sessionId <= 0L || meta.totalBytes <= 0L) {
      appendLog("Backlog meta rejected: bad session or size")
      return
    }

    synchronized(this) {
      resetBacklogBlobReceiver(discardFile = true)
      val dir = File(cacheDir, "backlog_blob")
      if (!dir.exists()) {
        dir.mkdirs()
      }
      val file = File(dir, "session-${java.lang.Long.toUnsignedString(meta.sessionId, 16)}.dat")
      val out = BufferedOutputStream(FileOutputStream(file, false), 64 * 1024)
      backlogBlobRx =
        BacklogBlobReceiver(
          sessionId = meta.sessionId,
          totalBytes = meta.totalBytes,
          writtenSeq = meta.writtenSeq,
          file = file,
          out = out,
        )
    }

    appendLog(
      "Backlog blob session 0x${java.lang.Long.toUnsignedString(meta.sessionId, 16)} size=${meta.totalBytes}B acked=${meta.ackedSeq} written=${meta.writtenSeq}",
    )
  }

  private fun handleBacklogBlobChunkFrame(payload: ByteArray) {
    val chunk: BacklogBlobChunkPayload = try {
      WgProtocol.decodeBacklogBlobChunkPayload(payload)
    } catch (e: Exception) {
      appendLog("Backlog chunk decode error: ${e.message}")
      return
    }

    synchronized(this) {
      val rx = backlogBlobRx
      if (rx == null) {
        appendLog("Backlog chunk ignored: no active session")
        return
      }
      if (rx.sessionId != chunk.sessionId) {
        appendLog("Backlog chunk session mismatch; restarting receiver")
        resetBacklogBlobReceiver(discardFile = true)
        return
      }
      if (chunk.offset != rx.receivedBytes) {
        appendLog("Backlog chunk offset mismatch; expected=${rx.receivedBytes} got=${chunk.offset}")
        resetBacklogBlobReceiver(discardFile = true)
        return
      }
      try {
        rx.out.write(chunk.data)
      } catch (e: Exception) {
        appendLog("Backlog chunk write failed: ${e.message}")
        resetBacklogBlobReceiver(discardFile = true)
        return
      }
      rx.receivedBytes += chunk.data.size.toLong()
      if (rx.receivedBytes % (64L * 1024L) < chunk.data.size.toLong() || rx.receivedBytes == rx.totalBytes) {
        appendLog("Backlog blob progress ${rx.receivedBytes}/${rx.totalBytes}B")
      }
    }
  }

  private fun handleBacklogBlobDoneFrame(payload: ByteArray) {
    val done: BacklogBlobDonePayload = try {
      WgProtocol.decodeBacklogBlobDonePayload(payload)
    } catch (e: Exception) {
      appendLog("Backlog done decode error: ${e.message}")
      return
    }

    val completed: BacklogBlobReceiver = synchronized(this) {
      val rx = backlogBlobRx
      if (rx == null) {
        appendLog("Backlog done ignored: no active session")
        return
      }
      if (rx.sessionId != done.sessionId) {
        appendLog("Backlog done session mismatch")
        resetBacklogBlobReceiver(discardFile = true)
        return
      }
      if (rx.receivedBytes != rx.totalBytes || done.totalBytes != rx.totalBytes) {
        appendLog("Backlog done size mismatch")
        resetBacklogBlobReceiver(discardFile = true)
        return
      }
      try {
        rx.out.flush()
      } catch (_: Exception) {}
      try {
        rx.out.close()
      } catch (_: Exception) {}
      backlogBlobRx = null
      rx
    }

    appendLog(
      "Backlog blob received session 0x${java.lang.Long.toUnsignedString(done.sessionId, 16)} (${done.totalBytes}B), importing...",
    )
    importBacklogBlobSessionAsync(
      file = completed.file,
      sessionId = done.sessionId,
      writtenSeq = if (done.writtenSeq > 0L) done.writtenSeq else completed.writtenSeq,
    )
  }

  private fun importBacklogBlobSessionAsync(file: File, sessionId: Long, writtenSeq: Long) {
    scope.launch {
      val result = parseBacklogBlobFile(file, sessionId)
      val ackSeq = maxOf(writtenSeq, result.highestSeq)
      appendLog(
        "Backlog import done sid=0x${java.lang.Long.toUnsignedString(sessionId, 16)} records=${result.records} bad_blocks=${result.badBlocks}",
      )
      if (ackSeq > 0L && controlLinkReadySilent()) {
        if (!bleClient.sendReplayAck(sessionId, ackSeq)) {
          appendLog("Backlog ack send failed sid=0x${java.lang.Long.toUnsignedString(sessionId, 16)}")
        }
      }
      try {
        file.delete()
      } catch (_: Exception) {}
      if (controlLinkReadySilent()) {
        bleClient.requestStatus()
      }
    }
  }

  private fun parseBacklogBlobFile(file: File, expectedSessionId: Long): BacklogImportResult {
    if (!file.exists()) {
      return BacklogImportResult(records = 0, highestSeq = 0L, badBlocks = 1)
    }

    var records = 0
    var highestSeq = 0L
    var badBlocks = 0
    val block = ByteArray(BLOB_BLOCK_SIZE)

    BufferedInputStream(FileInputStream(file), 64 * 1024).use { input ->
      while (true) {
        val read = readFullBlock(input, block)
        if (read < 0) break
        if (read != BLOB_BLOCK_SIZE) {
          badBlocks += 1
          break
        }

        val bb = ByteBuffer.wrap(block).order(ByteOrder.LITTLE_ENDIAN)
        val magic = bb.int.toLong() and 0xFFFF_FFFFL
        val version = bb.short.toInt() and 0xFFFF
        val headerSize = bb.short.toInt() and 0xFFFF
        val sessionId = bb.long
        val firstSeq = bb.int.toLong() and 0xFFFF_FFFFL
        val recordCount = bb.short.toInt() and 0xFFFF
        val payloadLen = bb.short.toInt() and 0xFFFF
        val expectedCrc = bb.int.toLong() and 0xFFFF_FFFFL

        if (magic != BLOB_BLOCK_MAGIC ||
            version != BLOB_BLOCK_VERSION ||
            headerSize != BLOB_BLOCK_HEADER_SIZE ||
            sessionId != expectedSessionId ||
            recordCount == 0 ||
            payloadLen == 0 ||
            payloadLen > BLOB_BLOCK_SIZE - headerSize) {
          badBlocks += 1
          continue
        }

        val crc = CRC32()
        crc.update(block, headerSize, payloadLen)
        val actualCrc = crc.value and 0xFFFF_FFFFL
        if (actualCrc != expectedCrc) {
          badBlocks += 1
          continue
        }

        var payloadPos = headerSize
        val payloadEnd = headerSize + payloadLen
        var blockValid = true
        for (index in 0 until recordCount) {
          if (payloadPos + BLOB_RECORD_HEADER_SIZE > payloadEnd) {
            blockValid = false
            break
          }
          val recordLen =
            (block[payloadPos].toInt() and 0xFF) or
              ((block[payloadPos + 1].toInt() and 0xFF) shl 8)
          payloadPos += BLOB_RECORD_HEADER_SIZE
          if (recordLen <= 0 || payloadPos + recordLen > payloadEnd) {
            blockValid = false
            break
          }
          val recordBytes = block.copyOfRange(payloadPos, payloadPos + recordLen)
          payloadPos += recordLen

          val sighting = try {
            WgProtocol.decodeSightingPayload(recordBytes)
          } catch (_: Exception) {
            continue
          }
          appendCsvFromBacklogSighting(sighting, System.currentTimeMillis())
          records += 1

          val seq = if (sighting.recordSeq > 0L) sighting.recordSeq else firstSeq + index
          if (seq > highestSeq) {
            highestSeq = seq
          }
        }
        if (!blockValid) {
          badBlocks += 1
        }
      }
    }

    return BacklogImportResult(records = records, highestSeq = highestSeq, badBlocks = badBlocks)
  }

  private fun readFullBlock(input: BufferedInputStream, target: ByteArray): Int {
    var offset = 0
    while (offset < target.size) {
      val read = input.read(target, offset, target.size - offset)
      if (read < 0) {
        return if (offset == 0) -1 else offset
      }
      offset += read
    }
    return offset
  }

  private fun handleAckFrame(payload: ByteArray) {
    val cmd = payload.getOrNull(0)?.toInt()?.and(0xFF) ?: 0
    appendLog("ACK cmd=0x${cmd.toString(16).padStart(2, '0')}")
  }

  private fun handleErrorFrame(payload: ByteArray) {
    val cmd = payload.getOrNull(0)?.toInt()?.and(0xFF) ?: 0
    val code = payload.getOrNull(1)?.toInt()?.and(0xFF) ?: 0
    appendLog("ERR cmd=0x${cmd.toString(16).padStart(2, '0')} code=$code")
  }

  private fun onPhoneLocation(location: Location) {
    lastPhoneLocation = location
    lastPhoneFixMs = System.currentTimeMillis()
    lastPhoneSpeedMmps = if (location.hasSpeed()) (location.speed * 1000f).roundToInt() else 0
    val speedKmh = speedMmpsToKmh(lastPhoneSpeedMmps)
    _state.update { current ->
      current.copy(
        phoneGpsAgeS = 0,
        phoneGpsAccuracyM = location.accuracy,
        phoneSpeedKmh = speedKmh,
      )
    }

    if (!bleClient.isConnected()) return
    if (!_state.value.bleEncrypted) return
    if (!phoneGpsPushActive) return

    val fix = locationToGpsFix(location)
    val ok = bleClient.sendGpsFix(fix)
    if (ok) {
      maybeApplyAutoHop(force = false, reason = "phone-gps-push")
    } else {
      appendLog("Failed to send phone GPS fix")
    }
  }

  private fun locationToGpsFix(location: Location): GpsFix {
    var flags = WgProtocol.GPS_FLAG_VALID
    val latE7 = (location.latitude * 10_000_000.0).roundToInt()
    val lonE7 = (location.longitude * 10_000_000.0).roundToInt()

    var altMm = 0
    if (location.hasAltitude()) {
      flags = flags or WgProtocol.GPS_FLAG_HAS_ALT
      altMm = (location.altitude * 1000.0).roundToInt()
    }

    var speedMmps = 0
    if (location.hasSpeed()) {
      flags = flags or WgProtocol.GPS_FLAG_HAS_SPEED
      speedMmps = (location.speed * 1000f).roundToInt()
    }

    var bearingMdeg = 0
    if (location.hasBearing()) {
      flags = flags or WgProtocol.GPS_FLAG_HAS_BEARING
      bearingMdeg = (location.bearing * 1000f).roundToInt()
    }

    val unixTimeS = (location.time / 1000L).coerceAtLeast(0L).toInt()
    val accuracyCm = (location.accuracy * 100f).roundToInt().coerceIn(1, 0xFFFF)

    return GpsFix(
      flags = flags,
      latE7 = latE7,
      lonE7 = lonE7,
      altMm = altMm,
      speedMmps = speedMmps,
      bearingMdeg = bearingMdeg,
      unixTimeS = unixTimeS,
      accuracyCm = accuracyCm,
    )
  }

  private suspend fun pruneSightingsAndRefreshAges() {
    val now = System.currentTimeMillis()
    val timeoutMs = visibleTimeoutSec * 1000L
    var removed = false

    sightingsMutex.withLock {
      if (sightings.isNotEmpty()) {
        val iter = sightings.entries.iterator()
        while (iter.hasNext()) {
          val entry = iter.next().value
          if (now - entry.lastSeenMs > timeoutMs) {
            iter.remove()
            removed = true
          }
        }
        if (removed) {
          publishSightingsLocked()
        }
      }
    }

    val phoneAge = if (lastPhoneFixMs <= 0L) -1 else ((now - lastPhoneFixMs) / 1000L).toInt()
    val normalizedPhoneAge = phoneAge.coerceAtLeast(-1)
    if (normalizedPhoneAge != _state.value.phoneGpsAgeS) {
      _state.update { current -> current.copy(phoneGpsAgeS = normalizedPhoneAge) }
    }
    flushReplayAcks(force = false)
  }

  private fun publishSightingsLocked() {
    val sorted = sightings.values.sortedByDescending { it.lastSeenMs }
    _state.update { current -> current.copy(sightings = sorted) }
  }

  private data class CsvLocation(
    val latitude: Double,
    val longitude: Double,
    val altitudeMeters: Double,
    val accuracyMeters: Double,
  )

  private data class CsvSample(
    val rowTimestampMs: Long,
    val location: CsvLocation,
  )

  private fun resolveCsvLocation(nowMs: Long): CsvLocation? {
    val esp = lastEspGps
    val espAgeMs = if (lastEspFixMs <= 0L) Long.MAX_VALUE else nowMs - lastEspFixMs
    if (esp != null && esp.valid && espAgeMs <= MAX_ESP_GPS_AGE_MS_FOR_LOGGING) {
      return CsvLocation(
        latitude = esp.latE7 / 10_000_000.0,
        longitude = esp.lonE7 / 10_000_000.0,
        altitudeMeters = esp.altMm / 1000.0,
        accuracyMeters = esp.accuracyCm / 100.0,
      )
    }

    val phone = lastPhoneLocation
    val phoneAgeMs = if (lastPhoneFixMs <= 0L) Long.MAX_VALUE else nowMs - lastPhoneFixMs
    if (phone != null && phoneAgeMs <= MAX_PHONE_GPS_AGE_MS_FOR_LOGGING) {
      return CsvLocation(
        latitude = phone.latitude,
        longitude = phone.longitude,
        altitudeMeters = if (phone.hasAltitude()) phone.altitude else 0.0,
        accuracyMeters = phone.accuracy.toDouble(),
      )
    }

    return null
  }

  private fun resolveAutoHopSpeedMmps(nowMs: Long): Int {
    val esp = lastEspGps
    val espAgeMs = if (lastEspFixMs <= 0L) Long.MAX_VALUE else nowMs - lastEspFixMs
    if (esp != null && esp.valid && espAgeMs <= MAX_ESP_GPS_AGE_MS_FOR_LOGGING) {
      return esp.speedMmps.coerceAtLeast(0)
    }
    val phoneAgeMs = if (lastPhoneFixMs <= 0L) Long.MAX_VALUE else nowMs - lastPhoneFixMs
    if (phoneAgeMs <= MAX_PHONE_GPS_AGE_MS_FOR_LOGGING) {
      return lastPhoneSpeedMmps.coerceAtLeast(0)
    }
    return 0
  }

  private fun resolveCsvSample(sighting: SightingPayload, nowMs: Long): CsvSample? {
    val hasPayloadGps = sighting.gpsValid &&
      sighting.gpsLatE7 in -900_000_000..900_000_000 &&
      sighting.gpsLonE7 in -1_800_000_000..1_800_000_000
    if (hasPayloadGps) {
      val rowTsMs = if (sighting.gpsUnixTimeS > 0L) sighting.gpsUnixTimeS * 1000L else nowMs
      return CsvSample(
        rowTimestampMs = rowTsMs,
        location =
          CsvLocation(
            latitude = sighting.gpsLatE7 / 10_000_000.0,
            longitude = sighting.gpsLonE7 / 10_000_000.0,
            altitudeMeters = sighting.gpsAltMm / 1000.0,
            accuracyMeters = if (sighting.gpsAccuracyCm > 0) sighting.gpsAccuracyCm / 100.0 else 0.0,
          ),
      )
    }

    val fallback = resolveCsvLocation(nowMs) ?: return null
    return CsvSample(rowTimestampMs = nowMs, location = fallback)
  }

  private fun maybeAppendCsvRow(payload: SightingPayload, sighting: SightingUi, nowMs: Long) {
    if (!csvLogger.isActive) return

    if ((payload.flags and WgProtocol.SIGHTING_FLAG_SNAPSHOT) != 0) {
      return
    }

    val sample = resolveCsvSample(payload, nowMs)
    if (sample == null) {
      if (nowMs - lastNoGpsLogMs >= NO_GPS_LOG_THROTTLE_MS) {
        lastNoGpsLogMs = nowMs
        appendLog("CSV paused: waiting for valid GPS fix")
      }
      return
    }

    val rowTsMs = sample.rowTimestampMs
    val lastWrite = lastCsvWriteByBssid[sighting.bssid] ?: 0L
    if (lastWrite > 0L) {
      if (rowTsMs <= lastWrite) return
      if (rowTsMs - lastWrite < MIN_LOG_INTERVAL_PER_BSSID_MS) return
    }
    lastCsvWriteByBssid[sighting.bssid] = rowTsMs

    appendCsvRowValues(
      bssid = sighting.bssid,
      ssid = sighting.ssid,
      auth = sighting.auth,
      channel = sighting.channel,
      rssi = sighting.rssi,
      rowTs = rowTsMs,
      location = sample.location,
    )
  }

  private fun appendCsvFromBacklogSighting(sighting: SightingPayload, nowMs: Long) {
    if (!csvLogger.isActive) return
    if ((sighting.flags and WgProtocol.SIGHTING_FLAG_SNAPSHOT) != 0) return

    val sample = resolveCsvSample(sighting, nowMs) ?: return
    if (sighting.ssid.isNotBlank()) {
      ssidMemory[sighting.bssid] = sighting.ssid
    }
    val resolvedSsid = if (sighting.ssid.isNotBlank()) sighting.ssid else (ssidMemory[sighting.bssid] ?: "")
    appendCsvRowValues(
      bssid = sighting.bssid,
      ssid = resolvedSsid,
      auth = sighting.auth,
      channel = sighting.channel,
      rssi = sighting.rssi,
      rowTs = sample.rowTimestampMs,
      location = sample.location,
    )
  }

  private fun appendCsvRowValues(
    bssid: String,
    ssid: String,
    auth: String,
    channel: Int,
    rssi: Int,
    rowTs: Long,
    location: CsvLocation,
  ) {
    try {
      csvLogger.append(
        WigleWifiRow(
          mac = bssid,
          ssid = ssid,
          authMode = auth,
          firstSeenEpochMs = rowTs,
          channel = channel,
          rssi = rssi,
          latitude = location.latitude,
          longitude = location.longitude,
          altitudeMeters = location.altitudeMeters,
          accuracyMeters = location.accuracyMeters,
          type = "WIFI",
        ),
      )
    } catch (e: Exception) {
      appendLog("CSV write failed: ${e.message}")
    }
  }

  private fun trackReplayRecord(sessionId: Long, recordSeq: Long): Boolean {
    if (sessionId <= 0L || recordSeq <= 0L) return true
    synchronized(ackLock) {
      val tracker = sessionAcks.getOrPut(sessionId) { SessionAckTracker() }
      val highest = tracker.highestContiguous
      if (recordSeq <= highest) {
        maybeSendReplayAckLocked(sessionId, tracker, force = false)
        return false
      }
      if (recordSeq == highest + 1L) {
        tracker.highestContiguous = recordSeq
        while (tracker.gaps.remove(tracker.highestContiguous + 1L)) {
          tracker.highestContiguous += 1L
        }
        maybeSendReplayAckLocked(sessionId, tracker, force = false)
        return true
      }
      val added = tracker.gaps.add(recordSeq)
      maybeSendReplayAckLocked(sessionId, tracker, force = false)
      return added
    }
  }

  private fun flushReplayAcks(force: Boolean) {
    synchronized(ackLock) {
      for ((sessionId, tracker) in sessionAcks) {
        maybeSendReplayAckLocked(sessionId, tracker, force)
      }
    }
  }

  private fun maybeSendReplayAckLocked(sessionId: Long, tracker: SessionAckTracker, force: Boolean) {
    if (tracker.highestContiguous <= 0L || tracker.highestContiguous <= tracker.lastSent) return
    if (!controlLinkReadySilent()) return
    val now = System.currentTimeMillis()
    val delta = tracker.highestContiguous - tracker.lastSent
    if (!force && delta < REPLAY_ACK_BATCH_SIZE && now - tracker.lastSentMs < REPLAY_ACK_MIN_INTERVAL_MS) {
      return
    }
    if (bleClient.sendReplayAck(sessionId, tracker.highestContiguous)) {
      tracker.lastSent = tracker.highestContiguous
      tracker.lastSentMs = now
    }
  }

  private fun appendLog(message: String) {
    val line = "[${logTime.format(Date())}] $message"
    _state.update { current ->
      val next = mutableListOf(line)
      next.addAll(current.logs)
      if (next.size > MAX_LOG_LINES) {
        next.subList(MAX_LOG_LINES, next.size).clear()
      }
      current.copy(logs = next)
    }
  }

  private fun controlLinkReady(action: String): Boolean {
    val snapshot = _state.value
    if (!snapshot.connected) {
      appendLog("$action blocked: not connected")
      return false
    }
    if (!snapshot.bleEncrypted) {
      appendLog("$action blocked: waiting for encrypted BLE link")
      return false
    }
    return true
  }

  private fun controlLinkReadySilent(): Boolean {
    val snapshot = _state.value
    return snapshot.connected && snapshot.bleEncrypted
  }

  private fun maybeApplyAutoHop(force: Boolean, reason: String): Boolean {
    val snapshot = _state.value
    if (!snapshot.autoHopEnabled) return true
    if (!controlLinkReadySilent()) return false

    val now = System.currentTimeMillis()
    val speedMmps = resolveAutoHopSpeedMmps(now)
    val nextHop = computeAutoHopMs(snapshot.autoHopBaseMs, speedMmps)
    if (!force) {
      if (abs(nextHop - snapshot.autoHopAppliedMs) < AUTO_HOP_MIN_DELTA_MS) return true
      if (now - lastAutoHopApplyMs < AUTO_HOP_MIN_APPLY_INTERVAL_MS) return true
    }

    val ok = bleClient.setHopMs(nextHop)
    if (!ok) return false

    lastAutoHopApplyMs = now
    _state.update { it.copy(autoHopAppliedMs = nextHop) }

    if (force || now - lastAutoHopLogMs >= AUTO_HOP_LOG_INTERVAL_MS) {
      lastAutoHopLogMs = now
      appendLog(
        "Auto hop ${snapshot.autoHopBaseMs}→${nextHop}ms @ ${"%.1f".format(Locale.US, speedMmpsToKmh(speedMmps))} km/h ($reason)",
      )
    }
    return true
  }

  private fun updateNotification() {
    val s = _state.value
    val notification = NotificationHelper.build(
      context = this,
      connected = s.connected,
      scanning = s.scanning,
      apCount = s.uniqueBssids,
      csvActive = s.loggingEnabled,
    )
    val manager = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
    manager.notify(NotificationHelper.NOTIFICATION_ID, notification)
  }
}
