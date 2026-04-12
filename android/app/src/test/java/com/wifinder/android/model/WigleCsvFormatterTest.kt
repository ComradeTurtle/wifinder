package com.wifinder.android.model

import kotlin.test.assertEquals
import kotlin.test.assertTrue
import org.junit.Test

class WigleCsvFormatterTest {
  @Test
  fun `header matches wigle 1_6 format`() {
    val header = WigleCsvFormatter.header("wifinder-android")

    assertTrue(header[0].startsWith("WigleWifi-1.6,appRelease=wifinder-android"))
    assertTrue(header[0].contains("star=Sol,body=3,subBody=0"))
    assertEquals(
      "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type",
      header[1],
    )
  }

  @Test
  fun `format row includes frequency and empty rcois mfgrid`() {
    val row = WigleCsvFormatter.formatRow(
      WigleWifiRow(
        mac = "40:ED:00:25:E0:02",
        ssid = "Red, \"Dragon\"",
        authMode = "WPA2-PSK-CCMP-128",
        firstSeenEpochMs = 1_700_000_000_000,
        channel = 6,
        rssi = -54,
        latitude = 37.9838,
        longitude = 23.7275,
        altitudeMeters = 120.4,
        accuracyMeters = 3.1,
        type = "WIFI",
      ),
    )

    assertTrue(row.contains("\"Red, \"\"Dragon\"\"\""))
    assertTrue(row.contains("40:ED:00:25:E0:02"))
    assertTrue(row.contains("[WPA2-PSK-CCMP-128][ESS]"))
    assertTrue(row.contains(",2437,"))  // channel 6 → 2437 MHz
    assertTrue(row.endsWith(",WIFI"))
  }

  @Test
  fun `channel to frequency maps correctly`() {
    // Channel 1 → 2412 MHz
    val row1 = WigleCsvFormatter.formatRow(
      WigleWifiRow(
        mac = "AA:BB:CC:DD:EE:FF", ssid = "test", authMode = "Open",
        firstSeenEpochMs = 0, channel = 1, rssi = -50,
        latitude = 0.0, longitude = 0.0, altitudeMeters = 0.0, accuracyMeters = 0.0,
      ),
    )
    assertTrue(row1.contains(",2412,"))

    // Channel 36 → 5180 MHz
    val row36 = WigleCsvFormatter.formatRow(
      WigleWifiRow(
        mac = "AA:BB:CC:DD:EE:FF", ssid = "test", authMode = "Open",
        firstSeenEpochMs = 0, channel = 36, rssi = -50,
        latitude = 0.0, longitude = 0.0, altitudeMeters = 0.0, accuracyMeters = 0.0,
      ),
    )
    assertTrue(row36.contains(",5180,"))
  }

  @Test
  fun `open auth becomes ESS only`() {
    val row = WigleCsvFormatter.formatRow(
      WigleWifiRow(
        mac = "AA:BB:CC:DD:EE:FF", ssid = "open", authMode = "OPEN",
        firstSeenEpochMs = 0, channel = 1, rssi = -50,
        latitude = 0.0, longitude = 0.0, altitudeMeters = 0.0, accuracyMeters = 0.0,
      ),
    )
    assertTrue(row.contains("[ESS]"))
    assertTrue(!row.contains("[OPEN][ESS]"))
  }

  @Test
  fun `transitional wpa2_wpa3 auth converts to android capabilities style`() {
    val row = WigleCsvFormatter.formatRow(
      WigleWifiRow(
        mac = "AA:BB:CC:DD:EE:FF", ssid = "secured", authMode = "WPA2/WPA3-PSK/SAE-CCMP-128",
        firstSeenEpochMs = 0, channel = 36, rssi = -50,
        latitude = 0.0, longitude = 0.0, altitudeMeters = 0.0, accuracyMeters = 0.0,
      ),
    )
    assertTrue(row.contains("[WPA2-PSK-CCMP-128][WPA3-SAE-CCMP-128][ESS]"))
    assertTrue(!row.contains("[WPA2/WPA3-PSK/SAE-CCMP-128][ESS]"))
  }

  @Test
  fun `pre_header fields are csv escaped`() {
    val header = WigleCsvFormatter.header(
      appRelease = "wifinder-android",
      brand = "Moog, LLC",
      model = "X\"37B",
    )
    assertTrue(header[0].contains("\"brand=Moog, LLC\""))
    assertTrue(header[0].contains("\"model=X\"\"37B\""))
  }
}
