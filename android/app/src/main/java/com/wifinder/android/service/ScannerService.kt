package com.wifinder.android.service

import android.Manifest
import android.annotation.SuppressLint
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.location.Location
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiNetworkSpecifier
import android.os.Binder
import android.os.Build
import android.os.IBinder
import com.wifinder.android.ble.WiFinderBleClient
import com.wifinder.android.logging.GpxTrackLogger
import com.wifinder.android.gps.GpsTracker
import com.wifinder.android.logging.WigleCsvLogger
import com.wifinder.android.model.EspGpsPayload
import com.wifinder.android.model.GpxTrackSample
import com.wifinder.android.model.GpxTrackSampling
import com.wifinder.android.model.GpsFix
import com.wifinder.android.model.BacklogBlobDonePayload
import com.wifinder.android.model.BacklogBlobMetaPayload
import com.wifinder.android.model.SightingPayload
import com.wifinder.android.model.StatusPayload
import com.wifinder.android.model.WifiBand
import com.wifinder.android.model.WgFrame
import com.wifinder.android.model.WgProtocol
import com.wifinder.android.model.WigleWifiRow
import com.wifinder.android.ui.SightingUi
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TreeSet
import java.util.zip.CRC32
import kotlin.math.abs
import kotlin.math.roundToInt
import kotlin.coroutines.resume
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import org.json.JSONObject

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
  val localChannelMask: Int = 0x1FFF,
  val uniqueBssids: Int = 0,
  val packetsPerSec: Int = 0,
  val droppedNotifies: Int = 0,
  val bootMode: Int = 0,
  val gpsValid: Boolean = false,
  val gpsSource: Int = 0,
  val gpsAgeS: Int = 0,
  val gpsAccuracyDm: Int = 0,
  val gpsSatCount: Int = 0,
  val gpsSatInUse: Int = 0,
  val gpsSatInView: Int = 0,
  val gpsHdopCenti: Int = 0,
  val gpsPdopCenti: Int = 0,
  val gpsVdopCenti: Int = 0,
  val nodeLinkUp: Boolean = false,
  val nodeLastSeenS: Int = 0,
  val nodePacketsPerSec: Int = 0,
  val nodeForwardedSightings: Int = 0,
  val nodeChannel: Int = 0,
  val nodeChannelMask: Int = 0,
  val nodeChannelMask24: Int = 0,
  val nodeChannelMask5Ghz: Long = 0L,
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
  val dieTempCenti: Int = Int.MIN_VALUE,
  val blobActive: Boolean = false,
  val blobSessionId: Long = 0L,
  val blobBytesSent: Long = 0L,
  val blobBytesTotal: Long = 0L,
  val phoneGpsPushActive: Boolean = false,
  val downloadBacklogActive: Boolean = false,
  val gpxEnabled: Boolean = true,
  val gpxLogging: Boolean = false,
  val gpxPath: String = "",
  val gpxPointCount: Long = 0L,
  val loggingEnabled: Boolean = false,
  val csvPath: String = "",
  val sightings: List<SightingUi> = emptyList(),
  val logs: List<String> = emptyList(),
)

class ScannerService : Service(), WiFinderBleClient.Listener {

  companion object {
    const val ACTION_STOP = "com.wifinder.android.STOP_SERVICE"
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
    private const val PREFS_NAME = "wifinder_settings"
    private const val PREF_VISIBLE_TIMEOUT_SEC = "visible_timeout_sec"
    private const val PREF_AUTO_HOP_ENABLED = "auto_hop_enabled"
    private const val PREF_AUTO_HOP_BASE_MS = "auto_hop_base_ms"
    private const val PREF_GPS_NAV_MODE = "gps_nav_mode"
    private const val PREF_GPX_ENABLED = "gpx_enabled"
    private const val GPX_MIN_ELAPSED_MS = 1000L
    private const val GPX_MIN_DISTANCE_METERS = 5.0
    private const val BLOB_BLOCK_SIZE = 1024
    private const val BLOB_BLOCK_MAGIC = 0x314B4257L
    private const val BLOB_BLOCK_VERSION = 1
    private const val BLOB_BLOCK_HEADER_SIZE = 28
    private const val BLOB_RECORD_HEADER_SIZE = 2
    private const val BLOB_PROGRESS_UI_INTERVAL_MS = 120L
    private const val BLOB_PROGRESS_UI_STEP_BYTES = 16L * 1024L
    private const val DEBUG_SEED_TARGET_BYTES = 768 * 1024
    private const val DEBUG_SEED_MIN_BYTES = 512 * 1024
    private const val DEBUG_SEED_MAX_BYTES = 1024 * 1024
    private const val WIFI_DUMP_AP_SSID = "WIFINDER-DUMP"
    private const val WIFI_DUMP_AP_HOST = "192.168.4.1"
    private const val WIFI_DUMP_MANIFEST_PATH = "/wifinder/dump/manifest.json"
    private const val WIFI_DUMP_FILE_PATH = "/wifinder/dump/file"
    private const val WIFI_DUMP_HTTP_CONNECT_TIMEOUT_MS = 8_000
    private const val WIFI_DUMP_HTTP_READ_TIMEOUT_MS = 20_000
    private const val WIFI_DUMP_NETWORK_WAIT_MS = 25_000L
    private const val WIFI_DUMP_PROGRESS_UI_INTERVAL_MS = 150L

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
    val gapDetected: Boolean,
    val badBlocks: Int,
  )

  private data class WifiDumpSession(
    val sessionId: Long,
    val datBytes: Long,
    val gpxBytes: Long,
    val writtenSeq: Long,
    val ackedSeq: Long,
  )

  private data class WifiDumpManifest(
    val runId: Long,
    val sessions: List<WifiDumpSession>,
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
  private val backlogRecoveryLock = Any()
  private val sessionAcks = hashMapOf<Long, SessionAckTracker>()

  private val logTime = SimpleDateFormat("HH:mm:ss", Locale.US)

  private lateinit var bleClient: WiFinderBleClient
  private lateinit var gpxLogger: GpxTrackLogger
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
  @Volatile private var gpxEnabled: Boolean = true
  @Volatile private var liveGpxSessionId: Long = 0L
  @Volatile private var lastLiveGpxSample: GpxTrackSample? = null
  @Volatile private var phoneGpsPushActive: Boolean = false
  @Volatile private var downloadBacklogAutoStop: Boolean = false
  @Volatile private var downloadBacklogStartMs: Long = 0L
  @Volatile private var visibleTimeoutSec: Int = 25
  @Volatile private var backlogBlobRecoveryInFlight: Boolean = false
  @Volatile private var backlogBlobRx: BacklogBlobReceiver? = null
  @Volatile private var blobProgressUiLastMs: Long = 0L
  @Volatile private var blobProgressUiLastBytes: Long = 0L
  @Volatile private var wifiDumpInProgress: Boolean = false
  @Volatile private var wifiDumpJob: Job? = null

  override fun onCreate() {
    super.onCreate()
    NotificationHelper.createChannel(this)
    prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
    bleClient = WiFinderBleClient(applicationContext, this)
    gpxLogger = GpxTrackLogger(applicationContext)
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
    wifiDumpJob?.cancel()
    wifiDumpJob = null
    wifiDumpInProgress = false
    try { bleClient.sendGpsClear() } catch (_: Exception) {}
    try { bleClient.setWifiDumpEnabled(false) } catch (_: Exception) {}
    stopLiveGpxSession(reason = null)
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
    gpxEnabled = prefs.getBoolean(PREF_GPX_ENABLED, true)
    _state.update {
      it.copy(
        visibleTimeoutSec = visibleTimeoutSec,
        autoHopEnabled = autoHopEnabled,
        autoHopBaseMs = autoHopBase,
        autoHopAppliedMs = autoHopBase,
        gpsNavMode = gpsNavMode,
        gpxEnabled = gpxEnabled,
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

  private fun persistGpxEnabledSetting(enabled: Boolean) {
    prefs.edit().putBoolean(PREF_GPX_ENABLED, enabled).apply()
  }

  // ── Public API (called by ViewModel via binder) ─────────────

  fun doConnect() {
    bleClient.connect()
  }

  fun doDisconnect() {
    wifiDumpJob?.cancel()
    wifiDumpJob = null
    wifiDumpInProgress = false
    phoneGpsPushActive = false
    stopLiveGpxSession(reason = null)
    if (controlLinkReadySilent()) {
      bleClient.setBacklogBlobEnabled(false)
      bleClient.setWifiDumpEnabled(false)
    }
    val shouldAutoStopCsv = downloadBacklogAutoStop
    downloadBacklogAutoStop = false
    downloadBacklogStartMs = 0L
    synchronized(backlogRecoveryLock) {
      backlogBlobRecoveryInFlight = false
    }
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
      appendLog("Backlog download: scanner active, sending stop first")
      if (!bleClient.sendStop()) {
        appendLog("Backlog download failed: stop command failed")
        return false
      }
    }
    if (_state.value.downloadBacklogActive) {
      wifiDumpJob?.cancel()
      wifiDumpJob = null
      wifiDumpInProgress = false
      val okWifiDump = bleClient.setWifiDumpEnabled(false)
      val okBlob = bleClient.setBacklogBlobEnabled(false)
      if (!okWifiDump && !okBlob) {
        appendLog("Backlog stop command failed")
        return false
      }
      downloadBacklogStartMs = 0L
      resetBacklogBlobReceiver(discardFile = true)
      _state.update {
        it.copy(
          downloadBacklogActive = false,
          blobActive = false,
          blobSessionId = 0L,
          blobBytesSent = 0L,
          blobBytesTotal = 0L,
        )
      }
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
    val okWifiDump = bleClient.setWifiDumpEnabled(true)
    if (!okWifiDump) {
      downloadBacklogStartMs = 0L
      _state.update {
        it.copy(
          downloadBacklogActive = false,
          blobActive = false,
          blobSessionId = 0L,
          blobBytesSent = 0L,
          blobBytesTotal = 0L,
        )
      }
      appendLog("Backlog download request failed")
      if (downloadBacklogAutoStop && csvLogger.isActive) {
        stopLogging()
      }
      downloadBacklogAutoStop = false
      return false
    }

    downloadBacklogStartMs = System.currentTimeMillis()
    wifiDumpInProgress = true
    _state.update {
      it.copy(
        downloadBacklogActive = true,
        blobActive = true,
        blobSessionId = 0L,
        blobBytesSent = 0L,
        blobBytesTotal = 0L,
      )
    }
    appendLog("Backlog download started (Wi-Fi dump)")
    wifiDumpJob?.cancel()
    wifiDumpJob = scope.launch {
      runWifiDumpTransfer()
    }
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

  fun setChannelPlan(localMask: Int, nodeMask24: Int, nodeMask5Ghz: Long): Boolean {
    if (!controlLinkReady("Set channel plan")) return false
    val local = localMask and 0x1FFF
    val node24 = nodeMask24 and 0x1FFF
    val node5 = WifiBand.sanitizeNode5GhzMask(nodeMask5Ghz)
    if (local == 0) {
      appendLog("Channel plan rejected: master needs at least one 2.4 GHz channel")
      return false
    }
    if (node24 == 0 && node5 == 0L) {
      appendLog("Channel plan rejected: slave needs at least one channel")
      return false
    }
    val ok = bleClient.setChannelPlan(local, node24, node5)
    if (ok) {
      appendLog(
        "Applied channel plan M24=0x${local.toString(16).uppercase(Locale.US)} " +
          "S24=0x${node24.toString(16).uppercase(Locale.US)} " +
          "S5=0x${node5.toULong().toString(16).uppercase(Locale.US)}",
      )
    }
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

  fun setGpxEnabled(enabled: Boolean) {
    gpxEnabled = enabled
    persistGpxEnabledSetting(enabled)
    if (!enabled) {
      stopLiveGpxSession(reason = null)
    } else {
      val snapshot = _state.value
      if (snapshot.connected && snapshot.sessionOpen && snapshot.scanning && snapshot.sessionId != 0L) {
        startLiveGpxSession(snapshot.sessionId)
      }
    }
    _state.update { it.copy(gpxEnabled = enabled, gpxLogging = gpxLogger.isActive) }
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
    downloadBacklogStartMs = 0L
    csvLogger.stopSession()
    _state.update { it.copy(loggingEnabled = false, downloadBacklogActive = false) }
    appendLog("CSV logging stopped")
  }

  // ── WiFinderBleClient.Listener ──────────────────────────────────

  override fun onConnectState(connected: Boolean) {
    if (!connected) {
      wifiDumpJob?.cancel()
      wifiDumpJob = null
      wifiDumpInProgress = false
      lastEspGps = null
      lastEspFixMs = -1L
      stopLiveGpxSession(reason = null)
      phoneGpsPushActive = false
      gpsTracker.stop()
      synchronized(ackLock) {
        sessionAcks.clear()
      }
      resetBacklogBlobReceiver(discardFile = true)
      val shouldAutoStopCsv = downloadBacklogAutoStop
      downloadBacklogAutoStop = false
      downloadBacklogStartMs = 0L
      synchronized(backlogRecoveryLock) {
        backlogBlobRecoveryInFlight = false
      }
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
        gpsSatInUse = if (!connected) 0 else current.gpsSatInUse,
        gpsSatInView = if (!connected) 0 else current.gpsSatInView,
        gpsHdopCenti = if (!connected) 0 else current.gpsHdopCenti,
        gpsPdopCenti = if (!connected) 0 else current.gpsPdopCenti,
        gpsVdopCenti = if (!connected) 0 else current.gpsVdopCenti,
        nodeLinkUp = if (!connected) false else current.nodeLinkUp,
        nodeLastSeenS = if (!connected) 0 else current.nodeLastSeenS,
        nodePacketsPerSec = if (!connected) 0 else current.nodePacketsPerSec,
        nodeForwardedSightings = if (!connected) 0 else current.nodeForwardedSightings,
        nodeChannel = if (!connected) 0 else current.nodeChannel,
        nodeChannelMask = if (!connected) 0 else current.nodeChannelMask,
        nodeChannelMask24 = if (!connected) 0 else current.nodeChannelMask24,
        nodeChannelMask5Ghz = if (!connected) 0L else current.nodeChannelMask5Ghz,
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
        dieTempCenti = if (!connected) Int.MIN_VALUE else current.dieTempCenti,
        blobActive = if (!connected) false else current.blobActive,
        blobSessionId = if (!connected) 0L else current.blobSessionId,
        blobBytesSent = if (!connected) 0L else current.blobBytesSent,
        blobBytesTotal = if (!connected) 0L else current.blobBytesTotal,
        phoneGpsPushActive = if (!connected) false else current.phoneGpsPushActive,
        downloadBacklogActive = if (!connected) false else current.downloadBacklogActive,
        gpxEnabled = current.gpxEnabled,
        gpxLogging = if (!connected) false else current.gpxLogging,
        gpxPath = current.gpxPath,
        gpxPointCount = current.gpxPointCount,
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
        channelMask = status.localChannelMask,
        localChannelMask = status.localChannelMask,
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
        nodeChannelMask = status.nodeChannelMask24,
        nodeChannelMask24 = status.nodeChannelMask24,
        nodeChannelMask5Ghz = status.nodeChannelMask5Ghz,
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
        dieTempCenti = status.dieTempCenti,
        blobActive = if (wifiDumpInProgress) current.blobActive else status.blobActive,
        blobSessionId = if (wifiDumpInProgress) current.blobSessionId else status.blobSessionId,
        blobBytesSent = if (wifiDumpInProgress) current.blobBytesSent else status.blobBytesSent,
        blobBytesTotal = if (wifiDumpInProgress) current.blobBytesTotal else status.blobBytesTotal,
      )
    }

    syncLiveGpxSession(status)

    val nowMs = System.currentTimeMillis()
    val withinStartGrace =
      downloadBacklogStartMs > 0L && (nowMs - downloadBacklogStartMs) < 2500L
    if (_state.value.downloadBacklogActive && status.scanning && !withinStartGrace) {
      downloadBacklogStartMs = 0L
      _state.update { it.copy(downloadBacklogActive = false) }
      appendLog("Backlog download stopped: scanner is active")
      val shouldAutoStopCsv = downloadBacklogAutoStop
      downloadBacklogAutoStop = false
      if (shouldAutoStopCsv && csvLogger.isActive) {
        stopLogging()
      }
    } else if (_state.value.downloadBacklogActive &&
      !wifiDumpInProgress &&
      status.queuedRecords == 0L &&
      !status.replayActive &&
      !status.blobActive) {
      _state.update { it.copy(downloadBacklogActive = false) }
      if (controlLinkReadySilent()) {
        bleClient.setBacklogBlobEnabled(false)
      }
      downloadBacklogStartMs = 0L
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
        gpsSatCount = if (gps.satInUse > 0) gps.satInUse else gps.satCount,
        gpsSatInUse = gps.satInUse,
        gpsSatInView = gps.satInView,
        gpsHdopCenti = gps.hdopCenti,
        gpsPdopCenti = gps.pdopCenti,
        gpsVdopCenti = gps.vdopCenti,
      )
    }

    if (gpxEnabled) {
      val sample = gpxSampleFromEspGps(gps, now)
      if (sample != null) {
        maybeAppendLiveGpxPoint(
          sample = sample,
          altitudeMeters = gps.altMm / 1000.0,
          hdop = if (gps.hdopCenti > 0) gps.hdopCenti / 100.0 else null,
          satCount = gps.satInUse.takeIf { it > 0 } ?: gps.satCount.takeIf { it > 0 },
        )
      }
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
    blobProgressUiLastMs = 0L
    blobProgressUiLastBytes = 0L
  }

  private fun handleBacklogBlobMetaFrame(payload: ByteArray) {
    val meta: BacklogBlobMetaPayload = try {
      WgProtocol.decodeBacklogBlobMetaPayload(payload)
    } catch (e: Exception) {
      appendLog("Backlog meta decode error: ${e.message}")
      return
    }
    if (meta.sessionId == 0L || meta.totalBytes <= 0L) {
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
      blobProgressUiLastMs = 0L
      blobProgressUiLastBytes = 0L
    }

    appendLog(
      "Backlog blob session 0x${java.lang.Long.toUnsignedString(meta.sessionId, 16)} size=${meta.totalBytes}B acked=${meta.ackedSeq} written=${meta.writtenSeq}",
    )
    _state.update {
      it.copy(
        blobActive = true,
        blobSessionId = meta.sessionId,
        blobBytesSent = 0L,
        blobBytesTotal = meta.totalBytes,
      )
    }
  }

  private fun handleBacklogBlobChunkFrame(payload: ByteArray) {
    if (payload.size < 14) {
      appendLog("Backlog chunk decode error: payload too short")
      return
    }
    val sessionId = readU64Le(payload, 0)
    val offset = readU32Le(payload, 8)
    val chunkLen = readU16Le(payload, 12)
    if (chunkLen <= 0 || payload.size != 14 + chunkLen) {
      appendLog("Backlog chunk decode error: payload length mismatch")
      return
    }

    var uiUpdateSessionId = 0L
    var uiUpdateBytes = 0L
    var uiUpdateTotal = 0L
    var shouldUpdateUi = false
    var replyOffset = offset
    var replyLen = chunkLen
    var replyAccept = false

    synchronized(this) {
      val rx = backlogBlobRx
      if (rx == null) {
        appendLog("Backlog chunk ignored: no active session")
        replyOffset = 0L
        replyLen = 0
      } else if (rx.sessionId != sessionId) {
        appendLog(
          "Backlog chunk session mismatch; expected=0x${java.lang.Long.toUnsignedString(rx.sessionId, 16)} got=0x${java.lang.Long.toUnsignedString(sessionId, 16)}",
        )
        replyOffset = rx.receivedBytes
        replyLen = 0
      } else if (offset < rx.receivedBytes) {
        // Duplicate chunk (typically ACK loss); ACK again.
        replyAccept = true
      } else if (offset > rx.receivedBytes) {
        appendLog("Backlog chunk gap; expected=${rx.receivedBytes} got=${offset}")
        replyOffset = rx.receivedBytes
        replyLen = 0
      } else {
        if (rx.receivedBytes == 0L && offset == 0L) {
          appendLog("Backlog blob first chunk received")
        }
        try {
          rx.out.write(payload, 14, chunkLen)
        } catch (e: Exception) {
          appendLog("Backlog chunk write failed: ${e.message}")
          sendBacklogBlobChunkReply(sessionId, rx.receivedBytes, 0, accepted = false)
          resetBacklogBlobReceiver(discardFile = true)
          recoverBacklogBlobStream("chunk write failed")
          return
        }
        rx.receivedBytes += chunkLen.toLong()
        replyAccept = true
        if (rx.receivedBytes % (64L * 1024L) < chunkLen.toLong() || rx.receivedBytes == rx.totalBytes) {
          appendLog("Backlog blob progress ${rx.receivedBytes}/${rx.totalBytes}B")
        }
        val nowMs = System.currentTimeMillis()
        val bytesDelta = rx.receivedBytes - blobProgressUiLastBytes
        val dueToTime = (nowMs - blobProgressUiLastMs) >= BLOB_PROGRESS_UI_INTERVAL_MS
        val dueToBytes = bytesDelta >= BLOB_PROGRESS_UI_STEP_BYTES
        val done = rx.receivedBytes == rx.totalBytes
        if (dueToTime || dueToBytes || done) {
          blobProgressUiLastMs = nowMs
          blobProgressUiLastBytes = rx.receivedBytes
          shouldUpdateUi = true
          uiUpdateSessionId = rx.sessionId
          uiUpdateBytes = rx.receivedBytes
          uiUpdateTotal = rx.totalBytes
        }
      }
    }

    sendBacklogBlobChunkReply(
      sessionId = sessionId,
      chunkOffset = replyOffset,
      chunkLen = replyLen,
      accepted = replyAccept,
    )

    if (shouldUpdateUi) {
      _state.update {
        it.copy(
          blobActive = true,
          blobSessionId = uiUpdateSessionId,
          blobBytesSent = uiUpdateBytes,
          blobBytesTotal = uiUpdateTotal,
        )
      }
    }
  }

  private fun sendBacklogBlobChunkReply(
    sessionId: Long,
    chunkOffset: Long,
    chunkLen: Int,
    accepted: Boolean,
  ) {
    if (!bleClient.isConnected()) {
      return
    }
    var ok = false
    repeat(3) {
      ok = bleClient.sendBacklogBlobChunkReply(
        sessionId = sessionId,
        chunkOffset = chunkOffset,
        chunkLen = chunkLen.coerceIn(0, 0xFFFF),
        accepted = accepted,
      )
      if (ok) return
    }
    appendLog(
      "Backlog chunk ${if (accepted) "ACK" else "NAK"} send failed off=$chunkOffset len=$chunkLen",
    )
  }

  private fun readU16Le(bytes: ByteArray, offset: Int): Int =
    (bytes[offset].toInt() and 0xFF) or ((bytes[offset + 1].toInt() and 0xFF) shl 8)

  private fun readU32Le(bytes: ByteArray, offset: Int): Long =
    ((bytes[offset].toLong() and 0xFFL) or
      ((bytes[offset + 1].toLong() and 0xFFL) shl 8) or
      ((bytes[offset + 2].toLong() and 0xFFL) shl 16) or
      ((bytes[offset + 3].toLong() and 0xFFL) shl 24))

  private fun readU64Le(bytes: ByteArray, offset: Int): Long =
    (readU32Le(bytes, offset) or (readU32Le(bytes, offset + 4) shl 32))

  private fun handleBacklogBlobDoneFrame(payload: ByteArray) {
    val done: BacklogBlobDonePayload = try {
      WgProtocol.decodeBacklogBlobDonePayload(payload)
    } catch (e: Exception) {
      appendLog("Backlog done decode error: ${e.message}")
      return
    }

    var nakOffset = 0L
    var shouldNak = false
    val completed: BacklogBlobReceiver? = synchronized(this) {
      val rx = backlogBlobRx
      if (rx == null) {
        appendLog("Backlog done ignored: no active session")
        shouldNak = true
        nakOffset = 0L
        null
      } else if (rx.sessionId != done.sessionId) {
        appendLog("Backlog done session mismatch")
        shouldNak = true
        nakOffset = rx.receivedBytes
        null
      } else if (rx.receivedBytes != rx.totalBytes || done.totalBytes != rx.totalBytes) {
        appendLog("Backlog done size mismatch")
        shouldNak = true
        nakOffset = rx.receivedBytes
        null
      } else {
        try {
          rx.out.flush()
        } catch (_: Exception) {}
        try {
          rx.out.close()
        } catch (_: Exception) {}
        backlogBlobRx = null
        rx
      }
    }
    if (completed == null) {
      if (shouldNak) {
        sendBacklogBlobChunkReply(done.sessionId, nakOffset, 0, accepted = false)
      }
      return
    }

    appendLog(
      "Backlog blob received session 0x${java.lang.Long.toUnsignedString(done.sessionId, 16)} (${done.totalBytes}B), importing...",
    )
    _state.update {
      it.copy(
        blobActive = false,
        blobSessionId = done.sessionId,
        blobBytesSent = done.totalBytes,
        blobBytesTotal = done.totalBytes,
      )
    }
    importBacklogBlobSessionAsync(
      file = completed.file,
      sessionId = done.sessionId,
      writtenSeq = if (done.writtenSeq > 0L) done.writtenSeq else completed.writtenSeq,
    )
  }

  private fun importBacklogBlobSessionAsync(
    file: File,
    sessionId: Long,
    writtenSeq: Long,
  ) {
    scope.launch {
      val result = parseBacklogBlobFile(file, sessionId)
      val targetSeq = if (writtenSeq > 0L) writtenSeq else result.highestSeq
      val importComplete =
        targetSeq > 0L &&
          result.badBlocks == 0 &&
          !result.gapDetected &&
          result.highestSeq >= targetSeq
      appendLog(
        "Backlog import done sid=0x${java.lang.Long.toUnsignedString(sessionId, 16)} records=${result.records} highest_seq=${result.highestSeq} bad_blocks=${result.badBlocks} gap=${result.gapDetected}",
      )
      if (importComplete) {
        queueBlobSessionAck(sessionId, targetSeq, force = true)
      } else {
        appendLog(
          "Backlog import incomplete sid=0x${java.lang.Long.toUnsignedString(sessionId, 16)}; withholding ACK and retrying",
        )
        recoverBacklogBlobStream("import integrity check failed")
      }
      try {
        file.delete()
      } catch (_: Exception) {}
      if (controlLinkReadySilent()) {
        bleClient.requestStatus()
      }
    }
  }

  private fun queueBlobSessionAck(sessionId: Long, ackSeq: Long, force: Boolean) {
    if (sessionId == 0L || ackSeq <= 0L) {
      return
    }
    synchronized(ackLock) {
      val tracker = sessionAcks.getOrPut(sessionId) { SessionAckTracker() }
      if (ackSeq > tracker.highestContiguous) {
        tracker.highestContiguous = ackSeq
      }
      maybeSendReplayAckLocked(sessionId, tracker, force)
    }
  }

  private fun recoverBacklogBlobStream(reason: String) {
    if (!_state.value.downloadBacklogActive) {
      return
    }
    if (!controlLinkReadySilent()) {
      appendLog("Backlog recovery skipped: control link unavailable")
      return
    }
    synchronized(backlogRecoveryLock) {
      if (backlogBlobRecoveryInFlight) {
        return
      }
      backlogBlobRecoveryInFlight = true
    }
    appendLog("Backlog blob desync ($reason); restarting stream")
    scope.launch {
      try {
        val disableOk = bleClient.setBacklogBlobEnabled(false)
        if (!disableOk) {
          appendLog("Backlog recovery failed: disable command failed")
          return@launch
        }
        delay(120)
        val enableOk = bleClient.setBacklogBlobEnabled(true)
        if (!enableOk) {
          appendLog("Backlog recovery failed: enable command failed")
          return@launch
        }
        delay(80)
        bleClient.requestStatus()
      } finally {
        synchronized(backlogRecoveryLock) {
          backlogBlobRecoveryInFlight = false
        }
      }
    }
  }

  private suspend fun runWifiDumpTransfer() {
    var success = false
    var cancelled = false
    var runId: Long = 0L
    try {
      val transferOk = withWifiDumpNetwork { network ->
        val manifest = fetchWifiDumpManifest(network)
        if (manifest == null) {
          appendLog("Backlog download failed: Wi-Fi dump manifest unavailable")
          return@withWifiDumpNetwork false
        }
        runId = manifest.runId
        if (manifest.sessions.isEmpty()) {
          appendLog("Backlog download: no pending sessions")
          return@withWifiDumpNetwork true
        }

        val dumpDirName = "wifi-dump-${SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())}"
        val dumpDir = File(getExternalFilesDir(null), dumpDirName)
        dumpDir.mkdirs()

        appendLog(
          "Backlog Wi-Fi dump manifest run=0x${toHex64(runId)} sessions=${manifest.sessions.size}",
        )

        for ((index, session) in manifest.sessions.withIndex()) {
          if (!currentCoroutineContext().isActive) {
            cancelled = true
            return@withWifiDumpNetwork false
          }
          val sidHex = toHex64(session.sessionId)
          appendLog(
            "Backlog Wi-Fi session ${index + 1}/${manifest.sessions.size} sid=0x$sidHex dat=${session.datBytes}B gpx=${session.gpxBytes}B",
          )

          val datFile = File(dumpDir, "session-$sidHex.dat")
          _state.update {
            it.copy(
              blobActive = true,
              blobSessionId = session.sessionId,
              blobBytesSent = 0L,
              blobBytesTotal = session.datBytes,
            )
          }

          val datOk = downloadWifiDumpFile(
            network = network,
            sessionId = session.sessionId,
            type = "dat",
            expectedBytes = session.datBytes,
            outFile = datFile,
          ) { downloaded ->
            _state.update { state ->
              state.copy(
                blobActive = true,
                blobSessionId = session.sessionId,
                blobBytesSent = downloaded.coerceAtLeast(0L),
                blobBytesTotal = session.datBytes,
              )
            }
          }
          if (!datOk) {
            appendLog("Backlog Wi-Fi session failed sid=0x$sidHex (dat download)")
            return@withWifiDumpNetwork false
          }

          val result = parseBacklogBlobFile(datFile, session.sessionId)
          val targetSeq = if (session.writtenSeq > 0L) session.writtenSeq else result.highestSeq
          val importComplete =
            targetSeq > 0L &&
              result.badBlocks == 0 &&
              !result.gapDetected &&
              result.highestSeq >= targetSeq
          appendLog(
            "Backlog Wi-Fi import sid=0x$sidHex records=${result.records} highest_seq=${result.highestSeq} bad_blocks=${result.badBlocks} gap=${result.gapDetected}",
          )
          if (!importComplete) {
            appendLog("Backlog Wi-Fi import failed integrity check sid=0x$sidHex")
            return@withWifiDumpNetwork false
          }
          try {
            datFile.delete()
          } catch (_: Exception) {}

          if (session.gpxBytes > 0L) {
            val gpxFile = File(dumpDir, "session-$sidHex.gpx")
            val gpxOk = downloadWifiDumpFile(
              network = network,
              sessionId = session.sessionId,
              type = "gpx",
              expectedBytes = session.gpxBytes,
              outFile = gpxFile,
            )
            if (!gpxOk) {
              appendLog("Backlog Wi-Fi session sid=0x$sidHex gpx download failed")
              return@withWifiDumpNetwork false
            }
            try {
              gpxFile.delete()
            } catch (_: Exception) {}
          }
        }
        true
      } ?: false
      if (!transferOk) {
        return
      }

      if (!bleClient.commitWifiDump(runId)) {
        appendLog("Backlog commit failed (run=0x${toHex64(runId)})")
        return
      }
      bleClient.requestStatus()
      success = true
      appendLog("Backlog Wi-Fi download complete")
    } catch (e: kotlinx.coroutines.CancellationException) {
      cancelled = true
    } catch (e: Exception) {
      appendLog("Backlog Wi-Fi transfer error: ${e.message}")
    } finally {
      wifiDumpInProgress = false
      wifiDumpJob = null
      downloadBacklogStartMs = 0L
      resetBacklogBlobReceiver(discardFile = true)
      if (controlLinkReadySilent()) {
        bleClient.setWifiDumpEnabled(false)
        bleClient.setBacklogBlobEnabled(false)
        bleClient.requestStatus()
      }
      _state.update {
        it.copy(
          downloadBacklogActive = false,
          blobActive = false,
          blobSessionId = 0L,
          blobBytesSent = 0L,
          blobBytesTotal = 0L,
        )
      }
      val shouldAutoStopCsv = downloadBacklogAutoStop
      downloadBacklogAutoStop = false
      if (shouldAutoStopCsv && csvLogger.isActive) {
        stopLogging()
      }
      if (!success && !cancelled) {
        appendLog(
          if (runId != 0L) {
            "Backlog Wi-Fi download ended with errors (run=0x${toHex64(runId)})"
          } else {
            "Backlog Wi-Fi download ended with errors"
          },
        )
      } else if (cancelled) {
        appendLog("Backlog download stopped")
      }
    }
  }

  private fun toHex64(value: Long): String =
    java.lang.Long.toUnsignedString(value, 16).uppercase(Locale.US).padStart(16, '0')

  @SuppressLint("MissingPermission")
  private suspend fun <T> withWifiDumpNetwork(block: suspend (Network) -> T?): T? {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
      appendLog("Backlog Wi-Fi dump requires Android 10+")
      return null
    }
    if (ContextCompat.checkSelfPermission(
        this,
        Manifest.permission.ACCESS_FINE_LOCATION,
      ) != PackageManager.PERMISSION_GRANTED) {
      appendLog("Backlog Wi-Fi dump blocked: ACCESS_FINE_LOCATION permission missing")
      return null
    }
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
      ContextCompat.checkSelfPermission(
        this,
        Manifest.permission.NEARBY_WIFI_DEVICES,
      ) != PackageManager.PERMISSION_GRANTED) {
      appendLog("Backlog Wi-Fi dump blocked: NEARBY_WIFI_DEVICES permission missing")
      return null
    }
    val cm = getSystemService(ConnectivityManager::class.java)
    if (cm == null) {
      appendLog("Backlog Wi-Fi dump failed: ConnectivityManager unavailable")
      return null
    }
    val request = NetworkRequest.Builder()
      .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
      .setNetworkSpecifier(
        WifiNetworkSpecifier.Builder()
          .setSsid(WIFI_DUMP_AP_SSID)
          .build(),
      )
      .build()

    val acquired = withTimeoutOrNull(WIFI_DUMP_NETWORK_WAIT_MS) {
      suspendCancellableCoroutine<Pair<Network, ConnectivityManager.NetworkCallback>?> { cont ->
        var completed = false
        lateinit var callback: ConnectivityManager.NetworkCallback
        fun finish(value: Pair<Network, ConnectivityManager.NetworkCallback>?) {
          if (completed) return
          completed = true
          cont.resume(value)
        }
        callback = object : ConnectivityManager.NetworkCallback() {
          override fun onAvailable(network: Network) {
            appendLog("Backlog Wi-Fi network acquired: $WIFI_DUMP_AP_SSID")
            finish(network to this)
          }

          override fun onUnavailable() {
            appendLog("Backlog Wi-Fi network unavailable: $WIFI_DUMP_AP_SSID")
            try {
              cm.unregisterNetworkCallback(this)
            } catch (_: Exception) {}
            finish(null)
          }

          override fun onLost(network: Network) {
            if (!completed) {
              appendLog("Backlog Wi-Fi network lost before transfer")
              try {
                cm.unregisterNetworkCallback(this)
              } catch (_: Exception) {}
              finish(null)
            }
          }
        }
        try {
          cm.requestNetwork(request, callback)
        } catch (e: Exception) {
          appendLog("Backlog Wi-Fi request failed: ${e.javaClass.simpleName}${if (e.message.isNullOrBlank()) "" else " (${e.message})"}")
          finish(null)
          return@suspendCancellableCoroutine
        }
        cont.invokeOnCancellation {
          try {
            cm.unregisterNetworkCallback(callback)
          } catch (_: Exception) {}
        }
      }
    }
    if (acquired == null) {
      appendLog("Backlog Wi-Fi request timed out (${WIFI_DUMP_NETWORK_WAIT_MS}ms)")
      return null
    }

    val network = acquired.first
    val callback = acquired.second
    val previous = cm.boundNetworkForProcess
    val bound = try {
      cm.bindProcessToNetwork(network)
    } catch (_: Exception) {
      false
    }
    if (!bound) {
      appendLog("Backlog Wi-Fi bindProcessToNetwork failed")
      try {
        cm.unregisterNetworkCallback(callback)
      } catch (_: Exception) {}
      return null
    }

    return try {
      block(network)
    } finally {
      try {
        cm.bindProcessToNetwork(previous)
      } catch (_: Exception) {}
      try {
        cm.unregisterNetworkCallback(callback)
      } catch (_: Exception) {}
    }
  }

  private suspend fun fetchWifiDumpManifest(network: Network): WifiDumpManifest? =
    withContext(Dispatchers.IO) {
      val url = URL("http://$WIFI_DUMP_AP_HOST$WIFI_DUMP_MANIFEST_PATH")
      val conn = (network.openConnection(url) as? HttpURLConnection) ?: return@withContext null
      try {
        conn.requestMethod = "GET"
        conn.connectTimeout = WIFI_DUMP_HTTP_CONNECT_TIMEOUT_MS
        conn.readTimeout = WIFI_DUMP_HTTP_READ_TIMEOUT_MS
        conn.setRequestProperty("Connection", "close")
        val code = conn.responseCode
        if (code != HttpURLConnection.HTTP_OK) {
          return@withContext null
        }
        val body = conn.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
        val root = JSONObject(body)
        val runId = root.optString("run_id").trim()
        if (runId.isEmpty()) {
          return@withContext null
        }
        val runValue = try {
          runId.toULong(16).toLong()
        } catch (_: Exception) {
          return@withContext null
        }
        val sessionsJson = root.optJSONArray("sessions") ?: return@withContext WifiDumpManifest(
          runId = runValue,
          sessions = emptyList(),
        )
        val sessions = mutableListOf<WifiDumpSession>()
        for (i in 0 until sessionsJson.length()) {
          val item = sessionsJson.optJSONObject(i) ?: continue
          val sidHex = item.optString("sid").trim()
          if (sidHex.isEmpty()) {
            continue
          }
          val sid = try {
            sidHex.toULong(16).toLong()
          } catch (_: Exception) {
            continue
          }
          val datBytes = item.optLong("dat_bytes", 0L).coerceAtLeast(0L)
          if (datBytes <= 0L) {
            continue
          }
          sessions += WifiDumpSession(
            sessionId = sid,
            datBytes = datBytes,
            gpxBytes = item.optLong("gpx_bytes", 0L).coerceAtLeast(0L),
            writtenSeq = item.optLong("written_seq", 0L).coerceAtLeast(0L),
            ackedSeq = item.optLong("acked_seq", 0L).coerceAtLeast(0L),
          )
        }
        WifiDumpManifest(runId = runValue, sessions = sessions)
      } finally {
        conn.disconnect()
      }
    }

  private suspend fun downloadWifiDumpFile(
    network: Network,
    sessionId: Long,
    type: String,
    expectedBytes: Long,
    outFile: File,
    onProgress: ((Long) -> Unit)? = null,
  ): Boolean = withContext(Dispatchers.IO) {
    if (expectedBytes <= 0L) {
      return@withContext false
    }
    outFile.parentFile?.mkdirs()
    val tmpFile = File(outFile.parentFile, "${outFile.name}.part")
    if (tmpFile.exists()) {
      tmpFile.delete()
    }

    val sidHex = toHex64(sessionId)
    val url = URL("http://$WIFI_DUMP_AP_HOST$WIFI_DUMP_FILE_PATH?sid=$sidHex&type=$type")
    val conn = (network.openConnection(url) as? HttpURLConnection) ?: return@withContext false
    try {
      conn.requestMethod = "GET"
      conn.connectTimeout = WIFI_DUMP_HTTP_CONNECT_TIMEOUT_MS
      conn.readTimeout = WIFI_DUMP_HTTP_READ_TIMEOUT_MS
      conn.setRequestProperty("Connection", "close")
      val code = conn.responseCode
      if (code != HttpURLConnection.HTTP_OK && code != HttpURLConnection.HTTP_PARTIAL) {
        return@withContext false
      }
      val input = conn.inputStream ?: return@withContext false
      var written = 0L
      var lastUiMs = 0L
      input.use { stream ->
        BufferedInputStream(stream, 64 * 1024).use { bin ->
          FileOutputStream(tmpFile, false).use { fout ->
            BufferedOutputStream(fout, 64 * 1024).use { bout ->
              val buffer = ByteArray(64 * 1024)
              while (true) {
                val read = bin.read(buffer)
                if (read < 0) {
                  break
                }
                if (read == 0) {
                  continue
                }
                bout.write(buffer, 0, read)
                written += read.toLong()
                if (onProgress != null) {
                  val now = System.currentTimeMillis()
                  if (written == expectedBytes ||
                    lastUiMs == 0L ||
                    now - lastUiMs >= WIFI_DUMP_PROGRESS_UI_INTERVAL_MS) {
                    lastUiMs = now
                    onProgress(written)
                  }
                }
              }
              bout.flush()
            }
          }
        }
      }
      if (written != expectedBytes) {
        tmpFile.delete()
        return@withContext false
      }
      if (outFile.exists()) {
        outFile.delete()
      }
      if (!tmpFile.renameTo(outFile)) {
        return@withContext false
      }
      true
    } catch (_: Exception) {
      tmpFile.delete()
      false
    } finally {
      conn.disconnect()
    }
  }

  private fun parseBacklogBlobFile(file: File, expectedSessionId: Long): BacklogImportResult {
    if (!file.exists()) {
      return BacklogImportResult(records = 0, highestSeq = 0L, gapDetected = false, badBlocks = 1)
    }

    var records = 0
    var highestSeq = 0L
    var gapDetected = false
    var badBlocks = 0
    var expectedNextSeq = 0L
    var hasExpectedNextSeq = false
    var backlogLastGpxSample: GpxTrackSample? = null
    val block = ByteArray(BLOB_BLOCK_SIZE)
    var backlogGpxLogger: GpxTrackLogger? = null

    if (gpxEnabled) {
      try {
        backlogGpxLogger = GpxTrackLogger(applicationContext)
        val session = backlogGpxLogger.startSession(expectedSessionId, "backlog")
        _state.update {
          it.copy(
            gpxEnabled = gpxEnabled,
            gpxLogging = true,
            gpxPath = session.displayPath,
            gpxPointCount = 0L,
          )
        }
        appendLog("Backlog GPX started: ${session.displayPath}")
      } catch (e: Exception) {
        backlogGpxLogger = null
        appendLog("Backlog GPX start failed: ${e.message}")
      }
    }

    try {
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
              blockValid = false
              break
            }
            appendCsvFromBacklogSighting(sighting, System.currentTimeMillis())

            if (backlogGpxLogger != null && sighting.gpsValid &&
                validLatLon(sighting.gpsLatE7, sighting.gpsLonE7)) {
              val sample = GpxTrackSample(
                lat = sighting.gpsLatE7 / 10_000_000.0,
                lon = sighting.gpsLonE7 / 10_000_000.0,
                unixTimeMs = if (sighting.gpsUnixTimeS > 0L) sighting.gpsUnixTimeS * 1000L else System.currentTimeMillis(),
              )
              if (GpxTrackSampling.shouldEmit(
                  previous = backlogLastGpxSample,
                  candidate = sample,
                  minElapsedMs = GPX_MIN_ELAPSED_MS,
                  minDistanceMeters = GPX_MIN_DISTANCE_METERS,
                )) {
                backlogGpxLogger.appendTrackPoint(
                  sample = sample,
                  altitudeMeters = sighting.gpsAltMm / 1000.0,
                  hdop = null,
                  satCount = null,
                )
                backlogLastGpxSample = sample
              }
            }

            records += 1

            val seq = if (sighting.recordSeq > 0L) sighting.recordSeq else firstSeq + index
            if (!hasExpectedNextSeq) {
              expectedNextSeq = seq
              hasExpectedNextSeq = true
            }
            if (seq == expectedNextSeq) {
              expectedNextSeq += 1L
            } else if (seq > expectedNextSeq) {
              gapDetected = true
            }
            if (seq > highestSeq) {
              highestSeq = seq
            }
          }
          if (payloadPos != payloadEnd) {
            blockValid = false
          }
          if (!blockValid) {
            badBlocks += 1
          }
        }
      }
    } finally {
      if (backlogGpxLogger != null) {
        try {
          val pointCount = backlogGpxLogger.currentPointCount()
          val path = backlogGpxLogger.currentPath()
          backlogGpxLogger.stopSession(finalize = true)
          _state.update {
            it.copy(
              gpxEnabled = gpxEnabled,
              gpxLogging = gpxLogger.isActive,
              gpxPath = path ?: it.gpxPath,
              gpxPointCount = pointCount,
            )
          }
          appendLog("Backlog GPX complete (${pointCount} points)")
        } catch (e: Exception) {
          appendLog("Backlog GPX finalize failed: ${e.message}")
        }
      } else {
        _state.update { it.copy(gpxLogging = gpxLogger.isActive) }
      }
    }

    return BacklogImportResult(
      records = records,
      highestSeq = highestSeq,
      gapDetected = gapDetected,
      badBlocks = badBlocks,
    )
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
    val nowMs = System.currentTimeMillis()
    lastPhoneLocation = location
    lastPhoneFixMs = nowMs
    lastPhoneSpeedMmps = if (location.hasSpeed()) (location.speed * 1000f).roundToInt() else 0
    val speedKmh = speedMmpsToKmh(lastPhoneSpeedMmps)
    _state.update { current ->
      current.copy(
        phoneGpsAgeS = 0,
        phoneGpsAccuracyM = location.accuracy,
        phoneSpeedKmh = speedKmh,
      )
    }

    if (gpxEnabled && liveGpxSessionId != 0L && gpxLogger.isActive) {
      val espAgeMs = if (lastEspFixMs <= 0L) Long.MAX_VALUE else nowMs - lastEspFixMs
      val hasFreshEsp = lastEspGps?.valid == true && espAgeMs <= MAX_ESP_GPS_AGE_MS_FOR_LOGGING
      if (!hasFreshEsp) {
        val sample = gpxSampleFromPhoneLocation(location, nowMs)
        if (sample != null) {
          maybeAppendLiveGpxPoint(
            sample = sample,
            altitudeMeters = if (location.hasAltitude()) location.altitude else null,
            hdop = null,
            satCount = null,
          )
        }
      }
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

  private fun startLiveGpxSession(sessionId: Long) {
    if (!gpxEnabled || sessionId == 0L) {
      return
    }
    if (liveGpxSessionId == sessionId && gpxLogger.isActive) {
      return
    }
    stopLiveGpxSession(reason = null)
    try {
      val session = gpxLogger.startSession(sessionId = sessionId, sourceTag = "live")
      liveGpxSessionId = sessionId
      lastLiveGpxSample = null
      _state.update {
        it.copy(
          gpxEnabled = gpxEnabled,
          gpxLogging = true,
          gpxPath = session.displayPath,
          gpxPointCount = 0L,
        )
      }
      appendLog("GPX logging started: ${session.displayPath}")
    } catch (e: Exception) {
      liveGpxSessionId = 0L
      lastLiveGpxSample = null
      _state.update { it.copy(gpxLogging = false) }
      appendLog("GPX start failed: ${e.message}")
    }
  }

  private fun stopLiveGpxSession(reason: String?) {
    val wasActive = gpxLogger.isActive
    if (wasActive) {
      try {
        gpxLogger.stopSession(finalize = true)
      } catch (_: Exception) {}
    }
    if (wasActive && reason != null) {
      appendLog("GPX logging stopped ($reason)")
    }
    liveGpxSessionId = 0L
    lastLiveGpxSample = null
    _state.update {
      it.copy(
        gpxLogging = false,
      )
    }
  }

  private fun syncLiveGpxSession(status: StatusPayload) {
    if (!gpxEnabled) {
      stopLiveGpxSession(reason = null)
      return
    }
    if (status.scanning && status.sessionOpen && status.sessionId != 0L) {
      startLiveGpxSession(status.sessionId)
      return
    }
    if (liveGpxSessionId != 0L || gpxLogger.isActive) {
      stopLiveGpxSession(reason = "session closed")
    }
  }

  private fun maybeAppendLiveGpxPoint(
    sample: GpxTrackSample,
    altitudeMeters: Double? = null,
    hdop: Double? = null,
    satCount: Int? = null,
  ) {
    if (!gpxEnabled || liveGpxSessionId == 0L || !gpxLogger.isActive) {
      return
    }
    if (!GpxTrackSampling.shouldEmit(
        previous = lastLiveGpxSample,
        candidate = sample,
        minElapsedMs = GPX_MIN_ELAPSED_MS,
        minDistanceMeters = GPX_MIN_DISTANCE_METERS,
      )) {
      return
    }
    try {
      gpxLogger.appendTrackPoint(
        sample = sample,
        altitudeMeters = altitudeMeters,
        hdop = hdop,
        satCount = satCount,
      )
      lastLiveGpxSample = sample
      _state.update {
        it.copy(
          gpxLogging = true,
          gpxPath = gpxLogger.currentPath() ?: it.gpxPath,
          gpxPointCount = gpxLogger.currentPointCount(),
        )
      }
    } catch (e: Exception) {
      appendLog("GPX write failed: ${e.message}")
    }
  }

  private fun validLatLon(latE7: Int, lonE7: Int): Boolean {
    return latE7 in -900_000_000..900_000_000 && lonE7 in -1_800_000_000..1_800_000_000
  }

  private fun gpxSampleFromEspGps(gps: EspGpsPayload, nowMs: Long): GpxTrackSample? {
    if (!gps.valid) return null
    if (!validLatLon(gps.latE7, gps.lonE7)) return null
    val sampleTs = if (gps.unixTimeS > 0L) gps.unixTimeS * 1000L else nowMs
    return GpxTrackSample(
      lat = gps.latE7 / 10_000_000.0,
      lon = gps.lonE7 / 10_000_000.0,
      unixTimeMs = sampleTs,
    )
  }

  private fun gpxSampleFromPhoneLocation(location: Location, nowMs: Long): GpxTrackSample? {
    val latE7 = (location.latitude * 10_000_000.0).roundToInt()
    val lonE7 = (location.longitude * 10_000_000.0).roundToInt()
    if (!validLatLon(latE7, lonE7)) return null
    return GpxTrackSample(
      lat = location.latitude,
      lon = location.longitude,
      unixTimeMs = nowMs,
    )
  }

  private fun trackReplayRecord(sessionId: Long, recordSeq: Long): Boolean {
    if (sessionId == 0L || recordSeq <= 0L) return true
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
