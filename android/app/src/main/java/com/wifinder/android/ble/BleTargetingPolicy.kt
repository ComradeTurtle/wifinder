package com.wifinder.android.ble

import java.util.Locale

data class BleDeviceCandidate(
  val address: String,
  val name: String,
  val rssi: Int,
  val remembered: Boolean = false,
)

object BleTargetingPolicy {
  private const val NAME_PREFIX = "WIFINDER"

  fun normalizeAddress(address: String?): String? {
    if (address.isNullOrBlank()) {
      return null
    }
    return address.trim().uppercase(Locale.US)
  }

  fun isNameCompatible(advertisedName: String?): Boolean {
    if (advertisedName.isNullOrBlank()) {
      return true
    }
    return advertisedName.trim().uppercase(Locale.US).startsWith(NAME_PREFIX)
  }

  fun isEligibleCandidate(hasServiceUuid: Boolean, advertisedName: String?): Boolean {
    if (!hasServiceUuid) {
      return false
    }
    return isNameCompatible(advertisedName)
  }

  fun sortCandidates(
    candidates: Collection<BleDeviceCandidate>,
    preferredAddress: String?,
  ): List<BleDeviceCandidate> {
    val normalizedPreferred = normalizeAddress(preferredAddress)
    return candidates
      .mapNotNull { candidate ->
        val normalizedAddress = normalizeAddress(candidate.address) ?: return@mapNotNull null
        candidate.copy(
          address = normalizedAddress,
          remembered = normalizedPreferred != null && normalizedAddress == normalizedPreferred,
        )
      }
      .distinctBy { it.address }
      .sortedWith(
        compareByDescending<BleDeviceCandidate> { it.remembered }
          .thenByDescending { it.rssi }
          .thenBy { it.name }
          .thenBy { it.address },
      )
  }

  fun chooseAutoConnect(
    candidates: List<BleDeviceCandidate>,
    preferredAddress: String?,
  ): BleDeviceCandidate? {
    if (candidates.isEmpty()) {
      return null
    }
    val normalizedPreferred = normalizeAddress(preferredAddress)
    if (normalizedPreferred != null) {
      candidates.firstOrNull { normalizeAddress(it.address) == normalizedPreferred }?.let { return it }
    }
    return if (candidates.size == 1) candidates.first() else null
  }
}
