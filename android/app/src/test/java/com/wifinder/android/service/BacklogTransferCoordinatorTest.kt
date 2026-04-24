package com.wifinder.android.service

import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import org.junit.Test

class BacklogTransferCoordinatorTest {

  @Test
  fun `wifi failure transitions to single ble fallback and then complete`() {
    val snapshots = mutableListOf<BacklogTransferSnapshot>()
    val logs = mutableListOf<String>()
    val coordinator = BacklogTransferCoordinator(
      onSnapshot = { snapshots += it },
      onLog = { logs += it },
    )

    assertTrue(coordinator.startWifi())
    assertEquals(BacklogTransferStage.WIFI_ACTIVE, coordinator.currentStage())

    val startedFallback = coordinator.failWifiAndRequestBleFallback("manifest unavailable")
    assertTrue(startedFallback)
    assertEquals(BacklogTransferStage.BLE_FALLBACK_ACTIVE, coordinator.currentStage())

    coordinator.noteBleSessionMeta(sessionId = 0x1234L, totalBytes = 4096L)
    coordinator.noteBleSessionProgress(sessionId = 0x1234L, bytesReceived = 4096L, totalBytes = 4096L)
    coordinator.completeBleFallback("fallback imported")

    assertEquals(BacklogTransferStage.COMPLETE, coordinator.currentStage())
    val last = snapshots.last()
    assertFalse(last.downloadActive)
    assertFalse(last.blobActive)
    assertTrue(logs.any { it.contains("Switching to BLE fallback") })
    assertTrue(logs.any { it.contains("fallback imported") })
  }

  @Test
  fun `only one fallback attempt is allowed`() {
    val coordinator = BacklogTransferCoordinator(
      onSnapshot = {},
      onLog = {},
    )

    assertTrue(coordinator.startWifi())
    assertTrue(coordinator.failWifiAndRequestBleFallback("network timeout"))
    coordinator.failBleFallback("chunk mismatch")

    assertEquals(BacklogTransferStage.FAILED, coordinator.currentStage())
    assertFalse(coordinator.failWifiAndRequestBleFallback("second wifi failure"))
    assertEquals(BacklogTransferStage.FAILED, coordinator.currentStage())
  }

  @Test
  fun `cancel returns transfer to idle`() {
    val coordinator = BacklogTransferCoordinator(
      onSnapshot = {},
      onLog = {},
    )

    assertTrue(coordinator.startWifi())
    coordinator.cancel("user cancelled")
    assertEquals(BacklogTransferStage.IDLE, coordinator.currentStage())
  }
}
