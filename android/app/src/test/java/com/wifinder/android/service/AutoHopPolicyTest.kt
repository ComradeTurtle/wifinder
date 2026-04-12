package com.wifinder.android.service

import kotlin.test.assertEquals
import kotlin.test.assertTrue
import org.junit.Test

class AutoHopPolicyTest {
  @Test
  fun `compute auto hop keeps base when stationary`() {
    assertEquals(150, ScannerService.computeAutoHopMs(baseHopMs = 150, speedMmps = 0))
  }

  @Test
  fun `compute auto hop reduces with speed and respects dynamic floor`() {
    val citySpeed = ScannerService.computeAutoHopMs(baseHopMs = 150, speedMmps = 13_888) // ~50 km/h
    val highwaySpeed = ScannerService.computeAutoHopMs(baseHopMs = 150, speedMmps = 27_777) // ~100 km/h
    val cappedSpeed = ScannerService.computeAutoHopMs(baseHopMs = 150, speedMmps = 80_000)

    assertTrue(citySpeed in 100..130)
    assertTrue(highwaySpeed in 70..95)
    assertEquals(70, cappedSpeed)
  }

  @Test
  fun `small bases never get forced above base`() {
    assertEquals(60, ScannerService.computeAutoHopMs(baseHopMs = 60, speedMmps = 80_000))
    assertEquals(50, ScannerService.computeAutoHopMs(baseHopMs = 50, speedMmps = 80_000))
  }
}
