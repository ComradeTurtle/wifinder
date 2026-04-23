package com.wifinder.android.ble

import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue
import org.junit.Test

class BleTargetingPolicyTest {

  @Test
  fun `service uuid is required and name guard is secondary`() {
    assertFalse(BleTargetingPolicy.isEligibleCandidate(hasServiceUuid = false, advertisedName = "WIFINDER-C6"))
    assertFalse(BleTargetingPolicy.isEligibleCandidate(hasServiceUuid = true, advertisedName = "OtherDevice"))
    assertTrue(BleTargetingPolicy.isEligibleCandidate(hasServiceUuid = true, advertisedName = "WIFINDER-S3"))
    assertTrue(BleTargetingPolicy.isEligibleCandidate(hasServiceUuid = true, advertisedName = null))
  }

  @Test
  fun `remembered address is prioritized for auto-connect`() {
    val candidates = listOf(
      BleDeviceCandidate(address = "AA:BB:CC:DD:EE:01", name = "WIFINDER-1", rssi = -50),
      BleDeviceCandidate(address = "AA:BB:CC:DD:EE:02", name = "WIFINDER-2", rssi = -40),
    )

    val chosen = BleTargetingPolicy.chooseAutoConnect(candidates, preferredAddress = "aa:bb:cc:dd:ee:01")

    assertEquals("AA:BB:CC:DD:EE:01", chosen?.address)
  }

  @Test
  fun `no preferred device auto-connects only when single candidate exists`() {
    val one = listOf(BleDeviceCandidate(address = "AA:BB:CC:DD:EE:01", name = "WIFINDER-1", rssi = -55))
    val many = listOf(
      BleDeviceCandidate(address = "AA:BB:CC:DD:EE:01", name = "WIFINDER-1", rssi = -55),
      BleDeviceCandidate(address = "AA:BB:CC:DD:EE:02", name = "WIFINDER-2", rssi = -40),
    )

    assertEquals("AA:BB:CC:DD:EE:01", BleTargetingPolicy.chooseAutoConnect(one, preferredAddress = null)?.address)
    assertNull(BleTargetingPolicy.chooseAutoConnect(many, preferredAddress = null))
  }

  @Test
  fun `sorted candidates place remembered first`() {
    val sorted = BleTargetingPolicy.sortCandidates(
      candidates = listOf(
        BleDeviceCandidate(address = "AA:BB:CC:DD:EE:01", name = "WIFINDER-1", rssi = -55),
        BleDeviceCandidate(address = "AA:BB:CC:DD:EE:02", name = "WIFINDER-2", rssi = -40),
      ),
      preferredAddress = "AA:BB:CC:DD:EE:01",
    )

    assertEquals("AA:BB:CC:DD:EE:01", sorted.first().address)
    assertTrue(sorted.first().remembered)
  }
}
