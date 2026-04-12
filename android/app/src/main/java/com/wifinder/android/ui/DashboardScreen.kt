package com.wifinder.android.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.wifinder.android.model.WifiBand

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------
private val DashBg = Color(0xFF0D0D0D)
private val CardBg = Color(0xFF1A1A1A)
private val Teal = Color(0xFF00BFA5)
private val TealDim = Color(0xFF00897B)
private val Amber = Color(0xFFFFC107)
private val Red = Color(0xFFEF5350)
private val Green = Color(0xFF4CAF50)
private val DimLabel = Color(0xFF757575)
private val BrightLabel = Color(0xFFE0E0E0)

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
@Composable
fun DashboardScreen(
  state: AppUiState,
  onExit: () -> Unit,
) {
  Surface(
    modifier = Modifier.fillMaxSize(),
    color = DashBg,
  ) {
    BoxWithConstraints(modifier = Modifier.fillMaxSize()) {
      val isLandscape = maxWidth > maxHeight
      if (isLandscape) {
        DashboardLandscape(state = state, onExit = onExit)
      } else {
        DashboardPortrait(state = state, onExit = onExit)
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Portrait layout
// ---------------------------------------------------------------------------
@Composable
private fun DashboardPortrait(state: AppUiState, onExit: () -> Unit) {
  Column(
    modifier = Modifier
      .fillMaxSize()
      .padding(horizontal = 16.dp, vertical = 8.dp),
  ) {
    DashTopBar(onExit = onExit, state = state)

    Spacer(Modifier.weight(1f))

    HeroApCount(count = state.uniqueBssids, modifier = Modifier.fillMaxWidth())

    Spacer(Modifier.weight(1f))

    RateStrip(state = state, modifier = Modifier.fillMaxWidth())

    Spacer(Modifier.weight(1f))

    HealthIndicators(state = state, modifier = Modifier.fillMaxWidth())

    Spacer(Modifier.weight(1f))

    GpsDetailRow(state = state, modifier = Modifier.fillMaxWidth())

    Spacer(Modifier.weight(1f))

    ChannelChart(
      sightings = state.sightings,
      modifier = Modifier
        .fillMaxWidth()
        .height(150.dp),
    )

    Spacer(Modifier.weight(1f))

    StorageBar(state = state, modifier = Modifier.fillMaxWidth())
    Spacer(Modifier.height(4.dp))
    BacklogRow(state = state, modifier = Modifier.fillMaxWidth())
  }
}

// ---------------------------------------------------------------------------
// Landscape layout – two-column
// ---------------------------------------------------------------------------
@Composable
private fun DashboardLandscape(state: AppUiState, onExit: () -> Unit) {
  Column(
    modifier = Modifier
      .fillMaxSize()
      .padding(horizontal = 16.dp, vertical = 6.dp),
  ) {
    DashTopBar(onExit = onExit, state = state)

    Spacer(Modifier.height(4.dp))

    Row(
      modifier = Modifier
        .fillMaxWidth()
        .weight(1f),
      horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
      // Left column
      Column(
        modifier = Modifier
          .weight(1f)
          .fillMaxHeight(),
        verticalArrangement = Arrangement.SpaceEvenly,
      ) {
        HeroApCount(count = state.uniqueBssids, modifier = Modifier.fillMaxWidth())
        RateStrip(state = state, modifier = Modifier.fillMaxWidth())
        HealthIndicators(state = state, modifier = Modifier.fillMaxWidth())
        GpsDetailRow(state = state, modifier = Modifier.fillMaxWidth())
      }

      // Right column
      Column(
        modifier = Modifier
          .weight(1f)
          .fillMaxHeight(),
        verticalArrangement = Arrangement.SpaceBetween,
      ) {
        ChannelChart(
          sightings = state.sightings,
          modifier = Modifier
            .fillMaxWidth()
            .weight(1f),
        )
        Spacer(Modifier.height(6.dp))
        StorageBar(state = state, modifier = Modifier.fillMaxWidth())
        Spacer(Modifier.height(4.dp))
        BacklogRow(state = state, modifier = Modifier.fillMaxWidth())
      }
    }
    Spacer(Modifier.height(4.dp))
  }
}

// ---------------------------------------------------------------------------
// Top bar (← exit + scanning / logging dots)
// ---------------------------------------------------------------------------
@Composable
private fun DashTopBar(onExit: () -> Unit, state: AppUiState) {
  Row(
    modifier = Modifier
      .fillMaxWidth()
      .clip(RoundedCornerShape(6.dp))
      .background(CardBg)
      .clickable(onClick = onExit)
      .padding(horizontal = 12.dp, vertical = 8.dp),
    verticalAlignment = Alignment.CenterVertically,
  ) {
    Text(
      text = "←",
      color = DimLabel,
      fontSize = 18.sp,
      fontWeight = FontWeight.Bold,
    )
    Spacer(Modifier.width(8.dp))
    Text(
      text = "DASHBOARD",
      color = DimLabel,
      fontSize = 12.sp,
      fontWeight = FontWeight.Medium,
      letterSpacing = 2.sp,
    )
    Spacer(Modifier.weight(1f))
    if (state.scanning) {
      MiniDot(color = Teal)
      Spacer(Modifier.width(4.dp))
      Text("SCAN", color = Teal, fontSize = 10.sp, fontWeight = FontWeight.Bold)
      Spacer(Modifier.width(8.dp))
    }
    if (state.loggingEnabled) {
      MiniDot(color = Red)
      Spacer(Modifier.width(4.dp))
      Text("CSV", color = Red, fontSize = 10.sp, fontWeight = FontWeight.Bold)
      Spacer(Modifier.width(8.dp))
    }
    if (state.gpxEnabled && state.gpxLogging) {
      MiniDot(color = Green)
      Spacer(Modifier.width(4.dp))
      Text("GPX", color = Green, fontSize = 10.sp, fontWeight = FontWeight.Bold)
    }
  }
}

@Composable
private fun MiniDot(color: Color) {
  Box(
    modifier = Modifier
      .size(6.dp)
      .clip(CircleShape)
      .background(color),
  )
}

// ---------------------------------------------------------------------------
// Hero AP count
// ---------------------------------------------------------------------------
@Composable
private fun HeroApCount(count: Int, modifier: Modifier = Modifier) {
  Column(
    modifier = modifier,
    horizontalAlignment = Alignment.CenterHorizontally,
  ) {
    Text(
      text = "%,d".format(count),
      color = Teal,
      fontSize = 56.sp,
      fontWeight = FontWeight.Bold,
      fontFamily = FontFamily.Monospace,
      textAlign = TextAlign.Center,
      maxLines = 1,
    )
    Text(
      text = "UNIQUE APs",
      color = DimLabel,
      fontSize = 12.sp,
      fontWeight = FontWeight.Medium,
      letterSpacing = 2.sp,
    )
  }
}

// ---------------------------------------------------------------------------
// Rate strip: pkt/s | speed | channel
// ---------------------------------------------------------------------------
@Composable
private fun RateStrip(state: AppUiState, modifier: Modifier = Modifier) {
  Row(
    modifier = modifier
      .clip(RoundedCornerShape(8.dp))
      .background(CardBg)
      .padding(horizontal = 12.dp, vertical = 8.dp),
    horizontalArrangement = Arrangement.SpaceEvenly,
    verticalAlignment = Alignment.CenterVertically,
  ) {
    MetricCell(
      value = state.packetsPerSec.toString(),
      label = "pkt/s",
      modifier = Modifier.weight(1f),
    )
    VerticalDivider()
    MetricCell(
      value = "%.0f".format(state.phoneSpeedKmh),
      label = "km/h",
      modifier = Modifier.weight(1f),
    )
    VerticalDivider()
    MetricCell(
      value = WifiBand.conciseChannelLabel(state.currentChannel),
      label = if (state.autoHopEnabled) "auto ${state.autoHopAppliedMs}ms" else "${state.hopMs}ms",
      modifier = Modifier.weight(1f),
    )
  }
}

@Composable
private fun MetricCell(value: String, label: String, modifier: Modifier = Modifier) {
  Column(
    modifier = modifier,
    horizontalAlignment = Alignment.CenterHorizontally,
  ) {
    Text(
      text = value,
      color = BrightLabel,
      fontSize = 26.sp,
      fontWeight = FontWeight.Bold,
      fontFamily = FontFamily.Monospace,
      maxLines = 1,
    )
    Text(
      text = label,
      color = DimLabel,
      fontSize = 10.sp,
      maxLines = 1,
    )
  }
}

@Composable
private fun VerticalDivider() {
  Box(
    modifier = Modifier
      .width(1.dp)
      .height(32.dp)
      .background(Color(0xFF333333)),
  )
}

// ---------------------------------------------------------------------------
// Health indicator dots
// ---------------------------------------------------------------------------
@Composable
private fun HealthIndicators(state: AppUiState, modifier: Modifier = Modifier) {
  Row(
    modifier = modifier
      .clip(RoundedCornerShape(8.dp))
      .background(CardBg)
      .padding(horizontal = 12.dp, vertical = 8.dp),
    horizontalArrangement = Arrangement.SpaceEvenly,
    verticalAlignment = Alignment.CenterVertically,
  ) {
    HealthDot(
      label = "BLE",
      color = if (state.connected) Green else Red,
    )
    HealthDot(
      label = "GPS",
      color = if (state.gpsValid) Green else Red,
    )
    HealthDot(
      label = "Phone",
      color = when {
        state.phoneGpsAgeS < 0 -> Red
        state.phoneGpsAgeS <= 3 -> Green
        else -> Amber
      },
    )
    HealthDot(
      label = "Node",
      color = if (state.nodeLinkUp) Green else Red,
    )
    HealthDot(
      label = "Scan",
      color = if (state.scanning) Green else Red,
    )
    HealthDot(
      label = "Log",
      color = when {
        state.loggingEnabled -> Red  // recording red dot
        state.gpxEnabled && state.gpxLogging -> Green
        else -> DimLabel
      },
    )
  }
}

@Composable
private fun HealthDot(label: String, color: Color) {
  Column(
    horizontalAlignment = Alignment.CenterHorizontally,
    verticalArrangement = Arrangement.spacedBy(3.dp),
  ) {
    Box(
      modifier = Modifier
        .size(12.dp)
        .clip(CircleShape)
        .background(color),
    )
    Text(
      text = label,
      color = DimLabel,
      fontSize = 9.sp,
      maxLines = 1,
    )
  }
}

// ---------------------------------------------------------------------------
// GPS detail row
// ---------------------------------------------------------------------------
private fun pdopColor(pdopCenti: Int): Color = when {
  pdopCenti <= 0 -> DimLabel
  pdopCenti <= 200 -> Green      // ≤ 2.0 – excellent
  pdopCenti <= 500 -> Amber      // ≤ 5.0 – moderate
  else -> Red                    // > 5.0 – poor
}

private fun gpsSourceLabel(source: Int): String = when (source) {
  1 -> "UART"
  2 -> "PHONE"
  else -> "NONE"
}

@Composable
private fun GpsDetailRow(state: AppUiState, modifier: Modifier = Modifier) {
  val hdopText = if (state.gpsHdopCenti > 0) "%.2f".format(state.gpsHdopCenti / 100.0) else "—"
  val vdopText = if (state.gpsVdopCenti > 0) "%.2f".format(state.gpsVdopCenti / 100.0) else "—"
  val pdopText = if (state.gpsPdopCenti > 0) "%.2f".format(state.gpsPdopCenti / 100.0) else "—"
  val satInUse = if (state.gpsSatInUse > 0) state.gpsSatInUse else state.gpsSatCount
  val satText = when {
    satInUse > 0 && state.gpsSatInView > 0 -> "$satInUse/${state.gpsSatInView}"
    satInUse > 0 -> satInUse.toString()
    else -> "—"
  }
  val accText = if (state.gpsValid) "±${"%.1f".format(state.gpsAccuracyDm / 10.0)}m" else "—"
  val ageText = if (state.gpsValid) "${state.gpsAgeS}s" else "—"
  val rateText = "${if (state.gpsNavAppliedHz > 0) state.gpsNavAppliedHz else 1} Hz"
  val dopText = "$hdopText/$vdopText/$pdopText"

  Row(
    modifier = modifier
      .clip(RoundedCornerShape(8.dp))
      .background(CardBg)
      .padding(horizontal = 10.dp, vertical = 8.dp),
    horizontalArrangement = Arrangement.SpaceEvenly,
    verticalAlignment = Alignment.CenterVertically,
  ) {
    GpsMini(value = dopText, label = "H/V/P", color = pdopColor(state.gpsPdopCenti))
    GpsMini(value = satText, label = "Use/View", color = BrightLabel)
    GpsMini(value = accText, label = "Acc", color = BrightLabel)
    GpsMini(
      value = gpsSourceLabel(state.gpsSource),
      label = "Src",
      color = when (state.gpsSource) {
        1 -> Teal
        2 -> Amber
        else -> DimLabel
      },
    )
    GpsMini(value = ageText, label = "Age", color = when {
      !state.gpsValid -> DimLabel
      state.gpsAgeS <= 2 -> Green
      state.gpsAgeS <= 5 -> Amber
      else -> Red
    })
    GpsMini(value = rateText, label = "Nav", color = DimLabel)
  }
}

@Composable
private fun GpsMini(value: String, label: String, color: Color) {
  Column(horizontalAlignment = Alignment.CenterHorizontally) {
    Text(
      text = value,
      color = color,
      fontSize = 17.sp,
      fontWeight = FontWeight.Bold,
      fontFamily = FontFamily.Monospace,
      maxLines = 1,
    )
    Text(
      text = label,
      color = DimLabel,
      fontSize = 9.sp,
      maxLines = 1,
    )
  }
}

// ---------------------------------------------------------------------------
// Channel activity bar chart (2.4 GHz)
// ---------------------------------------------------------------------------
@Composable
private fun ChannelChart(
  sightings: List<SightingUi>,
  modifier: Modifier = Modifier,
) {
  // Group by channel, count active APs
  val channelCounts = sightings.groupBy { it.channel }

  // 2.4 GHz counts (channels 1-13)
  val counts24 = (1..13).map { ch -> channelCounts[ch]?.size ?: 0 }
  val max24 = counts24.maxOrNull()?.coerceAtLeast(1) ?: 1

  // 5 GHz total
  val count5Total = sightings.count { WifiBand.fromChannel(it.channel) == WifiBand.BAND_5_GHZ }

  val textMeasurer = rememberTextMeasurer()

  Column(modifier = modifier) {
    // Header
    Row(
      modifier = Modifier.fillMaxWidth(),
      horizontalArrangement = Arrangement.SpaceBetween,
      verticalAlignment = Alignment.CenterVertically,
    ) {
      Text(
        text = "CHANNEL ACTIVITY",
        color = DimLabel,
        fontSize = 10.sp,
        fontWeight = FontWeight.Medium,
        letterSpacing = 1.5.sp,
      )
      if (count5Total > 0) {
        Text(
          text = "5G: $count5Total APs",
          color = TealDim,
          fontSize = 11.sp,
          fontWeight = FontWeight.Bold,
          fontFamily = FontFamily.Monospace,
        )
      }
    }

    Spacer(Modifier.height(4.dp))

    // Canvas bar chart
    Canvas(
      modifier = Modifier
        .fillMaxWidth()
        .fillMaxHeight()
        .clip(RoundedCornerShape(8.dp))
        .background(CardBg)
        .padding(4.dp),
    ) {
      drawChannelBars(
        counts = counts24,
        maxCount = max24,
        textMeasurer = textMeasurer,
      )
    }
  }
}

private fun DrawScope.drawChannelBars(
  counts: List<Int>,
  maxCount: Int,
  textMeasurer: TextMeasurer,
) {
  val barCount = counts.size
  val totalWidth = size.width
  val totalHeight = size.height

  val labelHeight = 16.sp.toPx()
  val countLabelHeight = 12.sp.toPx()
  val chartTop = countLabelHeight + 4f
  val chartHeight = totalHeight - labelHeight - chartTop - 8f
  if (chartHeight <= 0f) return

  val gapRatio = 0.3f
  val slotWidth = totalWidth / barCount
  val barWidth = slotWidth * (1f - gapRatio)
  val gap = slotWidth * gapRatio / 2f

  for (i in counts.indices) {
    val count = counts[i]
    val x = i * slotWidth + gap
    val fraction = if (maxCount > 0) count.toFloat() / maxCount else 0f
    val barHeight = chartHeight * fraction
    val barColor = when {
      count == 0 -> Color(0xFF2A2A2A)
      fraction >= 0.7f -> Teal
      fraction >= 0.4f -> TealDim
      else -> Color(0xFF00695C)
    }

    // Bar
    drawRect(
      color = barColor,
      topLeft = Offset(x, chartTop + chartHeight - barHeight),
      size = Size(barWidth, barHeight.coerceAtLeast(2f)),
    )

    // Count above bar
    if (count > 0) {
      val countText = textMeasurer.measure(
        text = count.toString(),
        style = TextStyle(
          color = BrightLabel,
          fontSize = 10.sp,
          fontWeight = FontWeight.Bold,
          fontFamily = FontFamily.Monospace,
          textAlign = TextAlign.Center,
        ),
      )
      drawText(
        textLayoutResult = countText,
        topLeft = Offset(
          x + (barWidth - countText.size.width) / 2f,
          chartTop + chartHeight - barHeight - countText.size.height - 2f,
        ),
      )
    }

    // Channel label below
    val label = textMeasurer.measure(
      text = (i + 1).toString(),
      style = TextStyle(
        color = if (count > 0) BrightLabel else DimLabel,
        fontSize = 10.sp,
        fontFamily = FontFamily.Monospace,
        textAlign = TextAlign.Center,
      ),
    )
    drawText(
      textLayoutResult = label,
      topLeft = Offset(
        x + (barWidth - label.size.width) / 2f,
        totalHeight - labelHeight,
      ),
    )
  }
}

// ---------------------------------------------------------------------------
// Storage bar
// ---------------------------------------------------------------------------
private fun formatBytes(bytes: Long): String {
  val safe = bytes.coerceAtLeast(0L).toDouble()
  val units = arrayOf("B", "KB", "MB", "GB")
  var value = safe
  var idx = 0
  while (value >= 1024.0 && idx < units.lastIndex) {
    value /= 1024.0
    idx++
  }
  return if (idx == 0) "${value.toLong()} ${units[idx]}"
  else "${"%.1f".format(value)} ${units[idx]}"
}

@Composable
private fun StorageBar(state: AppUiState, modifier: Modifier = Modifier) {
  val total = state.spiffsTotalBytes.coerceAtLeast(1L)
  val used = state.spiffsUsedBytes.coerceAtLeast(0L)
  val fraction = (used.toFloat() / total.toFloat()).coerceIn(0f, 1f)
  val barColor = when {
    fraction >= 0.9f -> Red
    fraction >= 0.75f -> Amber
    else -> TealDim
  }

  Column(modifier = modifier) {
    Row(
      modifier = Modifier.fillMaxWidth(),
      horizontalArrangement = Arrangement.SpaceBetween,
    ) {
      Text(
        text = "STORAGE",
        color = DimLabel,
        fontSize = 9.sp,
        letterSpacing = 1.sp,
      )
      Text(
        text = "${formatBytes(used)} / ${formatBytes(total)}",
        color = BrightLabel,
        fontSize = 10.sp,
        fontFamily = FontFamily.Monospace,
      )
    }
    Spacer(Modifier.height(3.dp))
    Box(
      modifier = Modifier
        .fillMaxWidth()
        .height(6.dp)
        .clip(RoundedCornerShape(3.dp))
        .background(Color(0xFF2A2A2A)),
    ) {
      Box(
        modifier = Modifier
          .fillMaxWidth(fraction)
          .fillMaxHeight()
          .clip(RoundedCornerShape(3.dp))
          .background(barColor),
      )
    }
  }
}

// ---------------------------------------------------------------------------
// Backlog / queue row
// ---------------------------------------------------------------------------
@Composable
private fun BacklogRow(state: AppUiState, modifier: Modifier = Modifier) {
  val text = when {
    state.downloadBacklogActive && state.blobActive && state.blobBytesTotal > 0L -> {
      val pct = ((state.blobBytesSent * 100L) / state.blobBytesTotal).coerceIn(0L, 100L)
      "⬇ ${formatBytes(state.blobBytesSent)}/${formatBytes(state.blobBytesTotal)} ($pct%)"
    }
    state.downloadBacklogActive -> "⬇ Downloading ${state.queuedRecords} rec"
    state.queuedRecords > 0L -> "Q: ${state.queuedRecords} rec / ${formatBytes(state.queuedBytes)}"
    else -> "Q: empty"
  }
  val color = when {
    state.downloadBacklogActive -> Amber
    state.queuedRecords > 0L -> DimLabel
    else -> DimLabel
  }
  Text(
    text = text,
    modifier = modifier,
    color = color,
    fontSize = 10.sp,
    fontFamily = FontFamily.Monospace,
    maxLines = 1,
    overflow = TextOverflow.Ellipsis,
  )
}
