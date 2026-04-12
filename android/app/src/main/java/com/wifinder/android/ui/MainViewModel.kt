package com.wifinder.android.ui

import android.app.Application
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.ServiceConnection
import android.os.Build
import android.os.IBinder
import com.wifinder.android.model.WifiBand
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.wifinder.android.service.ScannerService.Companion.normalizeGpsNavMode
import com.wifinder.android.service.ScannerService
import com.wifinder.android.service.ServiceState
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

class MainViewModel(application: Application) : AndroidViewModel(application) {

  companion object {
    private const val MIN_HOP_MS = 50
    private const val MAX_HOP_MS = 2000
    private const val MIN_VISIBLE_TIMEOUT_SEC = 5
    private const val MAX_VISIBLE_TIMEOUT_SEC = 300
    private const val PREFS_NAME = "wifinder_settings"
    private const val PREF_VISIBLE_TIMEOUT_SEC = "visible_timeout_sec"
    private const val PREF_AUTO_HOP_ENABLED = "auto_hop_enabled"
    private const val PREF_AUTO_HOP_BASE_MS = "auto_hop_base_ms"
    private const val PREF_CHANNEL_MASK_INPUT = "channel_mask_input"
    private const val PREF_LOCAL_CHANNEL_MASK_INPUT = "local_channel_mask_input"
    private const val PREF_NODE_CHANNEL_MASK24_INPUT = "node_channel_mask24_input"
    private const val PREF_NODE_CHANNEL_MASK5_INPUT = "node_channel_mask5_input"
    private const val PREF_GPS_NAV_MODE = "gps_nav_mode"
    private const val PREF_GPX_ENABLED = "gpx_enabled"
    private const val PREF_DASHBOARD_MODE = "dashboard_mode"
  }

  private val prefs: SharedPreferences =
    application.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
  private val stateFlow = MutableStateFlow(loadInitialState())
  val state: StateFlow<AppUiState> = stateFlow.asStateFlow()

  private var service: ScannerService? = null
  private var serviceCollector: Job? = null
  private var bound = false

  private fun loadInitialState(): AppUiState {
    val visibleTimeout = prefs
      .getInt(PREF_VISIBLE_TIMEOUT_SEC, 25)
      .coerceIn(MIN_VISIBLE_TIMEOUT_SEC, MAX_VISIBLE_TIMEOUT_SEC)
    val hopInput = prefs
      .getInt(PREF_AUTO_HOP_BASE_MS, 250)
      .coerceIn(MIN_HOP_MS, MAX_HOP_MS)
    val autoHopEnabled = prefs.getBoolean(PREF_AUTO_HOP_ENABLED, false)
    val gpsNavMode = normalizeGpsNavMode(prefs.getInt(PREF_GPS_NAV_MODE, 0))
    val gpxEnabled = prefs.getBoolean(PREF_GPX_ENABLED, true)
    val dashboardMode = prefs.getBoolean(PREF_DASHBOARD_MODE, false)
    val legacyMask = prefs.getInt(PREF_CHANNEL_MASK_INPUT, 0x1FFF) and 0x1FFF
    val localMask = prefs.getInt(PREF_LOCAL_CHANNEL_MASK_INPUT, legacyMask) and 0x1FFF
    val nodeMask24 = prefs.getInt(PREF_NODE_CHANNEL_MASK24_INPUT, 0x0000) and 0x1FFF
    val rawNodeMask5 =
      if (prefs.contains(PREF_NODE_CHANNEL_MASK5_INPUT)) {
        prefs.getLong(PREF_NODE_CHANNEL_MASK5_INPUT, 0L)
      } else {
        WifiBand.NODE_CHANNELS_5_GHZ_ALL_MASK
      }
    val nodeMask5 = WifiBand.sanitizeNode5GhzMask(rawNodeMask5)
    val normalizedLocalMask = if (localMask == 0) 0x1FFF else localMask
    return AppUiState(
      dashboardMode = dashboardMode,
      visibleTimeoutSec = visibleTimeout,
      hopInputMs = hopInput,
      autoHopBaseMs = hopInput,
      autoHopAppliedMs = hopInput,
      autoHopEnabled = autoHopEnabled,
      gpsNavMode = gpsNavMode,
      gpxEnabled = gpxEnabled,
      localChannelMaskInput = normalizedLocalMask,
      nodeChannelMask24Input = nodeMask24,
      nodeChannelMask5GhzInput = nodeMask5,
      channelMask = normalizedLocalMask,
      localChannelMask = normalizedLocalMask,
      nodeChannelMask24 = nodeMask24,
      nodeChannelMask5Ghz = nodeMask5,
      nodeChannelMask = nodeMask24,
    )
  }

  private fun persistInt(key: String, value: Int) {
    prefs.edit().putInt(key, value).apply()
  }

  private fun persistBoolean(key: String, value: Boolean) {
    prefs.edit().putBoolean(key, value).apply()
  }

  private fun persistLong(key: String, value: Long) {
    prefs.edit().putLong(key, value).apply()
  }

  private val connection = object : ServiceConnection {
    override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
      val svc = (binder as ScannerService.LocalBinder).service
      service = svc
      bound = true
      stateFlow.update { it.copy(serviceRunning = true) }
      serviceCollector = viewModelScope.launch {
        svc.state.collect { serviceState ->
          mergeServiceState(serviceState)
        }
      }
    }

    override fun onServiceDisconnected(name: ComponentName?) {
      service = null
      bound = false
      serviceCollector?.cancel()
      serviceCollector = null
      stateFlow.update { it.copy(serviceRunning = false) }
    }
  }

  fun setPermissionsGranted(granted: Boolean) {
    stateFlow.update { it.copy(permissionsGranted = granted) }
  }

  fun toggleSection(section: String) {
    stateFlow.update { current ->
      val next = current.expandedSections.toMutableSet()
      if (section in next) next.remove(section) else next.add(section)
      current.copy(expandedSections = next)
    }
  }

  fun toggleDashboardMode() {
    val next = !stateFlow.value.dashboardMode
    stateFlow.update { it.copy(dashboardMode = next) }
    persistBoolean(PREF_DASHBOARD_MODE, next)
  }

  // ── Service lifecycle ──────────────────────────────────────

  fun connect() {
    if (!stateFlow.value.permissionsGranted) return
    val ctx = getApplication<Application>()
    val intent = Intent(ctx, ScannerService::class.java)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
      ctx.startForegroundService(intent)
    } else {
      ctx.startService(intent)
    }
    ctx.bindService(intent, connection, Context.BIND_AUTO_CREATE)

    // The actual BLE connect happens after binding completes
    viewModelScope.launch {
      // Wait for binding
      var tries = 0
      while (service == null && tries < 50) {
        kotlinx.coroutines.delay(100)
        tries++
      }
      service?.doConnect()
    }
  }

  fun disconnect() {
    service?.disconnectAndStop()
    unbindSafely()
    stateFlow.update { it.copy(serviceRunning = false, connected = false, scanning = false) }
  }

  private fun unbindSafely() {
    if (bound) {
      try {
        getApplication<Application>().unbindService(connection)
      } catch (_: Exception) {}
      bound = false
    }
    serviceCollector?.cancel()
    serviceCollector = null
    service = null
  }

  // ── Delegated commands ────────────────────────────────────

  fun sendStart() { service?.sendStart() }
  fun sendStop() { service?.sendStop() }
  fun requestStatus() { service?.requestStatus() }
  fun requestSnapshot() { service?.requestSnapshot() }
  fun clearSightings() { service?.clearSightings() }
  fun clearEspStorage() { service?.clearStorageSessions() }

  fun setHopInput(value: Int) {
    val clamped = value.coerceIn(MIN_HOP_MS, MAX_HOP_MS)
    stateFlow.update { it.copy(hopInputMs = clamped, hopInputDirty = true) }
    persistInt(PREF_AUTO_HOP_BASE_MS, clamped)
  }

  fun applyHopMs() {
    val hop = stateFlow.value.hopInputMs.coerceIn(MIN_HOP_MS, MAX_HOP_MS)
    val ok = if (stateFlow.value.autoHopEnabled) {
      service?.setAutoHopBaseMs(hop) == true
    } else {
      service?.setHopMs(hop) == true
    }
    if (ok) {
      stateFlow.update { it.copy(hopInputDirty = false) }
    }
  }

  fun setAutoHopEnabled(enabled: Boolean) {
    val base = stateFlow.value.hopInputMs.coerceIn(MIN_HOP_MS, MAX_HOP_MS)
    if (service?.setAutoHopEnabled(enabled, base) == true) {
      stateFlow.update { it.copy(hopInputDirty = false) }
    }
    persistBoolean(PREF_AUTO_HOP_ENABLED, enabled)
    persistInt(PREF_AUTO_HOP_BASE_MS, base)
  }

  fun toggleMaster24Channel(channel: Int, enabled: Boolean) {
    if (channel !in 1..13) return
    val bit = 1 shl (channel - 1)
    val current = stateFlow.value.localChannelMaskInput
    val next = if (enabled) current or bit else current and bit.inv()
    stateFlow.update { it.copy(localChannelMaskInput = next and 0x1FFF, channelPlanInputDirty = true) }
    persistInt(PREF_LOCAL_CHANNEL_MASK_INPUT, next and 0x1FFF)
    persistInt(PREF_CHANNEL_MASK_INPUT, next and 0x1FFF)
  }

  fun toggleNode24Channel(channel: Int, enabled: Boolean) {
    if (channel !in 1..13) return
    val bit = 1 shl (channel - 1)
    val current = stateFlow.value.nodeChannelMask24Input
    val next = if (enabled) current or bit else current and bit.inv()
    stateFlow.update { it.copy(nodeChannelMask24Input = next and 0x1FFF, channelPlanInputDirty = true) }
    persistInt(PREF_NODE_CHANNEL_MASK24_INPUT, next and 0x1FFF)
  }

  fun toggleNode5Channel(channel: Int, enabled: Boolean) {
    val current = stateFlow.value.nodeChannelMask5GhzInput
    val next = WifiBand.setNode5GhzChannel(current, channel, enabled)
    stateFlow.update { it.copy(nodeChannelMask5GhzInput = next, channelPlanInputDirty = true) }
    persistLong(PREF_NODE_CHANNEL_MASK5_INPUT, next)
  }

  fun applyChannelPlan() {
    val localMask = stateFlow.value.localChannelMaskInput and 0x1FFF
    val nodeMask24 = stateFlow.value.nodeChannelMask24Input and 0x1FFF
    val nodeMask5 = WifiBand.sanitizeNode5GhzMask(stateFlow.value.nodeChannelMask5GhzInput)
    if (service?.setChannelPlan(localMask, nodeMask24, nodeMask5) == true) {
      stateFlow.update { it.copy(channelPlanInputDirty = false) }
    }
  }

  fun setBootMode(mode: Int) { service?.setBootMode(mode) }

  fun setGpsNavMode(mode: Int) {
    val normalized = normalizeGpsNavMode(mode)
    service?.setGpsNavMode(normalized)
    stateFlow.update { it.copy(gpsNavMode = normalized) }
    persistInt(PREF_GPS_NAV_MODE, normalized)
  }

  fun setGpxEnabled(enabled: Boolean) {
    service?.setGpxEnabled(enabled)
    stateFlow.update { it.copy(gpxEnabled = enabled) }
    persistBoolean(PREF_GPX_ENABLED, enabled)
  }

  fun setVisibleTimeoutSec(value: Int) {
    val timeout = value.coerceIn(MIN_VISIBLE_TIMEOUT_SEC, MAX_VISIBLE_TIMEOUT_SEC)
    stateFlow.update { it.copy(visibleTimeoutSec = timeout) }
    persistInt(PREF_VISIBLE_TIMEOUT_SEC, timeout)
    service?.setVisibleTimeout(timeout)
  }

  fun startLogging() { service?.startLogging() }
  fun stopLogging() { service?.stopLogging() }
  fun downloadBacklog() { service?.downloadBacklogToCsv() }
  fun seedDebugBacklog() { service?.seedDebugBacklog() }
  fun pushPhoneGpsNow() { service?.pushPhoneGpsNow() }

  // ── State merging ─────────────────────────────────────────

  private fun mergeServiceState(svc: ServiceState) {
    stateFlow.update { ui ->
      ui.copy(
        connected = svc.connected,
        scanning = svc.scanning,
        bleEncrypted = svc.bleEncrypted,
        currentChannel = svc.currentChannel,
        hopMs = svc.hopMs,
        channelMask = svc.channelMask,
        localChannelMask = svc.localChannelMask,
        localChannelMaskInput = if (!ui.channelPlanInputDirty) svc.localChannelMask else ui.localChannelMaskInput,
        nodeChannelMask24Input =
          if (!ui.channelPlanInputDirty) svc.nodeChannelMask24 else ui.nodeChannelMask24Input,
        nodeChannelMask5GhzInput =
          if (!ui.channelPlanInputDirty) svc.nodeChannelMask5Ghz else ui.nodeChannelMask5GhzInput,
        uniqueBssids = svc.uniqueBssids,
        packetsPerSec = svc.packetsPerSec,
        droppedNotifies = svc.droppedNotifies,
        bootMode = svc.bootMode,
        gpsValid = svc.gpsValid,
        gpsSource = svc.gpsSource,
        gpsAgeS = svc.gpsAgeS,
        gpsAccuracyDm = svc.gpsAccuracyDm,
        gpsSatCount = svc.gpsSatCount,
        gpsSatInUse = svc.gpsSatInUse,
        gpsSatInView = svc.gpsSatInView,
        gpsHdopCenti = svc.gpsHdopCenti,
        gpsPdopCenti = svc.gpsPdopCenti,
        gpsVdopCenti = svc.gpsVdopCenti,
        nodeLinkUp = svc.nodeLinkUp,
        nodeLastSeenS = svc.nodeLastSeenS,
        nodePacketsPerSec = svc.nodePacketsPerSec,
        nodeForwardedSightings = svc.nodeForwardedSightings,
        nodeChannel = svc.nodeChannel,
        nodeChannelMask = svc.nodeChannelMask,
        nodeChannelMask24 = svc.nodeChannelMask24,
        nodeChannelMask5Ghz = svc.nodeChannelMask5Ghz,
        sessionOpen = svc.sessionOpen,
        sessionId = svc.sessionId,
        queuedRecords = svc.queuedRecords,
        queuedBytes = svc.queuedBytes,
        replayActive = svc.replayActive,
        replayCursor = svc.replayCursor,
        queueFull = svc.queueFull,
        droppedFlashFull = svc.droppedFlashFull,
        nodeCount = svc.nodeCount,
        phoneGpsAgeS = svc.phoneGpsAgeS,
        phoneGpsAccuracyM = svc.phoneGpsAccuracyM,
        phoneSpeedKmh = svc.phoneSpeedKmh,
        visibleTimeoutSec = svc.visibleTimeoutSec,
        autoHopEnabled = svc.autoHopEnabled,
        autoHopBaseMs = svc.autoHopBaseMs,
        autoHopAppliedMs = svc.autoHopAppliedMs,
        gpsNavMode = svc.gpsNavMode,
        gpsNavAppliedHz = svc.gpsNavAppliedHz,
        spiffsTotalBytes = svc.spiffsTotalBytes,
        spiffsUsedBytes = svc.spiffsUsedBytes,
        spiffsFreeBytes = svc.spiffsFreeBytes,
        dieTempCenti = svc.dieTempCenti,
        blobActive = svc.blobActive,
        blobSessionId = svc.blobSessionId,
        blobBytesSent = svc.blobBytesSent,
        blobBytesTotal = svc.blobBytesTotal,
        phoneGpsPushActive = svc.phoneGpsPushActive,
        downloadBacklogActive = svc.downloadBacklogActive,
        gpxEnabled = svc.gpxEnabled,
        gpxLogging = svc.gpxLogging,
        gpxPath = svc.gpxPath,
        gpxPointCount = svc.gpxPointCount,
        loggingEnabled = svc.loggingEnabled,
        csvPath = svc.csvPath,
        sightings = svc.sightings,
        logs = svc.logs,
        hopInputMs = if (!ui.hopInputDirty) svc.autoHopBaseMs else ui.hopInputMs,
      )
    }
  }

  override fun onCleared() {
    super.onCleared()
    unbindSafely()
  }
}
