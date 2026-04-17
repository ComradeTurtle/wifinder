package com.wifinder.android

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateContentSize
import androidx.compose.animation.expandVertically
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.AlertDialog
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.runtime.DisposableEffect
import androidx.compose.ui.platform.LocalView
import android.view.WindowManager
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.wifinder.android.model.WifiBand
import com.wifinder.android.ui.AppUiState
import com.wifinder.android.ui.DashboardScreen
import com.wifinder.android.ui.MainViewModel
import com.wifinder.android.ui.SightingUi
import com.wifinder.android.ui.theme.WiFinderTheme
import kotlin.math.roundToInt

class MainActivity : ComponentActivity() {
  private val viewModel: MainViewModel by viewModels()

  private val permissionLauncher =
    registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { _ ->
      refreshPermissionState()
    }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    enableEdgeToEdge()
    refreshPermissionState()

    setContent {
      WiFinderTheme(darkTheme = true) {
        val state by viewModel.state.collectAsStateWithLifecycle()
        if (state.dashboardMode) {
          val view = LocalView.current
          DisposableEffect(Unit) {
            val window = (view.context as? ComponentActivity)?.window
            window?.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            val controller = window?.let { WindowCompat.getInsetsController(it, view) }
            controller?.systemBarsBehavior =
              WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            controller?.hide(WindowInsetsCompat.Type.systemBars())
            onDispose {
              window?.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
              controller?.show(WindowInsetsCompat.Type.systemBars())
            }
          }
          DashboardScreen(
            state = state,
            onExit = viewModel::toggleDashboardMode,
          )
        } else {
        WiFinderScreen(
          state = state,
          onRequestPermissions = ::requestMissingPermissions,
          onConnect = viewModel::connect,
          onDisconnect = viewModel::disconnect,
          onStart = viewModel::sendStart,
          onStop = viewModel::sendStop,
          onRequestStatus = viewModel::requestStatus,
          onRequestSnapshot = viewModel::requestSnapshot,
          onClearSightings = viewModel::clearSightings,
          onClearEspStorage = viewModel::clearEspStorage,
          onHopInput = viewModel::setHopInput,
          onApplyHop = viewModel::applyHopMs,
          onSetAutoHopEnabled = viewModel::setAutoHopEnabled,
          onMasterChannelToggle = viewModel::toggleMaster24Channel,
          onNode24ChannelToggle = viewModel::toggleNode24Channel,
          onNode5ChannelToggle = viewModel::toggleNode5Channel,
          onApplyChannels = viewModel::applyChannelPlan,
          onSetBootMode = viewModel::setBootMode,
          onSetGpsNavMode = viewModel::setGpsNavMode,
          onSetGpxEnabled = viewModel::setGpxEnabled,
          onVisibleTimeout = viewModel::setVisibleTimeoutSec,
          onStartLogging = viewModel::startLogging,
          onStopLogging = viewModel::stopLogging,
          onDownloadBacklog = viewModel::downloadBacklog,
          onSeedDebugBacklog = viewModel::seedDebugBacklog,
          onPushPhoneGpsNow = viewModel::pushPhoneGpsNow,
          onToggleSection = viewModel::toggleSection,
          onToggleDashboard = viewModel::toggleDashboardMode,
        )
        }
      }
    }
  }

  private fun requiredPermissions(): List<String> {
    val permissions = mutableListOf<String>()
    permissions += Manifest.permission.ACCESS_FINE_LOCATION
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
      permissions += Manifest.permission.BLUETOOTH_SCAN
      permissions += Manifest.permission.BLUETOOTH_CONNECT
    }
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      permissions += Manifest.permission.POST_NOTIFICATIONS
    }
    return permissions
  }

  private fun refreshPermissionState() {
    val granted =
      requiredPermissions().all { permission ->
        ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED
      }
    viewModel.setPermissionsGranted(granted)
  }

  private fun requestMissingPermissions() {
    val missing =
      requiredPermissions().filter { permission ->
        ContextCompat.checkSelfPermission(this, permission) != PackageManager.PERMISSION_GRANTED
      }
    if (missing.isNotEmpty()) {
      permissionLauncher.launch(missing.toTypedArray())
    } else {
      refreshPermissionState()
    }
  }
}

// ---------------------------------------------------------------------------
// Section keys
// ---------------------------------------------------------------------------
private const val SEC_STATUS_GENERAL = "status_general"
private const val SEC_STATUS_GPS = "status_gps"
private const val SEC_STATUS_NODE = "status_node"
private const val SEC_CONFIG = "config"
private const val SEC_LOG = "log"

// ---------------------------------------------------------------------------
// RSSI colour helpers
// ---------------------------------------------------------------------------
private fun rssiColor(rssi: Int): Color = when {
  rssi >= -50 -> Color(0xFF4CAF50)   // strong – green
  rssi >= -70 -> Color(0xFFFFC107)   // medium – amber
  else -> Color(0xFFEF5350)          // weak – red
}

private fun ageColor(ageS: Long): Color = when {
  ageS <= 5 -> Color.Unspecified
  ageS <= 15 -> Color(0xFFFFC107)
  else -> Color(0xFFEF5350)
}

private fun gpsSourceLabel(source: Int): String = when (source) {
  1 -> "UART"
  2 -> "PHONE"
  else -> "NONE"
}

private fun gpsNavModeLabel(mode: Int): String = when (mode) {
  1 -> "Force 1 Hz"
  2 -> "Force 2 Hz"
  4 -> "Force 4 Hz"
  else -> "Auto"
}

private fun gpsNavModeWithAppliedLabel(mode: Int, appliedHz: Int): String {
  val applied = if (appliedHz > 0) appliedHz else 1
  return when (mode) {
    1, 2, 4 -> "${gpsNavModeLabel(mode)} (applied ${applied} Hz)"
    else -> "Auto @ ${applied} Hz"
  }
}

private fun formatBytesShort(bytes: Long): String {
  val safe = bytes.coerceAtLeast(0L).toDouble()
  val units = arrayOf("B", "KB", "MB", "GB")
  var value = safe
  var idx = 0
  while (value >= 1024.0 && idx < units.lastIndex) {
    value /= 1024.0
    idx++
  }
  return if (idx == 0) {
    "${value.toLong()} ${units[idx]}"
  } else {
    "${"%.1f".format(value)} ${units[idx]}"
  }
}

private fun transferStatusLabel(state: AppUiState): String {
  if (state.downloadBacklogActive && state.blobActive && state.blobBytesTotal > 0L) {
    val pct = ((state.blobBytesSent * 100L) / state.blobBytesTotal).coerceIn(0L, 100L)
    return "Blob ${formatBytesShort(state.blobBytesSent)} / ${formatBytesShort(state.blobBytesTotal)} (${pct}%)"
  }
  if (state.downloadBacklogActive) {
    return "Downloading ${state.queuedRecords} rec / ${formatBytesShort(state.queuedBytes)}"
  }
  if (state.replayActive) {
    return "Active @ ${state.replayCursor}"
  }
  if (state.queuedRecords > 0L || state.queuedBytes > 0L) {
    return "Idle (${state.queuedRecords} rec / ${formatBytesShort(state.queuedBytes)})"
  }
  return "Idle"
}

// ---------------------------------------------------------------------------
// Main screen
// ---------------------------------------------------------------------------
@Composable
@OptIn(ExperimentalMaterial3Api::class)
private fun WiFinderScreen(
  state: AppUiState,
  onRequestPermissions: () -> Unit,
  onConnect: () -> Unit,
  onDisconnect: () -> Unit,
  onStart: () -> Unit,
  onStop: () -> Unit,
  onRequestStatus: () -> Unit,
  onRequestSnapshot: () -> Unit,
  onClearSightings: () -> Unit,
  onClearEspStorage: () -> Unit,
  onHopInput: (Int) -> Unit,
  onApplyHop: () -> Unit,
  onSetAutoHopEnabled: (Boolean) -> Unit,
  onMasterChannelToggle: (channel: Int, enabled: Boolean) -> Unit,
  onNode24ChannelToggle: (channel: Int, enabled: Boolean) -> Unit,
  onNode5ChannelToggle: (channel: Int, enabled: Boolean) -> Unit,
  onApplyChannels: () -> Unit,
  onSetBootMode: (Int) -> Unit,
  onSetGpsNavMode: (Int) -> Unit,
  onSetGpxEnabled: (Boolean) -> Unit,
  onVisibleTimeout: (Int) -> Unit,
  onStartLogging: () -> Unit,
  onStopLogging: () -> Unit,
  onDownloadBacklog: () -> Unit,
  onSeedDebugBacklog: () -> Unit,
  onPushPhoneGpsNow: () -> Unit,
  onToggleSection: (String) -> Unit,
  onToggleDashboard: () -> Unit,
) {
  val now = System.currentTimeMillis()

  Surface(
    modifier = Modifier.fillMaxSize(),
    color = MaterialTheme.colorScheme.background,
  ) {
    Column(modifier = Modifier.fillMaxSize()) {
      // ── Top bar ─────────────────────────────────────────────
      TopAppBar(
        title = {
          Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
              "WIFINDER",
              fontWeight = FontWeight.Bold,
              letterSpacing = 1.sp,
            )
            Spacer(Modifier.width(8.dp))
            StatusDot(connected = state.connected)
            if (state.scanning) {
              Spacer(Modifier.width(6.dp))
              Text(
                "SCAN",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.primary,
                fontWeight = FontWeight.Bold,
              )
            }
          }
        },
        colors = TopAppBarDefaults.topAppBarColors(
          containerColor = MaterialTheme.colorScheme.surface,
        ),
        actions = {
          IconButton(onClick = onToggleDashboard) {
            Text(
              "▦",
              fontSize = 20.sp,
              color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
          }
          if (!state.permissionsGranted) {
            TextButton(onClick = onRequestPermissions) {
              Text("Grant", color = MaterialTheme.colorScheme.error)
            }
          }
        },
      )

      // ── Status ribbon ───────────────────────────────────────
      StatusRibbon(state = state)

      // ── Permissions warning ─────────────────────────────────
      if (!state.permissionsGranted) {
        Surface(
          modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 4.dp),
          color = Color(0xFF3A1F1F),
          shape = RoundedCornerShape(6.dp),
        ) {
          Text(
            "BLE + Location permissions required",
            modifier = Modifier.padding(8.dp),
            style = MaterialTheme.typography.bodySmall,
            color = Color(0xFFEF9A9A),
          )
        }
      }

      // ── Scrollable content ──────────────────────────────────
      LazyColumn(
        modifier = Modifier.fillMaxSize().padding(horizontal = 10.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
      ) {

        // ── Quick actions (always visible) ───────────────────
        item(key = "quick") {
          QuickActions(
            state = state,
            onConnect = onConnect,
            onDisconnect = onDisconnect,
            onStart = onStart,
            onStop = onStop,
            onRequestStatus = onRequestStatus,
            onRequestSnapshot = onRequestSnapshot,
            onClearSightings = onClearSightings,
            onClearEspStorage = onClearEspStorage,
            onStartLogging = onStartLogging,
            onStopLogging = onStopLogging,
            onDownloadBacklog = onDownloadBacklog,
            onSeedDebugBacklog = onSeedDebugBacklog,
            onPushPhoneGpsNow = onPushPhoneGpsNow,
          )
        }

        // ── General status (collapsible) ─────────────────────
        item(key = SEC_STATUS_GENERAL) {
          CollapsibleSection(
            title = "General Status",
            expanded = SEC_STATUS_GENERAL in state.expandedSections,
            onToggle = { onToggleSection(SEC_STATUS_GENERAL) },
          ) {
            GeneralStatusDetails(state = state)
          }
        }

        // ── GPS status (collapsible) ─────────────────────────
        item(key = SEC_STATUS_GPS) {
          CollapsibleSection(
            title = "GPS Status",
            expanded = SEC_STATUS_GPS in state.expandedSections,
            onToggle = { onToggleSection(SEC_STATUS_GPS) },
          ) {
            GpsStatusDetails(state = state)
          }
        }

        // ── Node status (collapsible) ────────────────────────
        item(key = SEC_STATUS_NODE) {
          CollapsibleSection(
            title = "Node Status",
            expanded = SEC_STATUS_NODE in state.expandedSections,
            onToggle = { onToggleSection(SEC_STATUS_NODE) },
          ) {
            NodeStatusDetails(state = state)
          }
        }

        // ── Config: hop, channels, boot (collapsible) ────────
        item(key = SEC_CONFIG) {
          CollapsibleSection(
            title = "Configuration",
            expanded = SEC_CONFIG in state.expandedSections,
            onToggle = { onToggleSection(SEC_CONFIG) },
          ) {
            ConfigSection(
              state = state,
              onHopInput = onHopInput,
              onApplyHop = onApplyHop,
              onSetAutoHopEnabled = onSetAutoHopEnabled,
              onMasterChannelToggle = onMasterChannelToggle,
              onNode24ChannelToggle = onNode24ChannelToggle,
              onNode5ChannelToggle = onNode5ChannelToggle,
              onApplyChannels = onApplyChannels,
              onSetBootMode = onSetBootMode,
              onSetGpsNavMode = onSetGpsNavMode,
              onSetGpxEnabled = onSetGpxEnabled,
              onVisibleTimeout = onVisibleTimeout,
            )
          }
        }

        // ── Log (collapsible) ────────────────────────────────
        item(key = SEC_LOG) {
          CollapsibleSection(
            title = "Log (${state.logs.size})",
            expanded = SEC_LOG in state.expandedSections,
            onToggle = { onToggleSection(SEC_LOG) },
          ) {
            LogContent(logs = state.logs)
          }
        }

        // ── Sighting header ─────────────────────────────────
        item(key = "sighting_header") {
          Column {
            Spacer(Modifier.height(4.dp))
            Row(
              modifier = Modifier.fillMaxWidth(),
              horizontalArrangement = Arrangement.SpaceBetween,
              verticalAlignment = Alignment.CenterVertically,
            ) {
              Text(
                "Visible: ${state.sightings.size}",
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.primary,
              )
              Text(
                "timeout ${state.visibleTimeoutSec}s",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
              )
            }
            Spacer(Modifier.height(4.dp))
            SightingHeaderRow()
            HorizontalDivider(color = MaterialTheme.colorScheme.outline)
          }
        }

        // ── Sighting rows ───────────────────────────────────
        items(state.sightings, key = { it.bssid }) { sighting ->
          SightingRowCompact(sighting = sighting, nowMs = now)
          HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        }

        item(key = "bottom_spacer") {
          Spacer(modifier = Modifier.height(24.dp))
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Status dot
// ---------------------------------------------------------------------------
@Composable
private fun StatusDot(connected: Boolean) {
  Box(
    modifier = Modifier
      .size(10.dp)
      .clip(CircleShape)
      .background(if (connected) Color(0xFF4CAF50) else Color(0xFFEF5350)),
  )
}

// ---------------------------------------------------------------------------
// Status ribbon – always visible single-row summary
// ---------------------------------------------------------------------------
@Composable
private fun StatusRibbon(state: AppUiState) {
  Surface(
    modifier = Modifier.fillMaxWidth(),
    color = MaterialTheme.colorScheme.surfaceVariant,
    tonalElevation = 1.dp,
  ) {
    Row(
      modifier = Modifier
        .horizontalScroll(rememberScrollState())
        .padding(horizontal = 10.dp, vertical = 6.dp),
      horizontalArrangement = Arrangement.spacedBy(12.dp),
      verticalAlignment = Alignment.CenterVertically,
    ) {
      RibbonChip(
        label = if (state.connected) "BLE ✓" else "BLE ✗",
        color = if (state.connected) Color(0xFF4CAF50) else Color(0xFFEF5350),
      )
      RibbonChip(label = "Ch ${WifiBand.conciseChannelLabel(state.currentChannel)}")
      RibbonChip(label = "${state.uniqueBssids} APs")
      if (state.autoHopEnabled) {
        RibbonChip(
          label = "AutoHop ${state.autoHopAppliedMs}ms",
          color = MaterialTheme.colorScheme.primary,
        )
      }
      RibbonChip(label = "${state.packetsPerSec} pkt/s")
      RibbonChip(
        label = if (state.gpsValid) "ESP GPS ✓" else "ESP GPS ✗",
        color = if (state.gpsValid) Color(0xFF4CAF50) else Color(0xFFEF5350),
      )
      if (state.gpsValid) {
        RibbonChip(label = "Src ${gpsSourceLabel(state.gpsSource)}")
      }
      RibbonChip(
        label = if (state.nodeLinkUp) "Node ✓" else "Node ✗",
        color = if (state.nodeLinkUp) Color(0xFF4CAF50) else Color(0xFFEF5350),
      )
      if (state.nodeLinkUp && state.nodeChannel > 0) {
        RibbonChip(label = "NodeCh ${WifiBand.conciseChannelLabel(state.nodeChannel)}")
      }
      RibbonChip(
        label = when {
          state.phoneGpsAgeS < 0 -> "📱GPS ✗"
          state.phoneGpsAgeS <= 3 -> "📱GPS ✓"
          else -> "📱GPS ${state.phoneGpsAgeS}s"
        },
        color = when {
          state.phoneGpsAgeS < 0 -> Color(0xFFEF5350)
          state.phoneGpsAgeS <= 3 -> Color(0xFF4CAF50)
          else -> Color(0xFFFFC107)
        },
      )
      if (state.loggingEnabled) {
        RibbonChip(label = "CSV ●", color = Color(0xFFEF5350))
      }
      if (state.gpxEnabled && state.gpxLogging) {
        RibbonChip(label = "GPX ●", color = Color(0xFF4CAF50))
      }
    }
  }
}

@Composable
private fun RibbonChip(label: String, color: Color = MaterialTheme.colorScheme.onSurfaceVariant) {
  Text(
    text = label,
    style = MaterialTheme.typography.labelSmall,
    fontWeight = FontWeight.Medium,
    color = color,
    maxLines = 1,
  )
}

// ---------------------------------------------------------------------------
// Collapsible section wrapper
// ---------------------------------------------------------------------------
@Composable
private fun CollapsibleSection(
  title: String,
  expanded: Boolean,
  onToggle: () -> Unit,
  content: @Composable () -> Unit,
) {
  Card(
    colors = CardDefaults.cardColors(
      containerColor = MaterialTheme.colorScheme.surface,
    ),
    shape = RoundedCornerShape(8.dp),
  ) {
    Column(modifier = Modifier.animateContentSize()) {
      Row(
        modifier = Modifier
          .fillMaxWidth()
          .clickable(onClick = onToggle)
          .padding(horizontal = 12.dp, vertical = 10.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
      ) {
        Text(
          text = title,
          style = MaterialTheme.typography.titleSmall,
          fontWeight = FontWeight.SemiBold,
        )
        Text(
          text = if (expanded) "▲" else "▼",
          style = MaterialTheme.typography.labelSmall,
          color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
      }
      AnimatedVisibility(
        visible = expanded,
        enter = expandVertically(),
        exit = shrinkVertically(),
      ) {
        Column(modifier = Modifier.padding(start = 12.dp, end = 12.dp, bottom = 10.dp)) {
          content()
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Quick actions – always visible compact button row
// ---------------------------------------------------------------------------
@Composable
private fun QuickActions(
  state: AppUiState,
  onConnect: () -> Unit,
  onDisconnect: () -> Unit,
  onStart: () -> Unit,
  onStop: () -> Unit,
  onRequestStatus: () -> Unit,
  onRequestSnapshot: () -> Unit,
  onClearSightings: () -> Unit,
  onClearEspStorage: () -> Unit,
  onStartLogging: () -> Unit,
  onStopLogging: () -> Unit,
  onDownloadBacklog: () -> Unit,
  onSeedDebugBacklog: () -> Unit,
  onPushPhoneGpsNow: () -> Unit,
) {
  Card(
    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
    shape = RoundedCornerShape(8.dp),
  ) {
    Column(
      modifier = Modifier.padding(10.dp),
      verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
      // Row 1: connection + scanning
      Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
      ) {
        if (!state.connected) {
          CompactButton(label = "Connect", primary = true, modifier = Modifier.weight(1f), onClick = onConnect)
        } else {
          CompactButton(label = "Disconnect", primary = false, modifier = Modifier.weight(1f), onClick = onDisconnect)
        }
        if (!state.scanning) {
          CompactButton(label = "Start", primary = true, modifier = Modifier.weight(1f), onClick = onStart)
        } else {
          CompactButton(label = "Stop", primary = false, modifier = Modifier.weight(1f), onClick = onStop)
        }
        CompactButton(label = "Status", primary = false, modifier = Modifier.weight(1f), onClick = onRequestStatus)
      }
      // Row 2: snapshot, backlog download, phone GPS streaming toggle
      Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
      ) {
        CompactButton(label = "Snapshot", primary = false, modifier = Modifier.weight(1f), onClick = onRequestSnapshot)
        CompactButton(
          label = if (state.downloadBacklogActive) "Stop Download" else "Download ESP",
          primary = !state.downloadBacklogActive,
          modifier = Modifier.weight(1f),
          onClick = onDownloadBacklog,
        )
        CompactButton(
          label = if (state.phoneGpsPushActive) "Stop GPS Push" else "Push GPS",
          primary = !state.phoneGpsPushActive,
          modifier = Modifier.weight(1f),
          onClick = onPushPhoneGpsNow,
        )
      }
      // Row 3: local clear, storage clear, CSV
      Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
      ) {
        CompactButton(label = "Clear", primary = false, modifier = Modifier.weight(1f), onClick = onClearSightings)

        var showClearEspConfirm by remember { mutableStateOf(false) }
        CompactButton(
          label = "Clear ESP",
          primary = false,
          modifier = Modifier.weight(1f),
          onClick = { showClearEspConfirm = true },
        )
        if (showClearEspConfirm) {
          AlertDialog(
            onDismissRequest = { showClearEspConfirm = false },
            title = { Text("Erase ESP Storage?") },
            text = {
              Text(
                "This will permanently delete ALL recorded sessions on the ESP.\n\n" +
                  "This action cannot be undone.",
              )
            },
            confirmButton = {
              TextButton(onClick = {
                showClearEspConfirm = false
                onClearEspStorage()
              }) {
                Text("Delete Everything", color = MaterialTheme.colorScheme.error)
              }
            },
            dismissButton = {
              TextButton(onClick = { showClearEspConfirm = false }) {
                Text("Cancel")
              }
            },
          )
        }
        if (state.loggingEnabled) {
          CompactButton(label = "Stop CSV", primary = false, modifier = Modifier.weight(1f), onClick = onStopLogging)
        } else {
          CompactButton(label = "Start CSV", primary = true, modifier = Modifier.weight(1f), onClick = onStartLogging)
        }
      }
      Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
      ) {
        CompactButton(
          label = "Dev Seed ~768KB",
          primary = false,
          modifier = Modifier.weight(1f),
          onClick = onSeedDebugBacklog,
        )
      }
    }
  }
}

@Composable
private fun CompactButton(
  label: String,
  primary: Boolean,
  modifier: Modifier = Modifier,
  onClick: () -> Unit,
) {
  if (primary) {
    Button(
      onClick = onClick,
      modifier = modifier.height(36.dp),
      shape = RoundedCornerShape(6.dp),
      contentPadding = ButtonDefaults.ContentPadding,
    ) {
      Text(label, style = MaterialTheme.typography.labelMedium)
    }
  } else {
    OutlinedButton(
      onClick = onClick,
      modifier = modifier.height(36.dp),
      shape = RoundedCornerShape(6.dp),
      contentPadding = ButtonDefaults.ContentPadding,
    ) {
      Text(label, style = MaterialTheme.typography.labelMedium)
    }
  }
}

// ---------------------------------------------------------------------------
// Status details (inside collapsibles)
// ---------------------------------------------------------------------------
@Composable
private fun GeneralStatusDetails(state: AppUiState) {
  Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
    KvRow("BLE", if (state.connected) "Connected" else "Disconnected")
    KvRow("Link", if (state.bleEncrypted) "Encrypted" else "Unencrypted")
    KvRow("Scanner", if (state.scanning) "Running" else "Stopped")
    KvRow("Current Channel", channelWithFrequencyLabel(state.currentChannel))
    KvRow("Hop Mode", if (state.autoHopEnabled) "Auto (speed)" else "Manual")
    if (state.autoHopEnabled) {
      KvRow("Hop Base", "${state.autoHopBaseMs} ms")
      KvRow("Hop Applied", "${state.autoHopAppliedMs} ms")
    } else {
      KvRow("Hop", "${state.hopMs} ms")
    }
    KvRow("Master 2.4 Mask", "0x${state.localChannelMask.toString(16).uppercase()}")
    KvRow("Unique BSSID", state.uniqueBssids.toString())
    KvRow("Packets/s", state.packetsPerSec.toString())
    KvRow("Notify Drops", state.droppedNotifies.toString())
    KvRow("Boot Mode", if (state.bootMode == 1) "Auto" else "Manual")
    KvRow(
      "Session",
      if (state.sessionId != 0L) {
        "${if (state.sessionOpen) "Open" else "Closed"} 0x${state.sessionId.toULong().toString(16).uppercase()}"
      } else {
        "-"
      },
    )
    KvRow("Backlog", transferStatusLabel(state))
    KvRow(
      "Storage",
      "${formatBytesShort(state.spiffsUsedBytes)} used / ${formatBytesShort(state.spiffsTotalBytes)} total (${formatBytesShort(state.spiffsFreeBytes)} free)",
    )
    KvRow(
      "Die Temp",
      if (state.dieTempCenti != Int.MIN_VALUE) "${"%.2f".format(state.dieTempCenti / 100.0)} °C" else "-",
    )
    KvRow(
      "Queue Pressure",
      if (state.queueFull) "YES (drops=${state.droppedFlashFull})" else "No (drops=${state.droppedFlashFull})",
    )
    KvRow("Visible Rows", state.sightings.size.toString())
    KvRow("CSV", if (state.loggingEnabled) "Recording" else "Stopped")
    KvRow(
      "GPX",
      when {
        !state.gpxEnabled -> "Disabled"
        state.gpxLogging -> "Recording (${state.gpxPointCount} pts)"
        state.gpxPath.isNotBlank() -> "Ready (${state.gpxPointCount} pts)"
        else -> "Idle"
      },
    )
    if (state.csvPath.isNotBlank()) {
      Text(
        text = state.csvPath,
        style = MaterialTheme.typography.bodySmall,
        maxLines = 2,
        overflow = TextOverflow.Ellipsis,
        fontFamily = FontFamily.Monospace,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
      )
    }
    if (state.gpxPath.isNotBlank()) {
      Text(
        text = state.gpxPath,
        style = MaterialTheme.typography.bodySmall,
        maxLines = 2,
        overflow = TextOverflow.Ellipsis,
        fontFamily = FontFamily.Monospace,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
      )
    }
  }
}

@Composable
private fun GpsStatusDetails(state: AppUiState) {
  val hdopText = if (state.gpsHdopCenti > 0) "%.2f".format(state.gpsHdopCenti / 100.0) else "-"
  val vdopText = if (state.gpsVdopCenti > 0) "%.2f".format(state.gpsVdopCenti / 100.0) else "-"
  val pdopText = if (state.gpsPdopCenti > 0) "%.2f".format(state.gpsPdopCenti / 100.0) else "-"
  val satInUse = if (state.gpsSatInUse > 0) state.gpsSatInUse else state.gpsSatCount
  val satUseText = if (satInUse > 0) satInUse.toString() else "-"
  val satViewText = if (state.gpsSatInView > 0) state.gpsSatInView.toString() else "-"
  val satText = if (satInUse > 0 && state.gpsSatInView > 0) "$satInUse/${state.gpsSatInView}" else satUseText

  Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
    KvRow(
      "ESP GPS",
      if (state.gpsValid) {
        "${state.gpsAgeS}s old, ±${state.gpsAccuracyDm / 10.0}m, sats(use/view)=$satText, DOP(H/V/P)=$hdopText/$vdopText/$pdopText, src=${gpsSourceLabel(state.gpsSource)}"
      } else {
        "No fix"
      },
    )
    KvRow("ESP Source", gpsSourceLabel(state.gpsSource))
    KvRow("ESP Valid", if (state.gpsValid) "Yes" else "No")
    KvRow("ESP Age", "${state.gpsAgeS}s")
    KvRow("ESP Accuracy", "±${state.gpsAccuracyDm / 10.0}m")
    KvRow("ESP Sats In Use", satUseText)
    KvRow("ESP Sats In View", satViewText)
    KvRow("ESP HDOP", hdopText)
    KvRow("ESP VDOP", vdopText)
    KvRow("ESP PDOP", pdopText)
    KvRow("Nav Rate Mode", gpsNavModeWithAppliedLabel(state.gpsNavMode, state.gpsNavAppliedHz))
    KvRow(
      "Phone GPS",
      if (state.phoneGpsAgeS >= 0) {
        "${state.phoneGpsAgeS}s old, ±${"%.1f".format(state.phoneGpsAccuracyM)}m, ${"%.1f".format(state.phoneSpeedKmh)} km/h"
      } else {
        "No fix"
      },
    )
    KvRow("Phone Age", if (state.phoneGpsAgeS >= 0) "${state.phoneGpsAgeS}s" else "-")
    KvRow("Phone Accuracy", if (state.phoneGpsAgeS >= 0) "±${"%.1f".format(state.phoneGpsAccuracyM)}m" else "-")
    KvRow("Phone Speed", "${"%.1f".format(state.phoneSpeedKmh)} km/h")
    KvRow("Phone Push", if (state.phoneGpsPushActive) "Streaming" else "Idle")
  }
}

@Composable
private fun NodeStatusDetails(state: AppUiState) {
  Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
    KvRow("Node Link", if (state.nodeLinkUp) "Up (${state.nodeLastSeenS}s)" else "Down")
    KvRow("Node Last Seen", "${state.nodeLastSeenS}s")
    KvRow("Node Ch", if (state.nodeChannel > 0) channelWithFrequencyLabel(state.nodeChannel) else "-")
    KvRow("Node 2.4 Mask", "0x${state.nodeChannelMask24.toString(16).uppercase()}")
    KvRow("Node 5G Mask", "0x${state.nodeChannelMask5Ghz.toULong().toString(16).uppercase()}")
    KvRow("Node pkt/s", state.nodePacketsPerSec.toString())
    KvRow("Node sightings", state.nodeForwardedSightings.toString())
    KvRow("Node count", state.nodeCount.toString())
    KvRow("Node Enabled", if (state.nodeChannelMask24 != 0 || state.nodeChannelMask5Ghz != 0L) "Yes" else "No")
  }
}

// ---------------------------------------------------------------------------
// Config section (inside collapsible)
// ---------------------------------------------------------------------------
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun ConfigSection(
  state: AppUiState,
  onHopInput: (Int) -> Unit,
  onApplyHop: () -> Unit,
  onSetAutoHopEnabled: (Boolean) -> Unit,
  onMasterChannelToggle: (channel: Int, enabled: Boolean) -> Unit,
  onNode24ChannelToggle: (channel: Int, enabled: Boolean) -> Unit,
  onNode5ChannelToggle: (channel: Int, enabled: Boolean) -> Unit,
  onApplyChannels: () -> Unit,
  onSetBootMode: (Int) -> Unit,
  onSetGpsNavMode: (Int) -> Unit,
  onSetGpxEnabled: (Boolean) -> Unit,
  onVisibleTimeout: (Int) -> Unit,
) {
  Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
    // -- Hop mode --
    Text("Hop Mode", style = MaterialTheme.typography.labelMedium)
    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
      FilterChip(
        selected = !state.autoHopEnabled,
        onClick = { onSetAutoHopEnabled(false) },
        label = { Text("Manual") },
        shape = RoundedCornerShape(6.dp),
      )
      FilterChip(
        selected = state.autoHopEnabled,
        onClick = { onSetAutoHopEnabled(true) },
        label = { Text("Auto (GPS speed)") },
        shape = RoundedCornerShape(6.dp),
      )
    }

    // -- Hop slider --
    Text(
      if (state.autoHopEnabled) {
        "Base Hop: ${state.hopInputMs} ms (applied ${state.autoHopAppliedMs} ms)"
      } else {
        "Hop: ${state.hopInputMs} ms"
      },
      style = MaterialTheme.typography.labelMedium,
    )
    Slider(
      value = state.hopInputMs.toFloat(),
      valueRange = 50f..2000f,
      onValueChange = { onHopInput(it.roundToInt()) },
      colors = SliderDefaults.colors(
        thumbColor = MaterialTheme.colorScheme.primary,
        activeTrackColor = MaterialTheme.colorScheme.primary,
      ),
    )
    if (state.autoHopEnabled) {
      Text(
        "Phone speed: ${"%.1f".format(state.phoneSpeedKmh)} km/h",
        style = MaterialTheme.typography.labelSmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
      )
    }
    OutlinedButton(
      onClick = onApplyHop,
      modifier = Modifier.fillMaxWidth().height(34.dp),
      shape = RoundedCornerShape(6.dp),
    ) {
      Text(
        if (state.autoHopEnabled) "Apply Base Hop" else "Apply Hop",
        style = MaterialTheme.typography.labelMedium,
      )
    }

    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)

    // -- Channel chips --
    Text("Master Channels (C6 / 2.4 GHz)", style = MaterialTheme.typography.labelMedium)
    FlowRow(
      horizontalArrangement = Arrangement.spacedBy(4.dp),
      verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
      for (ch in WifiBand.CHANNELS_24_GHZ) {
        val selected = (state.localChannelMaskInput and (1 shl (ch - 1))) != 0
        FilterChip(
          selected = selected,
          onClick = { onMasterChannelToggle(ch, !selected) },
          label = { Text(ch.toString(), style = MaterialTheme.typography.labelSmall) },
          modifier = Modifier.height(30.dp),
          shape = RoundedCornerShape(6.dp),
          colors = FilterChipDefaults.filterChipColors(
            selectedContainerColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.25f),
            selectedLabelColor = MaterialTheme.colorScheme.primary,
          ),
        )
      }
    }
    Text("Slave Channels (RTL / 2.4 GHz)", style = MaterialTheme.typography.labelMedium)
    FlowRow(
      horizontalArrangement = Arrangement.spacedBy(4.dp),
      verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
      for (ch in WifiBand.CHANNELS_24_GHZ) {
        val selected = (state.nodeChannelMask24Input and (1 shl (ch - 1))) != 0
        FilterChip(
          selected = selected,
          onClick = { onNode24ChannelToggle(ch, !selected) },
          label = { Text(ch.toString(), style = MaterialTheme.typography.labelSmall) },
          modifier = Modifier.height(30.dp),
          shape = RoundedCornerShape(6.dp),
          colors = FilterChipDefaults.filterChipColors(
            selectedContainerColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.25f),
            selectedLabelColor = MaterialTheme.colorScheme.primary,
          ),
        )
      }
    }
    Text("Slave Channels (RTL / 5 GHz)", style = MaterialTheme.typography.labelMedium)
    FlowRow(
      horizontalArrangement = Arrangement.spacedBy(4.dp),
      verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
      for (ch in WifiBand.NODE_CHANNELS_5_GHZ) {
        val selected = WifiBand.node5GhzChannelEnabled(state.nodeChannelMask5GhzInput, ch)
        FilterChip(
          selected = selected,
          onClick = { onNode5ChannelToggle(ch, !selected) },
          label = { Text(ch.toString(), style = MaterialTheme.typography.labelSmall) },
          modifier = Modifier.height(30.dp),
          shape = RoundedCornerShape(6.dp),
          colors = FilterChipDefaults.filterChipColors(
            selectedContainerColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.25f),
            selectedLabelColor = MaterialTheme.colorScheme.primary,
          ),
        )
      }
    }
    OutlinedButton(
      onClick = onApplyChannels,
      modifier = Modifier.fillMaxWidth().height(34.dp),
      shape = RoundedCornerShape(6.dp),
    ) {
      Text("Apply Channel Plan", style = MaterialTheme.typography.labelMedium)
    }

    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)

    // -- Boot mode --
    Text("Boot Mode", style = MaterialTheme.typography.labelMedium)
    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
      FilterChip(
        selected = state.bootMode == 0,
        onClick = { onSetBootMode(0) },
        label = { Text("Manual") },
        shape = RoundedCornerShape(6.dp),
      )
      FilterChip(
        selected = state.bootMode == 1,
        onClick = { onSetBootMode(1) },
        label = { Text("Auto") },
        shape = RoundedCornerShape(6.dp),
      )
    }

    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)

    // -- GPS nav mode --
    Text("GPS Nav Rate", style = MaterialTheme.typography.labelMedium)
    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
      FilterChip(
        selected = state.gpsNavMode == 0,
        onClick = { onSetGpsNavMode(0) },
        label = { Text("Auto") },
        shape = RoundedCornerShape(6.dp),
      )
      FilterChip(
        selected = state.gpsNavMode == 1,
        onClick = { onSetGpsNavMode(1) },
        label = { Text("1 Hz") },
        shape = RoundedCornerShape(6.dp),
      )
      FilterChip(
        selected = state.gpsNavMode == 2,
        onClick = { onSetGpsNavMode(2) },
        label = { Text("2 Hz") },
        shape = RoundedCornerShape(6.dp),
      )
      FilterChip(
        selected = state.gpsNavMode == 4,
        onClick = { onSetGpsNavMode(4) },
        label = { Text("4 Hz") },
        shape = RoundedCornerShape(6.dp),
      )
    }

    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)

    Text("GPX Track Logging", style = MaterialTheme.typography.labelMedium)
    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
      FilterChip(
        selected = state.gpxEnabled,
        onClick = { onSetGpxEnabled(true) },
        label = { Text("Enabled") },
        shape = RoundedCornerShape(6.dp),
      )
      FilterChip(
        selected = !state.gpxEnabled,
        onClick = { onSetGpxEnabled(false) },
        label = { Text("Disabled") },
        shape = RoundedCornerShape(6.dp),
      )
    }

    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)

    // -- Visible timeout slider --
    Text(
      "Visible timeout: ${state.visibleTimeoutSec}s",
      style = MaterialTheme.typography.labelMedium,
    )
    Slider(
      value = state.visibleTimeoutSec.toFloat(),
      valueRange = 5f..300f,
      onValueChange = { onVisibleTimeout(it.roundToInt()) },
      colors = SliderDefaults.colors(
        thumbColor = MaterialTheme.colorScheme.primary,
        activeTrackColor = MaterialTheme.colorScheme.primary,
      ),
    )
  }
}

// ---------------------------------------------------------------------------
// Log content (inside collapsible)
// ---------------------------------------------------------------------------
@Composable
private fun LogContent(logs: List<String>) {
  if (logs.isEmpty()) {
    Text(
      "No events yet",
      style = MaterialTheme.typography.bodySmall,
      color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
  } else {
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
      for (line in logs.take(20)) {
        Text(
          text = line,
          style = MaterialTheme.typography.bodySmall,
          fontFamily = FontFamily.Monospace,
          maxLines = 1,
          overflow = TextOverflow.Ellipsis,
          fontSize = 11.sp,
          color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Sighting table
// ---------------------------------------------------------------------------
@Composable
private fun SightingHeaderRow() {
  Row(modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
    HeaderCell(text = "BSSID", weight = 0.28f)
    HeaderCell(text = "SSID", weight = 0.26f)
    HeaderCell(text = "Auth", weight = 0.22f)
    HeaderCell(text = "Ch", weight = 0.08f)
    HeaderCell(text = "RSSI", weight = 0.08f)
    HeaderCell(text = "Age", weight = 0.08f)
  }
}

@Composable
private fun RowScope.HeaderCell(text: String, weight: Float) {
  Text(
    text = text,
    modifier = Modifier.weight(weight),
    style = MaterialTheme.typography.labelSmall,
    fontWeight = FontWeight.Bold,
    color = MaterialTheme.colorScheme.primary,
    maxLines = 1,
    fontSize = 10.sp,
  )
}

@Composable
private fun SightingRowCompact(sighting: SightingUi, nowMs: Long) {
  val ageS = ((nowMs - sighting.lastSeenMs).coerceAtLeast(0L)) / 1000L
  Row(modifier = Modifier.fillMaxWidth().padding(vertical = 3.dp)) {
    CompactCell(text = sighting.bssid, weight = 0.28f, mono = true)
    CompactCell(
      text = if (sighting.ssid.isBlank()) "<hidden>" else sighting.ssid,
      weight = 0.26f,
    )
    CompactCell(text = sighting.auth, weight = 0.22f)
    CompactCell(text = WifiBand.conciseChannelLabel(sighting.channel), weight = 0.08f)
    CompactCell(text = sighting.rssi.toString(), weight = 0.08f, color = rssiColor(sighting.rssi))
    CompactCell(text = "${ageS}s", weight = 0.08f, color = ageColor(ageS))
  }
}

@Composable
private fun RowScope.CompactCell(
  text: String,
  weight: Float,
  mono: Boolean = false,
  color: Color = Color.Unspecified,
) {
  Text(
    text = text,
    modifier = Modifier.weight(weight),
    style = MaterialTheme.typography.bodySmall,
    maxLines = 1,
    overflow = TextOverflow.Ellipsis,
    fontFamily = if (mono) FontFamily.Monospace else FontFamily.Default,
    fontSize = 11.sp,
    color = color,
  )
}

@Composable
private fun KvRow(label: String, value: String) {
  Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
    Text(
      label,
      style = MaterialTheme.typography.bodySmall,
      color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Text(
      value,
      style = MaterialTheme.typography.bodySmall,
    )
  }
}

private fun channelWithFrequencyLabel(channel: Int): String {
  val freq = WifiBand.frequencyMhzFromChannel(channel) ?: return channel.toString()
  return "${WifiBand.conciseChannelLabel(channel)} (${freq} MHz)"
}
