package com.wifinder.android.service

enum class BacklogTransferStage {
  IDLE,
  WIFI_ACTIVE,
  BLE_FALLBACK_ACTIVE,
  COMPLETE,
  FAILED,
}

data class BacklogTransferSnapshot(
  val stage: BacklogTransferStage,
  val downloadActive: Boolean,
  val blobActive: Boolean,
  val blobSessionId: Long,
  val blobBytesSent: Long,
  val blobBytesTotal: Long,
  val failureReason: String?,
)

class BacklogTransferCoordinator(
  private val onSnapshot: (BacklogTransferSnapshot) -> Unit,
  private val onLog: (String) -> Unit,
) {
  private var stage: BacklogTransferStage = BacklogTransferStage.IDLE
  private var fallbackAttempted: Boolean = false
  private var blobSessionId: Long = 0L
  private var blobBytesSent: Long = 0L
  private var blobBytesTotal: Long = 0L
  private var failureReason: String? = null

  init {
    emitSnapshot()
  }

  @Synchronized
  fun currentStage(): BacklogTransferStage = stage

  @Synchronized
  fun isActive(): Boolean =
    stage == BacklogTransferStage.WIFI_ACTIVE || stage == BacklogTransferStage.BLE_FALLBACK_ACTIVE

  @Synchronized
  fun isWifiActive(): Boolean = stage == BacklogTransferStage.WIFI_ACTIVE

  @Synchronized
  fun isBleFallbackActive(): Boolean = stage == BacklogTransferStage.BLE_FALLBACK_ACTIVE

  @Synchronized
  fun startWifi(): Boolean {
    if (isActive()) {
      return false
    }
    stage = BacklogTransferStage.WIFI_ACTIVE
    fallbackAttempted = false
    failureReason = null
    blobSessionId = 0L
    blobBytesSent = 0L
    blobBytesTotal = 0L
    onLog("Backlog download started (Wi-Fi primary)")
    emitSnapshot()
    return true
  }

  @Synchronized
  fun failWifiAndRequestBleFallback(reason: String): Boolean {
    if (stage != BacklogTransferStage.WIFI_ACTIVE) {
      return false
    }
    onLog("Backlog Wi-Fi failed: $reason")
    if (fallbackAttempted) {
      stage = BacklogTransferStage.FAILED
      failureReason = reason
      emitSnapshot()
      onLog("Backlog download failed")
      return false
    }
    fallbackAttempted = true
    stage = BacklogTransferStage.BLE_FALLBACK_ACTIVE
    blobSessionId = 0L
    blobBytesSent = 0L
    blobBytesTotal = 0L
    failureReason = null
    emitSnapshot()
    onLog("Switching to BLE fallback")
    return true
  }

  @Synchronized
  fun noteBleSessionMeta(sessionId: Long, totalBytes: Long) {
    if (stage != BacklogTransferStage.BLE_FALLBACK_ACTIVE &&
      stage != BacklogTransferStage.WIFI_ACTIVE) {
      return
    }
    blobSessionId = sessionId
    blobBytesSent = 0L
    blobBytesTotal = totalBytes.coerceAtLeast(0L)
    emitSnapshot()
  }

  @Synchronized
  fun noteBleSessionProgress(sessionId: Long, bytesReceived: Long, totalBytes: Long) {
    if (stage != BacklogTransferStage.BLE_FALLBACK_ACTIVE &&
      stage != BacklogTransferStage.WIFI_ACTIVE) {
      return
    }
    blobSessionId = sessionId
    blobBytesSent = bytesReceived.coerceAtLeast(0L)
    blobBytesTotal = totalBytes.coerceAtLeast(0L)
    emitSnapshot()
  }

  @Synchronized
  fun completeWifi(message: String = "Backlog Wi-Fi download complete") {
    if (stage != BacklogTransferStage.WIFI_ACTIVE) {
      return
    }
    stage = BacklogTransferStage.COMPLETE
    failureReason = null
    blobSessionId = 0L
    blobBytesSent = 0L
    blobBytesTotal = 0L
    emitSnapshot()
    onLog(message)
  }

  @Synchronized
  fun completeBleFallback(message: String = "Backlog BLE fallback complete") {
    if (stage != BacklogTransferStage.BLE_FALLBACK_ACTIVE) {
      return
    }
    stage = BacklogTransferStage.COMPLETE
    failureReason = null
    blobSessionId = 0L
    blobBytesSent = 0L
    blobBytesTotal = 0L
    emitSnapshot()
    onLog(message)
  }

  @Synchronized
  fun failBleFallback(reason: String) {
    if (stage != BacklogTransferStage.BLE_FALLBACK_ACTIVE) {
      return
    }
    stage = BacklogTransferStage.FAILED
    failureReason = reason
    blobSessionId = 0L
    blobBytesSent = 0L
    blobBytesTotal = 0L
    emitSnapshot()
    onLog("Backlog fallback failed: $reason")
  }

  @Synchronized
  fun failTransfer(reason: String, logMessage: String = "Backlog download failed: $reason") {
    if (stage == BacklogTransferStage.IDLE) {
      return
    }
    stage = BacklogTransferStage.FAILED
    failureReason = reason
    blobSessionId = 0L
    blobBytesSent = 0L
    blobBytesTotal = 0L
    emitSnapshot()
    onLog(logMessage)
  }

  @Synchronized
  fun cancel(message: String) {
    if (stage == BacklogTransferStage.IDLE) {
      return
    }
    stage = BacklogTransferStage.IDLE
    failureReason = null
    blobSessionId = 0L
    blobBytesSent = 0L
    blobBytesTotal = 0L
    emitSnapshot()
    onLog(message)
  }

  @Synchronized
  fun resetIdle() {
    stage = BacklogTransferStage.IDLE
    fallbackAttempted = false
    failureReason = null
    blobSessionId = 0L
    blobBytesSent = 0L
    blobBytesTotal = 0L
    emitSnapshot()
  }

  private fun emitSnapshot() {
    onSnapshot(
      BacklogTransferSnapshot(
        stage = stage,
        downloadActive = stage == BacklogTransferStage.WIFI_ACTIVE ||
          stage == BacklogTransferStage.BLE_FALLBACK_ACTIVE,
        blobActive = stage == BacklogTransferStage.WIFI_ACTIVE ||
          stage == BacklogTransferStage.BLE_FALLBACK_ACTIVE,
        blobSessionId = blobSessionId,
        blobBytesSent = blobBytesSent,
        blobBytesTotal = blobBytesTotal,
        failureReason = failureReason,
      ),
    )
  }
}
