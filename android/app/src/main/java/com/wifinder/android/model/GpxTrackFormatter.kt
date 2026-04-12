package com.wifinder.android.model

import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

data class GpxTrackSample(
  val lat: Double,
  val lon: Double,
  val unixTimeMs: Long,
)

object GpxTrackSampling {
  private const val EARTH_RADIUS_METERS = 6_371_000.0

  fun shouldEmit(
    previous: GpxTrackSample?,
    candidate: GpxTrackSample,
    minElapsedMs: Long,
    minDistanceMeters: Double,
  ): Boolean {
    if (previous == null) return true
    val elapsed = candidate.unixTimeMs - previous.unixTimeMs
    if (elapsed >= minElapsedMs) return true
    return distanceMeters(previous, candidate) >= minDistanceMeters
  }

  fun distanceMeters(a: GpxTrackSample, b: GpxTrackSample): Double {
    val lat1 = Math.toRadians(a.lat)
    val lat2 = Math.toRadians(b.lat)
    val dLat = Math.toRadians(b.lat - a.lat)
    val dLon = Math.toRadians(b.lon - a.lon)
    val sinDLat = Math.sin(dLat / 2.0)
    val sinDLon = Math.sin(dLon / 2.0)
    val h = sinDLat * sinDLat + Math.cos(lat1) * Math.cos(lat2) * sinDLon * sinDLon
    val c = 2.0 * Math.atan2(Math.sqrt(h), Math.sqrt(1.0 - h))
    return EARTH_RADIUS_METERS * c
  }
}

object GpxTrackFormatter {
  fun header(trackName: String): String {
    val safeName = xmlEscape(trackName)
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" +
      "<gpx version=\"1.1\" creator=\"wifinder-android\" xmlns=\"http://www.topografix.com/GPX/1/1\">" +
      "<trk><name>$safeName</name><trkseg>"
  }

  fun trackPoint(
    lat: Double,
    lon: Double,
    unixTimeMs: Long,
    altitudeMeters: Double? = null,
    hdop: Double? = null,
    satCount: Int? = null,
  ): String {
    val latStr = String.format(Locale.US, "%.7f", lat)
    val lonStr = String.format(Locale.US, "%.7f", lon)
    val timeStr = formatUtcTime(unixTimeMs)
    val body = StringBuilder()
    body.append("<trkpt lat=\"").append(latStr).append("\" lon=\"").append(lonStr).append("\">")
    body.append("<time>").append(timeStr).append("</time>")
    if (altitudeMeters != null) {
      body.append("<ele>").append(String.format(Locale.US, "%.1f", altitudeMeters)).append("</ele>")
    }
    if (hdop != null) {
      body.append("<hdop>").append(String.format(Locale.US, "%.2f", hdop)).append("</hdop>")
    }
    if (satCount != null && satCount > 0) {
      body.append("<sat>").append(satCount).append("</sat>")
    }
    body.append("</trkpt>")
    return body.toString()
  }

  fun footer(): String = "</trkseg></trk></gpx>"

  fun formatUtcTime(unixTimeMs: Long): String {
    val formatter = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US)
    formatter.timeZone = TimeZone.getTimeZone("UTC")
    return formatter.format(Date(unixTimeMs))
  }

  private fun xmlEscape(value: String): String =
    value
      .replace("&", "&amp;")
      .replace("<", "&lt;")
      .replace(">", "&gt;")
      .replace("\"", "&quot;")
      .replace("'", "&apos;")
}
