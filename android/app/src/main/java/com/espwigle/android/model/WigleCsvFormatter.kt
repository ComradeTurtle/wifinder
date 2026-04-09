package com.espwigle.android.model

import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

object WigleCsvFormatter {
  const val WIFI_HEADER =
    "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type"

  private val timestampFormat =
    SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).apply {
      timeZone = TimeZone.getTimeZone("UTC")
    }

  fun header(
    appRelease: String,
    model: String = "unknown",
    release: String = "unknown",
    device: String = "unknown",
    display: String = "unknown",
    board: String = "unknown",
    brand: String = "unknown",
  ): List<String> =
    listOf(preHeader(appRelease, model, release, device, display, board, brand), WIFI_HEADER)

  fun formatRow(row: WigleWifiRow): String {
    val firstSeen = timestampFormat.format(Date(row.firstSeenEpochMs))
    val freq = row.frequency ?: (WifiBand.frequencyMhzFromChannel(row.channel) ?: 0)
    return listOf(
      csvEscape(row.mac),
      csvEscape(row.ssid),
      csvEscape(formatAuthMode(row.authMode)),
      csvEscape(firstSeen),
      row.channel.toString(),
      freq.toString(),
      row.rssi.toString(),
      "%.7f".format(Locale.US, row.latitude),
      "%.7f".format(Locale.US, row.longitude),
      "%.1f".format(Locale.US, row.altitudeMeters),
      "%.1f".format(Locale.US, row.accuracyMeters),
      csvEscape(row.rcois),
      csvEscape(row.mfgrId),
      csvEscape(row.type),
    ).joinToString(",")
  }

  private fun formatAuthMode(auth: String): String {
    val trimmed = auth.trim()
    if (trimmed.isBlank()) return "[ESS]"

    // Already in Android/WiGLE capabilities style.
    if (trimmed.startsWith("[")) {
      return if (trimmed.contains("[ESS]")) trimmed else "$trimmed[ESS]"
    }

    if (trimmed.equals("open", ignoreCase = true)) return "[ESS]"

    val firstDash = trimmed.indexOf('-')
    val protoPart = if (firstDash < 0) trimmed else trimmed.substring(0, firstDash)
    val rest = if (firstDash < 0) "" else trimmed.substring(firstDash + 1)
    val secondDash = rest.indexOf('-')
    val akmPart = when {
      rest.isBlank() -> ""
      secondDash < 0 -> rest
      else -> rest.substring(0, secondDash)
    }
    val cipherPart = when {
      rest.isBlank() || secondDash < 0 -> ""
      else -> rest.substring(secondDash + 1)
    }

    val protos = protoPart.split('/').map { it.trim() }.filter { it.isNotBlank() }
    val akms = akmPart.split('/').map { it.trim() }.filter { it.isNotBlank() }

    val capabilities = mutableListOf<String>()
    if (protos.isNotEmpty()) {
      when {
        akms.size == protos.size && protos.size > 1 -> {
          for (i in protos.indices) {
            capabilityToken(protos[i], akms[i], cipherPart)?.let(capabilities::add)
          }
        }

        akms.size <= 1 -> {
          val akm = akms.firstOrNull().orEmpty()
          for (proto in protos) {
            capabilityToken(proto, akm, cipherPart)?.let(capabilities::add)
          }
        }

        protos.size == 1 -> {
          for (akm in akms) {
            capabilityToken(protos[0], akm, cipherPart)?.let(capabilities::add)
          }
        }

        else -> capabilityToken(trimmed, "", "")?.let(capabilities::add)
      }
    }

    val uniqueCaps = capabilities.distinct()
    if (uniqueCaps.isEmpty()) return "[ESS]"
    return uniqueCaps.joinToString(separator = "", transform = { "[$it]" }) + "[ESS]"
  }

  private fun csvEscape(input: String): String {
    val needsQuotes = input.contains(',') || input.contains('"') || input.contains('\n')
    if (!needsQuotes) {
      return input
    }
    return '"' + input.replace("\"", "\"\"") + '"'
  }

  private fun preHeader(
    appRelease: String,
    model: String,
    release: String,
    device: String,
    display: String,
    board: String,
    brand: String,
  ): String =
    listOf(
      "WigleWifi-1.6",
      preHeaderField("appRelease", appRelease),
      preHeaderField("model", model),
      preHeaderField("release", release),
      preHeaderField("device", device),
      preHeaderField("display", display),
      preHeaderField("board", board),
      preHeaderField("brand", brand),
      preHeaderField("star", "Sol"),
      preHeaderField("body", "3"),
      preHeaderField("subBody", "0"),
    ).joinToString(",")

  private fun preHeaderField(key: String, value: String): String = csvEscape("$key=$value")

  private fun capabilityToken(protoRaw: String, akmRaw: String, cipherRaw: String): String? {
    val proto = protoRaw.trim().uppercase(Locale.US)
    if (proto.isBlank() || proto == "OPEN") return null

    val parts = mutableListOf(proto)
    val akm = akmRaw.trim().uppercase(Locale.US)
    val cipher = cipherRaw.trim().uppercase(Locale.US)
    if (akm.isNotBlank()) parts += akm
    if (cipher.isNotBlank()) parts += cipher
    return parts.joinToString("-")
  }
}

data class WigleWifiRow(
  val mac: String,
  val ssid: String,
  val authMode: String,
  val firstSeenEpochMs: Long,
  val channel: Int,
  val frequency: Int? = null,
  val rssi: Int,
  val latitude: Double,
  val longitude: Double,
  val altitudeMeters: Double,
  val accuracyMeters: Double,
  val rcois: String = "",
  val mfgrId: String = "",
  val type: String = "WIFI",
)
