package com.espwigle.android.model

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.StandardCharsets
import kotlin.math.min

object WgProtocol {
  const val PROTOCOL_VERSION = 2
  private const val MAGIC0 = 0x57
  private const val MAGIC1 = 0x47
  const val HEADER_SIZE = 12

  object MessageType {
    const val STATUS = 0x01
    const val SIGHTING = 0x02
    const val ACK = 0x03
    const val ERROR = 0x04
    const val SNAPSHOT_END = 0x05
    const val CONFIG = 0x06
    const val GPS = 0x07
    const val REPLAY_BATCH = 0x08
    const val REPLAY_ACK = REPLAY_BATCH
    const val NODE_TABLE = 0x09
    const val BACKLOG_BLOB_META = 0x0A
    const val BACKLOG_BLOB_CHUNK = 0x0B
    const val BACKLOG_BLOB_DONE = 0x0C
    const val COMMAND = 0x81
  }

  object Command {
    const val START = 0x01
    const val STOP = 0x02
    const val SET_HOP_MS = 0x03
    const val SET_CHANNEL_MASK = 0x04
    const val SET_BOOT_MODE = 0x05
    const val REQUEST_STATUS = 0x06
    const val REQUEST_SNAPSHOT = 0x07
    const val SET_GPS_FIX = 0x08
    const val REPLAY_ACK = 0x09
    const val SET_REPLAY = 0x0A
    const val CLEAR_STORAGE = 0x0B
    const val SET_GPS_NAV_RATE = 0x0C
    const val SET_BACKLOG_BLOB = 0x0D
    const val DEBUG_SEED_STORAGE = 0x0E
    const val SET_CHANNEL_PLAN = 0x0F
    const val BACKLOG_BLOB_CHUNK_REPLY = 0x10
  }

  const val GPS_FLAG_VALID = 1 shl 0
  const val GPS_FLAG_HAS_ALT = 1 shl 1
  const val GPS_FLAG_HAS_SPEED = 1 shl 2
  const val GPS_FLAG_HAS_BEARING = 1 shl 3

  const val GPS_FIX_PAYLOAD_SIZE = 27

  const val SEC_PROTO_WPA = 1 shl 0
  const val SEC_PROTO_WPA2 = 1 shl 1
  const val SEC_PROTO_WPA3 = 1 shl 2

  const val SEC_AKM_EAP = 1 shl 0
  const val SEC_AKM_PSK = 1 shl 1
  const val SEC_AKM_SAE = 1 shl 2
  const val SEC_AKM_OWE = 1 shl 3

  const val SEC_CIPHER_TKIP = 1 shl 0
  const val SEC_CIPHER_CCMP_128 = 1 shl 1
  const val SEC_CIPHER_GCMP_128 = 1 shl 2
  const val SEC_CIPHER_GCMP_256 = 1 shl 3
  const val SEC_CIPHER_CCMP_256 = 1 shl 4
  const val SEC_CIPHER_WEP = 1 shl 5

  const val SIGHTING_FLAG_NEW = 1 shl 0
  const val SIGHTING_FLAG_SNAPSHOT = 1 shl 1
  const val SIGHTING_FLAG_UPDATED = 1 shl 2

  const val SIGHTING_SOURCE_LIVE = 1 shl 0
  const val SIGHTING_SOURCE_REPLAY = 1 shl 1

  fun encodeCommandFrame(
    seq: Int,
    deviceMs: Long,
    commandId: Int,
    payload: ByteArray = byteArrayOf(),
  ): ByteArray {
    val commandPayload = ByteArray(1 + payload.size)
    commandPayload[0] = (commandId and 0xFF).toByte()
    payload.copyInto(commandPayload, destinationOffset = 1)

    val out = ByteArray(HEADER_SIZE + commandPayload.size)
    val bb = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN)
    bb.put(MAGIC0.toByte())
    bb.put(MAGIC1.toByte())
    bb.put(PROTOCOL_VERSION.toByte())
    bb.put(MessageType.COMMAND.toByte())
    bb.putShort((seq and 0xFFFF).toShort())
    bb.putShort(commandPayload.size.toShort())
    bb.putInt((deviceMs and 0xFFFF_FFFFL).toInt())
    bb.put(commandPayload)
    return out
  }

  fun decodeFrame(bytes: ByteArray): WgFrame {
    require(bytes.size >= HEADER_SIZE) { "Frame too short" }
    require((bytes[0].toInt() and 0xFF) == MAGIC0 && (bytes[1].toInt() and 0xFF) == MAGIC1) {
      "Bad frame magic"
    }

    val bb = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
    bb.position(2)
    val version = bb.get().toInt() and 0xFF
    val type = bb.get().toInt() and 0xFF
    val seq = bb.short.toInt() and 0xFFFF
    val len = bb.short.toInt() and 0xFFFF
    val deviceMs = bb.int.toLong() and 0xFFFF_FFFFL

    require(bytes.size >= HEADER_SIZE + len) { "Truncated frame" }
    val payload = bytes.copyOfRange(HEADER_SIZE, HEADER_SIZE + len)
    return WgFrame(version, type, seq, len, deviceMs, payload)
  }

  fun decodeStatusPayload(payload: ByteArray): StatusPayload {
    require(payload.size >= 14) { "Bad status payload" }
    val bb = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
    val scanning = (payload[0].toInt() and 0xFF) == 1
    val bleEncrypted = (payload[1].toInt() and 0xFF) == 1
    val currentChannel = payload[2].toInt() and 0xFF
    val hopMs = bb.getShort(3).toInt() and 0xFFFF
    val channelMask = bb.getShort(5).toInt() and 0xFFFF
    val legacyUniqueBssids = bb.getShort(7).toInt() and 0xFFFF
    val uniqueBssids =
      if (payload.size >= 61) bb.getInt(57) else legacyUniqueBssids
    val packetsPerSec = bb.getShort(9).toInt() and 0xFFFF
    val droppedNotifies = bb.getShort(11).toInt() and 0xFFFF
    val bootMode = payload[13].toInt() and 0xFF
    val gpsValid = payload.size >= 15 && (payload[14].toInt() and 0xFF) == 1
    val gpsAgeS = if (payload.size >= 17) bb.getShort(15).toInt() and 0xFFFF else 0
    val gpsAccuracyDm = if (payload.size >= 19) bb.getShort(17).toInt() and 0xFFFF else 0
    val nodeLinkUp = payload.size >= 20 && (payload[19].toInt() and 0xFF) == 1
    val nodeLastSeenS = if (payload.size >= 22) bb.getShort(20).toInt() and 0xFFFF else 0
    val nodePacketsPerSec = if (payload.size >= 24) bb.getShort(22).toInt() and 0xFFFF else 0
    val nodeForwardedSightings = if (payload.size >= 26) bb.getShort(24).toInt() and 0xFFFF else 0
    val nodeChannel = if (payload.size >= 27) payload[26].toInt() and 0xFF else 0
    val nodeChannelMask = if (payload.size >= 29) bb.getShort(27).toInt() and 0xFFFF else 0
    val localChannelMask =
      if (payload.size >= 117) bb.getShort(115).toInt() and 0xFFFF else channelMask
    val nodeChannelMask24 =
      if (payload.size >= 119) bb.getShort(117).toInt() and 0xFFFF else nodeChannelMask
    val nodeChannelMask5Ghz = if (payload.size >= 127) bb.getLong(119) else 0L
    val sessionOpen = payload.size >= 30 && (payload[29].toInt() and 0xFF) == 1
    val sessionId = if (payload.size >= 38) bb.getLong(30) else 0L
    val queuedRecords = if (payload.size >= 42) bb.getInt(38).toLong() and 0xFFFF_FFFFL else 0L
    val queuedBytes = if (payload.size >= 46) bb.getInt(42).toLong() and 0xFFFF_FFFFL else 0L
    val replayActive = payload.size >= 47 && (payload[46].toInt() and 0xFF) == 1
    val replayCursor = if (payload.size >= 51) bb.getInt(47).toLong() and 0xFFFF_FFFFL else 0L
    val queueFull = payload.size >= 52 && (payload[51].toInt() and 0xFF) == 1
    val droppedFlashFull = if (payload.size >= 56) bb.getInt(52).toLong() and 0xFFFF_FFFFL else 0L
    val nodeCount = if (payload.size >= 57) payload[56].toInt() and 0xFF else 0
    val gpsNavAppliedHz = if (payload.size >= 62) payload[61].toInt() and 0xFF else 0
    val spiffsTotalLegacy = if (payload.size >= 66) bb.getInt(62).toLong() and 0xFFFF_FFFFL else 0L
    val spiffsUsedLegacy = if (payload.size >= 70) bb.getInt(66).toLong() and 0xFFFF_FFFFL else 0L
    val spiffsFreeLegacy = if (payload.size >= 74) bb.getInt(70).toLong() and 0xFFFF_FFFFL else 0L
    val spiffsTotalBytes = if (payload.size >= 99) bb.getLong(91) else spiffsTotalLegacy
    val spiffsUsedBytes = if (payload.size >= 107) bb.getLong(99) else spiffsUsedLegacy
    val spiffsFreeBytes = if (payload.size >= 115) bb.getLong(107) else spiffsFreeLegacy
    val blobActive = payload.size >= 75 && (payload[74].toInt() and 0xFF) == 1
    val blobSessionId = if (payload.size >= 83) bb.getLong(75) else 0L
    val blobBytesSent = if (payload.size >= 87) bb.getInt(83).toLong() and 0xFFFF_FFFFL else 0L
    val blobBytesTotal = if (payload.size >= 91) bb.getInt(87).toLong() and 0xFFFF_FFFFL else 0L
    return StatusPayload(
      scanning = scanning,
      bleEncrypted = bleEncrypted,
      currentChannel = currentChannel,
      hopMs = hopMs,
      channelMask = channelMask,
      localChannelMask = localChannelMask,
      uniqueBssids = uniqueBssids,
      packetsPerSec = packetsPerSec,
      droppedNotifies = droppedNotifies,
      bootMode = bootMode,
      gpsValid = gpsValid,
      gpsAgeS = gpsAgeS,
      gpsAccuracyDm = gpsAccuracyDm,
      nodeLinkUp = nodeLinkUp,
      nodeLastSeenS = nodeLastSeenS,
      nodePacketsPerSec = nodePacketsPerSec,
      nodeForwardedSightings = nodeForwardedSightings,
      nodeChannel = nodeChannel,
      nodeChannelMask = nodeChannelMask,
      nodeChannelMask24 = nodeChannelMask24,
      nodeChannelMask5Ghz = nodeChannelMask5Ghz,
      sessionOpen = sessionOpen,
      sessionId = sessionId,
      queuedRecords = queuedRecords,
      queuedBytes = queuedBytes,
      replayActive = replayActive,
      replayCursor = replayCursor,
      queueFull = queueFull,
      droppedFlashFull = droppedFlashFull,
      nodeCount = nodeCount,
      gpsNavAppliedHz = gpsNavAppliedHz,
      spiffsTotalBytes = spiffsTotalBytes,
      spiffsUsedBytes = spiffsUsedBytes,
      spiffsFreeBytes = spiffsFreeBytes,
      blobActive = blobActive,
      blobSessionId = blobSessionId,
      blobBytesSent = blobBytesSent,
      blobBytesTotal = blobBytesTotal,
    )
  }

  fun decodeGpsPayload(payload: ByteArray): EspGpsPayload {
    require(payload.size >= 28) { "Bad gps payload" }
    val bb = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
    val valid = (payload[0].toInt() and 0xFF) == 1
    val source = payload[1].toInt() and 0xFF
    val latE7 = bb.getInt(2)
    val lonE7 = bb.getInt(6)
    val altMm = bb.getInt(10)
    val speedMmps = bb.getInt(14)
    val bearingMdeg = bb.getInt(18)
    val unixTimeS = bb.getInt(22)
    val accuracyCm = bb.getShort(26).toInt() and 0xFFFF
    val satCount = if (payload.size >= 29) payload[28].toInt() and 0xFF else 0
    val hdopCenti = if (payload.size >= 31) bb.getShort(29).toInt() and 0xFFFF else 0
    val pdopCenti = if (payload.size >= 33) bb.getShort(31).toInt() and 0xFFFF else 0
    return EspGpsPayload(
      valid = valid,
      source = source,
      latE7 = latE7,
      lonE7 = lonE7,
      altMm = altMm,
      speedMmps = speedMmps,
      bearingMdeg = bearingMdeg,
      unixTimeS = unixTimeS,
      accuracyCm = accuracyCm,
      satCount = satCount,
      hdopCenti = hdopCenti,
      pdopCenti = pdopCenti,
    )
  }

  fun decodeSightingPayload(payload: ByteArray): SightingPayload {
    require(payload.size >= 14) { "Bad sighting payload" }

    val bssidBytes = payload.copyOfRange(0, 6)
    val bssid = bssidBytes.joinToString(":") { "%02X".format(it.toInt() and 0xFF) }
    val channel = payload[6].toInt() and 0xFF
    val rssi = payload[7].toInt().toByte().toInt()
    val authCode = payload[8].toInt() and 0xFF
    val protoFlags = payload[9].toInt() and 0xFF
    val akmFlags = payload[10].toInt() and 0xFF
    val cipherFlags = payload[11].toInt() and 0xFF
    val ssidLen = min(payload[12].toInt() and 0xFF, 32)
    val expected = 14 + ssidLen
    require(payload.size >= expected) { "Truncated sighting payload" }

    val ssidBytes = payload.copyOfRange(13, 13 + ssidLen)
    val ssid = String(ssidBytes, StandardCharsets.UTF_8)
    val flags = payload[13 + ssidLen].toInt() and 0xFF
    val bb = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
    val trailerOffset = 14 + ssidLen
    val sessionId = if (payload.size >= trailerOffset + 14) bb.getLong(trailerOffset) else 0L
    val recordSeq =
      if (payload.size >= trailerOffset + 14) {
        bb.getInt(trailerOffset + 8).toLong() and 0xFFFF_FFFFL
      } else {
        0L
      }
    val nodeId = if (payload.size >= trailerOffset + 14) payload[trailerOffset + 12].toInt() and 0xFF else 0
    val sourceFlags = if (payload.size >= trailerOffset + 14) payload[trailerOffset + 13].toInt() and 0xFF else 0

    val gpsOffset = trailerOffset + 14
    val gpsValid = if (payload.size >= gpsOffset + 20) (payload[gpsOffset].toInt() and 0xFF) == 1 else false
    val gpsSource = if (payload.size >= gpsOffset + 20) payload[gpsOffset + 1].toInt() and 0xFF else 0
    val gpsLatE7 = if (payload.size >= gpsOffset + 20) bb.getInt(gpsOffset + 2) else 0
    val gpsLonE7 = if (payload.size >= gpsOffset + 20) bb.getInt(gpsOffset + 6) else 0
    val gpsAltMm = if (payload.size >= gpsOffset + 20) bb.getInt(gpsOffset + 10) else 0
    val gpsUnixTimeS =
      if (payload.size >= gpsOffset + 20) bb.getInt(gpsOffset + 14).toLong() and 0xFFFF_FFFFL else 0L
    val gpsAccuracyCm = if (payload.size >= gpsOffset + 20) bb.getShort(gpsOffset + 18).toInt() and 0xFFFF else 0

    return SightingPayload(
      bssid = bssid,
      channel = channel,
      rssi = rssi,
      authCode = authCode,
      protoFlags = protoFlags,
      akmFlags = akmFlags,
      cipherFlags = cipherFlags,
      auth = formatSecurity(authCode, protoFlags, akmFlags, cipherFlags),
      ssid = ssid,
      flags = flags,
      sessionId = sessionId,
      recordSeq = recordSeq,
      nodeId = nodeId,
      sourceFlags = sourceFlags,
      gpsValid = gpsValid,
      gpsSource = gpsSource,
      gpsLatE7 = gpsLatE7,
      gpsLonE7 = gpsLonE7,
      gpsAltMm = gpsAltMm,
      gpsUnixTimeS = gpsUnixTimeS,
      gpsAccuracyCm = gpsAccuracyCm,
    )
  }

  fun decodeReplayBatchPayload(payload: ByteArray): List<ByteArray> {
    require(payload.isNotEmpty()) { "Bad replay batch payload" }
    val count = payload[0].toInt() and 0xFF
    require(count > 0) { "Empty replay batch payload" }
    var offset = 1
    val records = ArrayList<ByteArray>(count)
    repeat(count) {
      require(offset + 2 <= payload.size) { "Truncated replay batch header" }
      val len = (payload[offset].toInt() and 0xFF) or ((payload[offset + 1].toInt() and 0xFF) shl 8)
      offset += 2
      require(len > 0) { "Replay batch record has zero length" }
      require(offset + len <= payload.size) { "Truncated replay batch record" }
      records += payload.copyOfRange(offset, offset + len)
      offset += len
    }
    require(offset == payload.size) { "Replay batch payload has trailing bytes" }
    return records
  }

  fun encodeGpsFixPayload(fix: GpsFix): ByteArray {
    val out = ByteArray(GPS_FIX_PAYLOAD_SIZE)
    val bb = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN)
    bb.put((fix.flags and 0xFF).toByte())
    bb.putInt(fix.latE7)
    bb.putInt(fix.lonE7)
    bb.putInt(fix.altMm)
    bb.putInt(fix.speedMmps)
    bb.putInt(fix.bearingMdeg)
    bb.putInt(fix.unixTimeS)
    bb.putShort((fix.accuracyCm and 0xFFFF).toShort())
    return out
  }

  fun encodeReplayAckPayload(sessionId: Long, highestSeq: Long): ByteArray {
    val out = ByteArray(12)
    val bb = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN)
    bb.putLong(sessionId)
    bb.putInt((highestSeq and 0xFFFF_FFFFL).toInt())
    return out
  }

  fun encodeReplayTogglePayload(enabled: Boolean): ByteArray =
    byteArrayOf(if (enabled) 1 else 0)

  fun encodeBacklogBlobTogglePayload(enabled: Boolean): ByteArray =
    byteArrayOf(if (enabled) 1 else 0)

  fun encodeBacklogBlobChunkReplyPayload(
    sessionId: Long,
    chunkOffset: Long,
    chunkLen: Int,
    accepted: Boolean,
  ): ByteArray {
    val out = ByteArray(15)
    val bb = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN)
    bb.putLong(sessionId)
    bb.putInt((chunkOffset and 0xFFFF_FFFFL).toInt())
    bb.putShort((chunkLen and 0xFFFF).toShort())
    bb.put(if (accepted) 1 else 0)
    return out
  }

  fun encodeDebugSeedStoragePayload(targetBytes: Int): ByteArray {
    val out = ByteArray(4)
    val bb = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN)
    bb.putInt(targetBytes.coerceAtLeast(0))
    return out
  }

  fun encodeChannelPlanPayload(localChannelMask: Int, nodeChannelMask24: Int, nodeChannelMask5Ghz: Long): ByteArray {
    val out = ByteArray(12)
    val bb = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN)
    bb.putShort((localChannelMask and 0xFFFF).toShort())
    bb.putShort((nodeChannelMask24 and 0xFFFF).toShort())
    bb.putLong(nodeChannelMask5Ghz)
    return out
  }

  fun decodeBacklogBlobMetaPayload(payload: ByteArray): BacklogBlobMetaPayload {
    require(payload.size >= 20) { "Bad backlog blob meta payload" }
    val bb = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
    return BacklogBlobMetaPayload(
      sessionId = bb.getLong(0),
      totalBytes = bb.getInt(8).toLong() and 0xFFFF_FFFFL,
      ackedSeq = bb.getInt(12).toLong() and 0xFFFF_FFFFL,
      writtenSeq = bb.getInt(16).toLong() and 0xFFFF_FFFFL,
    )
  }

  fun decodeBacklogBlobChunkPayload(payload: ByteArray): BacklogBlobChunkPayload {
    require(payload.size >= 14) { "Bad backlog blob chunk payload" }
    val bb = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
    val sessionId = bb.getLong(0)
    val offset = bb.getInt(8).toLong() and 0xFFFF_FFFFL
    val len = bb.getShort(12).toInt() and 0xFFFF
    require(len > 0) { "Backlog blob chunk has zero length" }
    require(payload.size == 14 + len) { "Backlog blob chunk length mismatch" }
    return BacklogBlobChunkPayload(
      sessionId = sessionId,
      offset = offset,
      data = payload.copyOfRange(14, 14 + len),
    )
  }

  fun decodeBacklogBlobDonePayload(payload: ByteArray): BacklogBlobDonePayload {
    require(payload.size >= 16) { "Bad backlog blob done payload" }
    val bb = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
    return BacklogBlobDonePayload(
      sessionId = bb.getLong(0),
      totalBytes = bb.getInt(8).toLong() and 0xFFFF_FFFFL,
      writtenSeq = bb.getInt(12).toLong() and 0xFFFF_FFFFL,
    )
  }

  fun formatSecurity(authCode: Int, protoFlags: Int, akmFlags: Int, cipherFlags: Int): String {
    val parts = mutableListOf<String>()

    when {
      (protoFlags and (SEC_PROTO_WPA2 or SEC_PROTO_WPA3)) == (SEC_PROTO_WPA2 or SEC_PROTO_WPA3) -> {
        parts += "WPA2/WPA3"
      }
      (protoFlags and SEC_PROTO_WPA3) != 0 -> parts += "WPA3"
      (protoFlags and SEC_PROTO_WPA2) != 0 -> parts += "WPA2"
      (protoFlags and SEC_PROTO_WPA) != 0 -> parts += "WPA"
      authCode == 2 || (cipherFlags and SEC_CIPHER_WEP) != 0 -> parts += "WEP"
      authCode == 1 -> parts += "OPEN"
      else -> return authByCode(authCode)
    }

    when {
      (akmFlags and (SEC_AKM_PSK or SEC_AKM_SAE)) == (SEC_AKM_PSK or SEC_AKM_SAE) -> parts +=
        "PSK/SAE"
      (akmFlags and SEC_AKM_SAE) != 0 -> parts += "SAE"
      (akmFlags and SEC_AKM_PSK) != 0 -> parts += "PSK"
      (akmFlags and SEC_AKM_EAP) != 0 -> parts += "EAP"
      (akmFlags and SEC_AKM_OWE) != 0 -> parts += "OWE"
    }

    when {
      (cipherFlags and SEC_CIPHER_CCMP_256) != 0 -> parts += "CCMP-256"
      (cipherFlags and SEC_CIPHER_GCMP_256) != 0 -> parts += "GCMP-256"
      (cipherFlags and SEC_CIPHER_GCMP_128) != 0 -> parts += "GCMP-128"
      (cipherFlags and SEC_CIPHER_CCMP_128) != 0 -> parts += "CCMP-128"
      (cipherFlags and SEC_CIPHER_TKIP) != 0 -> parts += "TKIP"
      (cipherFlags and SEC_CIPHER_WEP) != 0 -> parts += "WEP"
    }

    return parts.joinToString("-")
  }

  private fun authByCode(code: Int): String =
    when (code) {
      1 -> "OPEN"
      2 -> "WEP?"
      3 -> "WPA"
      4 -> "WPA2/WPA3"
      else -> "UNKNOWN"
    }
}

data class WgFrame(
  val version: Int,
  val type: Int,
  val seq: Int,
  val len: Int,
  val deviceMs: Long,
  val payload: ByteArray,
)

data class StatusPayload(
  val scanning: Boolean,
  val bleEncrypted: Boolean,
  val currentChannel: Int,
  val hopMs: Int,
  val channelMask: Int,
  val localChannelMask: Int,
  val uniqueBssids: Int,
  val packetsPerSec: Int,
  val droppedNotifies: Int,
  val bootMode: Int,
  val gpsValid: Boolean,
  val gpsAgeS: Int,
  val gpsAccuracyDm: Int,
  val nodeLinkUp: Boolean,
  val nodeLastSeenS: Int,
  val nodePacketsPerSec: Int,
  val nodeForwardedSightings: Int,
  val nodeChannel: Int,
  val nodeChannelMask: Int,
  val nodeChannelMask24: Int,
  val nodeChannelMask5Ghz: Long,
  val sessionOpen: Boolean,
  val sessionId: Long,
  val queuedRecords: Long,
  val queuedBytes: Long,
  val replayActive: Boolean,
  val replayCursor: Long,
  val queueFull: Boolean,
  val droppedFlashFull: Long,
  val nodeCount: Int,
  val gpsNavAppliedHz: Int,
  val spiffsTotalBytes: Long,
  val spiffsUsedBytes: Long,
  val spiffsFreeBytes: Long,
  val blobActive: Boolean,
  val blobSessionId: Long,
  val blobBytesSent: Long,
  val blobBytesTotal: Long,
)

data class SightingPayload(
  val bssid: String,
  val channel: Int,
  val rssi: Int,
  val authCode: Int,
  val protoFlags: Int,
  val akmFlags: Int,
  val cipherFlags: Int,
  val auth: String,
  val ssid: String,
  val flags: Int,
  val sessionId: Long,
  val recordSeq: Long,
  val nodeId: Int,
  val sourceFlags: Int,
  val gpsValid: Boolean,
  val gpsSource: Int,
  val gpsLatE7: Int,
  val gpsLonE7: Int,
  val gpsAltMm: Int,
  val gpsUnixTimeS: Long,
  val gpsAccuracyCm: Int,
)

data class GpsFix(
  val flags: Int,
  val latE7: Int,
  val lonE7: Int,
  val altMm: Int,
  val speedMmps: Int,
  val bearingMdeg: Int,
  val unixTimeS: Int,
  val accuracyCm: Int,
)

data class EspGpsPayload(
  val valid: Boolean,
  val source: Int,
  val latE7: Int,
  val lonE7: Int,
  val altMm: Int,
  val speedMmps: Int,
  val bearingMdeg: Int,
  val unixTimeS: Int,
  val accuracyCm: Int,
  val satCount: Int,
  val hdopCenti: Int,
  val pdopCenti: Int,
)

data class BacklogBlobMetaPayload(
  val sessionId: Long,
  val totalBytes: Long,
  val ackedSeq: Long,
  val writtenSeq: Long,
)

data class BacklogBlobChunkPayload(
  val sessionId: Long,
  val offset: Long,
  val data: ByteArray,
)

data class BacklogBlobDonePayload(
  val sessionId: Long,
  val totalBytes: Long,
  val writtenSeq: Long,
)
