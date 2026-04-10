package com.espwigle.android.model

import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import org.junit.Test

class GpxTrackFormatterTest {
  @Test
  fun `header contains gpx track envelope`() {
    val header = GpxTrackFormatter.header(trackName = "session-123")

    assertTrue(header.contains("<gpx"))
    assertTrue(header.contains("<trk>"))
    assertTrue(header.contains("<name>session-123</name>"))
    assertTrue(header.contains("<trkseg>"))
  }

  @Test
  fun `track point formats as gpx trkpt with utc timestamp`() {
    val point = GpxTrackFormatter.trackPoint(
      lat = 37.9838,
      lon = 23.7275,
      unixTimeMs = 1_700_000_000_000L,
      altitudeMeters = 120.5,
      hdop = 0.92,
      satCount = 11,
    )

    assertTrue(point.startsWith("<trkpt lat=\"37.9838000\" lon=\"23.7275000\">"))
    assertTrue(point.contains("<time>2023-11-14T22:13:20Z</time>"))
    assertTrue(point.contains("<ele>120.5</ele>"))
    assertTrue(point.contains("<hdop>0.92</hdop>"))
    assertTrue(point.contains("<sat>11</sat>"))
    assertTrue(point.endsWith("</trkpt>"))
  }

  @Test
  fun `footer closes track envelope`() {
    assertEquals("</trkseg></trk></gpx>", GpxTrackFormatter.footer())
  }

  @Test
  fun `sampling emits first point then gates by time or distance`() {
    val first = GpxTrackSample(lat = 37.9838, lon = 23.7275, unixTimeMs = 1_000L)
    val tooSoonNear = GpxTrackSample(lat = 37.983801, lon = 23.727501, unixTimeMs = 1_500L)
    val timeReached = GpxTrackSample(lat = 37.983801, lon = 23.727501, unixTimeMs = 2_000L)
    val farEnough = GpxTrackSample(lat = 37.9839, lon = 23.7275, unixTimeMs = 1_500L)

    assertTrue(GpxTrackSampling.shouldEmit(previous = null, candidate = first, minElapsedMs = 1000L, minDistanceMeters = 5.0))
    assertFalse(GpxTrackSampling.shouldEmit(previous = first, candidate = tooSoonNear, minElapsedMs = 1000L, minDistanceMeters = 5.0))
    assertTrue(GpxTrackSampling.shouldEmit(previous = first, candidate = timeReached, minElapsedMs = 1000L, minDistanceMeters = 5.0))
    assertTrue(GpxTrackSampling.shouldEmit(previous = first, candidate = farEnough, minElapsedMs = 1000L, minDistanceMeters = 5.0))
  }
}
