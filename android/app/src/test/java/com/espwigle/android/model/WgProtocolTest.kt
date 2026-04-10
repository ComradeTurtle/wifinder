package com.espwigle.android.model

import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue
import org.junit.Test

class WgProtocolTest {
  @Test
  fun `decode status payload parses gps fields and extended storage stats`() {
    val payload = ByteArray(129)
    payload[0] = 1
    payload[1] = 1
    payload[2] = 11
    payload[3] = 0xFA.toByte()
    payload[4] = 0x00
    payload[5] = 0xFF.toByte()
    payload[6] = 0x1F
    payload[7] = 0x34
    payload[8] = 0x12
    payload[9] = 0x56
    payload[10] = 0x00
    payload[11] = 0x01
    payload[12] = 0x00
    payload[13] = 1
    payload[14] = 1
    payload[15] = 0x09
    payload[16] = 0x00
    payload[17] = 0x1E
    payload[18] = 0x00
    payload[19] = 1
    payload[20] = 0x05
    payload[21] = 0x00
    payload[22] = 0x4A
    payload[23] = 0x00
    payload[24] = 0x10
    payload[25] = 0x00
    payload[26] = 6
    payload[27] = 0x55
    payload[28] = 0x15
    payload[29] = 1
    val sessionId = 0x1122334455667788L
    for (i in 0 until 8) {
      payload[30 + i] = ((sessionId ushr (8 * i)) and 0xFF).toByte()
    }
    payload[38] = 0x2A
    payload[42] = 0x20
    payload[43] = 0x03
    payload[46] = 1
    payload[47] = 0x11
    payload[51] = 1
    payload[52] = 0x07
    payload[56] = 2
    payload[57] = 0xA0.toByte()
    payload[58] = 0x86.toByte()
    payload[59] = 0x01
    payload[60] = 0x00
    payload[61] = 2
    payload[62] = 0xFF.toByte()
    payload[63] = 0xFF.toByte()
    payload[64] = 0xFF.toByte()
    payload[65] = 0xFF.toByte()
    payload[66] = 0xFF.toByte()
    payload[67] = 0xFF.toByte()
    payload[68] = 0xFF.toByte()
    payload[69] = 0xFF.toByte()
    payload[70] = 0xFF.toByte()
    payload[71] = 0xFF.toByte()
    payload[72] = 0xFF.toByte()
    payload[73] = 0xFF.toByte()
    payload[74] = 1
    val blobSessionId = 0x0102030405060708L
    for (i in 0 until 8) {
      payload[75 + i] = ((blobSessionId ushr (8 * i)) and 0xFF).toByte()
    }
    payload[83] = 0x00
    payload[84] = 0x10
    payload[87] = 0x00
    payload[88] = 0x20
    val totalStorage = 32L * 1024L * 1024L * 1024L
    val usedStorage = 12L * 1024L * 1024L * 1024L
    val freeStorage = 20L * 1024L * 1024L * 1024L
    for (i in 0 until 8) {
      payload[91 + i] = ((totalStorage ushr (8 * i)) and 0xFF).toByte()
      payload[99 + i] = ((usedStorage ushr (8 * i)) and 0xFF).toByte()
      payload[107 + i] = ((freeStorage ushr (8 * i)) and 0xFF).toByte()
    }
    payload[115] = 0xAA.toByte()
    payload[116] = 0x0A
    payload[117] = 0x0F
    payload[118] = 0x00
    val nodeMask5 = 0x0000000012345678L
    for (i in 0 until 8) {
      payload[119 + i] = ((nodeMask5 ushr (8 * i)) and 0xFF).toByte()
    }
    val dieTempCenti = 4567
    payload[127] = (dieTempCenti and 0xFF).toByte()
    payload[128] = ((dieTempCenti ushr 8) and 0xFF).toByte()

    val status = WgProtocol.decodeStatusPayload(payload)

    assertTrue(status.scanning)
    assertTrue(status.bleEncrypted)
    assertEquals(11, status.currentChannel)
    assertEquals(250, status.hopMs)
    assertEquals(0x1FFF, status.channelMask)
    assertEquals(0x0AAA, status.localChannelMask)
    assertEquals(100000, status.uniqueBssids)
    assertEquals(86, status.packetsPerSec)
    assertEquals(1, status.droppedNotifies)
    assertEquals(1, status.bootMode)
    assertTrue(status.gpsValid)
    assertEquals(9, status.gpsAgeS)
    assertEquals(30, status.gpsAccuracyDm)
    assertTrue(status.nodeLinkUp)
    assertEquals(5, status.nodeLastSeenS)
    assertEquals(74, status.nodePacketsPerSec)
    assertEquals(16, status.nodeForwardedSightings)
    assertEquals(6, status.nodeChannel)
    assertEquals(0x1555, status.nodeChannelMask)
    assertEquals(0x000F, status.nodeChannelMask24)
    assertEquals(nodeMask5, status.nodeChannelMask5Ghz)
    assertTrue(status.sessionOpen)
    assertEquals(sessionId, status.sessionId)
    assertEquals(42L, status.queuedRecords)
    assertEquals(800L, status.queuedBytes)
    assertTrue(status.replayActive)
    assertEquals(17L, status.replayCursor)
    assertTrue(status.queueFull)
    assertEquals(7L, status.droppedFlashFull)
    assertEquals(2, status.nodeCount)
    assertEquals(2, status.gpsNavAppliedHz)
    assertEquals(totalStorage, status.spiffsTotalBytes)
    assertEquals(usedStorage, status.spiffsUsedBytes)
    assertEquals(freeStorage, status.spiffsFreeBytes)
    assertTrue(status.blobActive)
    assertEquals(blobSessionId, status.blobSessionId)
    assertEquals(4096L, status.blobBytesSent)
    assertEquals(8192L, status.blobBytesTotal)
    assertEquals(dieTempCenti, status.dieTempCenti)
  }

  @Test
  fun `decode status payload keeps legacy unique counter when extension missing`() {
    val payload = ByteArray(57)
    payload[7] = 0x34
    payload[8] = 0x12

    val status = WgProtocol.decodeStatusPayload(payload)

    assertEquals(0x1234, status.uniqueBssids)
    assertEquals(status.channelMask, status.localChannelMask)
    assertEquals(status.nodeChannelMask, status.nodeChannelMask24)
    assertEquals(0L, status.nodeChannelMask5Ghz)
    assertEquals(0, status.gpsNavAppliedHz)
    assertEquals(0L, status.spiffsFreeBytes)
    assertEquals(false, status.blobActive)
    assertEquals(Int.MIN_VALUE, status.dieTempCenti)
  }

  @Test
  fun `decode sighting payload keeps ssid and security details`() {
    val ssid = "TestAP"
    val ssidBytes = ssid.encodeToByteArray()
    val payload = ByteArray(48 + ssidBytes.size)
    payload[0] = 0x48
    payload[1] = 0xA9.toByte()
    payload[2] = 0x8A.toByte()
    payload[3] = 0xED.toByte()
    payload[4] = 0x1A
    payload[5] = 0x4A
    payload[6] = 6
    payload[7] = (-57).toByte()
    payload[8] = 4
    payload[9] = (WgProtocol.SEC_PROTO_WPA2 or WgProtocol.SEC_PROTO_WPA3).toByte()
    payload[10] = (WgProtocol.SEC_AKM_PSK or WgProtocol.SEC_AKM_SAE).toByte()
    payload[11] = WgProtocol.SEC_CIPHER_CCMP_128.toByte()
    payload[12] = ssidBytes.size.toByte()
    ssidBytes.copyInto(payload, destinationOffset = 13)
    payload[13 + ssidBytes.size] = 0x01
    val sessionId = 0x0123456789ABCDEFL
    for (i in 0 until 8) {
      payload[14 + ssidBytes.size + i] = ((sessionId ushr (8 * i)) and 0xFF).toByte()
    }
    payload[22 + ssidBytes.size] = 0x2A
    payload[26 + ssidBytes.size] = 1
    payload[27 + ssidBytes.size] = 1
    payload[28 + ssidBytes.size] = 1
    payload[29 + ssidBytes.size] = 1
    val latE7 = 391333890
    val lonE7 = 209626726
    val altMm = 22300
    val unixS = 1711565786
    for (i in 0 until 4) {
      payload[30 + ssidBytes.size + i] = ((latE7 ushr (8 * i)) and 0xFF).toByte()
      payload[34 + ssidBytes.size + i] = ((lonE7 ushr (8 * i)) and 0xFF).toByte()
      payload[38 + ssidBytes.size + i] = ((altMm ushr (8 * i)) and 0xFF).toByte()
      payload[42 + ssidBytes.size + i] = ((unixS ushr (8 * i)) and 0xFF).toByte()
    }
    payload[46 + ssidBytes.size] = 0xB0.toByte()
    payload[47 + ssidBytes.size] = 0x04

    val sighting = WgProtocol.decodeSightingPayload(payload)

    assertEquals("48:A9:8A:ED:1A:4A", sighting.bssid)
    assertEquals(6, sighting.channel)
    assertEquals(-57, sighting.rssi)
    assertEquals("WPA2/WPA3-PSK/SAE-CCMP-128", sighting.auth)
    assertEquals(ssid, sighting.ssid)
    assertEquals(0x01, sighting.flags)
    assertEquals(sessionId, sighting.sessionId)
    assertEquals(42L, sighting.recordSeq)
    assertEquals(1, sighting.nodeId)
    assertEquals(1, sighting.sourceFlags)
    assertTrue(sighting.gpsValid)
    assertEquals(1, sighting.gpsSource)
    assertEquals(latE7, sighting.gpsLatE7)
    assertEquals(lonE7, sighting.gpsLonE7)
    assertEquals(altMm, sighting.gpsAltMm)
    assertEquals(unixS.toLong(), sighting.gpsUnixTimeS)
    assertEquals(1200, sighting.gpsAccuracyCm)
  }

  @Test
  fun `decode sighting payload supports legacy records without gps extension`() {
    val ssid = "Legacy"
    val ssidBytes = ssid.encodeToByteArray()
    val payload = ByteArray(28 + ssidBytes.size)
    payload[0] = 0x40
    payload[1] = 0xED.toByte()
    payload[2] = 0x00
    payload[3] = 0x25
    payload[4] = 0xE0.toByte()
    payload[5] = 0x02
    payload[6] = 1
    payload[7] = (-44).toByte()
    payload[8] = 4
    payload[9] = WgProtocol.SEC_PROTO_WPA2.toByte()
    payload[10] = WgProtocol.SEC_AKM_PSK.toByte()
    payload[11] = WgProtocol.SEC_CIPHER_CCMP_128.toByte()
    payload[12] = ssidBytes.size.toByte()
    ssidBytes.copyInto(payload, destinationOffset = 13)
    payload[13 + ssidBytes.size] = 0x01
    payload[26 + ssidBytes.size] = 1
    payload[27 + ssidBytes.size] = 1

    val sighting = WgProtocol.decodeSightingPayload(payload)

    assertEquals("40:ED:00:25:E0:02", sighting.bssid)
    assertEquals(ssid, sighting.ssid)
    assertEquals(1, sighting.sourceFlags)
    assertEquals(false, sighting.gpsValid)
    assertEquals(0, sighting.gpsAccuracyCm)
  }

  @Test
  fun `encode gps command payload packs little endian fields`() {
    val fix = GpsFix(
      flags = WgProtocol.GPS_FLAG_VALID or WgProtocol.GPS_FLAG_HAS_ALT or WgProtocol.GPS_FLAG_HAS_SPEED,
      latE7 = 373320001,
      lonE7 = -1220311222,
      altMm = 123450,
      speedMmps = 5123,
      bearingMdeg = 90000,
      unixTimeS = 1_700_000_001,
      accuracyCm = 250,
    )

    val payload = WgProtocol.encodeGpsFixPayload(fix)

    assertEquals(WgProtocol.GPS_FIX_PAYLOAD_SIZE, payload.size)
    assertEquals(fix.flags.toByte(), payload[0])
    assertEquals(0xFA, payload[25].toInt() and 0xFF)
    assertEquals(0x00, payload[26].toInt() and 0xFF)
  }

  @Test
  fun `encode backlog blob chunk reply payload packs fields`() {
    val payload = WgProtocol.encodeBacklogBlobChunkReplyPayload(
      sessionId = 0x0123456789ABCDEFL,
      chunkOffset = 0x10203040L,
      chunkLen = 500,
      accepted = true,
    )

    assertEquals(15, payload.size)
    assertEquals(0xEF, payload[0].toInt() and 0xFF)
    assertEquals(0xCD, payload[1].toInt() and 0xFF)
    assertEquals(0x40, payload[8].toInt() and 0xFF)
    assertEquals(0x30, payload[9].toInt() and 0xFF)
    assertEquals(0xF4, payload[12].toInt() and 0xFF)
    assertEquals(0x01, payload[13].toInt() and 0xFF)
    assertEquals(1, payload[14].toInt() and 0xFF)
  }

  @Test
  fun `decode gps payload parses source and coordinates`() {
    val payload = byteArrayOf(
      1,
      2,
      0x10,
      0x27,
      0x4F,
      0x16,
      0xD2.toByte(),
      0x04,
      0x56,
      0xB7.toByte(),
      0x39,
      0x30,
      0x00,
      0x00,
      0xE8.toByte(),
      0x03,
      0x00,
      0x00,
      0x28,
      0x23,
      0x00,
      0x00,
      0x80.toByte(),
      0xF1.toByte(),
      0x53,
      0x65,
      0xFA.toByte(),
      0x00,
      11,
      0x5C,
      0x00,
      0x92.toByte(),
      0x00,
    )

    val gps = WgProtocol.decodeGpsPayload(payload)

    assertTrue(gps.valid)
    assertEquals(2, gps.source)
    assertEquals(374286096, gps.latE7)
    assertEquals(-1219099438, gps.lonE7)
    assertEquals(12345, gps.altMm)
    assertEquals(1000, gps.speedMmps)
    assertEquals(9000, gps.bearingMdeg)
    assertEquals(1700000128, gps.unixTimeS)
    assertEquals(250, gps.accuracyCm)
    assertEquals(11, gps.satCount)
    assertEquals(92, gps.hdopCenti)
    assertEquals(146, gps.pdopCenti)
  }

  @Test
  fun `encode replay ack payload packs session and sequence`() {
    val payload = WgProtocol.encodeReplayAckPayload(0x1122334455667788, 77)
    assertEquals(12, payload.size)
    assertEquals(0x88, payload[0].toInt() and 0xFF)
    assertEquals(0x11, payload[7].toInt() and 0xFF)
    assertEquals(77, payload[8].toInt() and 0xFF)
  }

  @Test
  fun `decode replay batch payload unpacks records`() {
    val payload =
      byteArrayOf(
        2, // count
        3, 0, // len #1
        0x11, 0x22, 0x33,
        2, 0, // len #2
        0x44, 0x55,
      )

    val records = WgProtocol.decodeReplayBatchPayload(payload)

    assertEquals(2, records.size)
    assertTrue(records[0].contentEquals(byteArrayOf(0x11, 0x22, 0x33)))
    assertTrue(records[1].contentEquals(byteArrayOf(0x44, 0x55)))
  }

  @Test
  fun `decode replay batch payload rejects malformed payload`() {
    val truncatedRecord = byteArrayOf(1, 4, 0, 0x10, 0x20, 0x30)
    val trailingBytes = byteArrayOf(1, 1, 0, 0x7F, 0x55)

    assertFailsWith<IllegalArgumentException> {
      WgProtocol.decodeReplayBatchPayload(truncatedRecord)
    }
    assertFailsWith<IllegalArgumentException> {
      WgProtocol.decodeReplayBatchPayload(trailingBytes)
    }
  }

  @Test
  fun `encode replay toggle payload packs single enable byte`() {
    val enabled = WgProtocol.encodeReplayTogglePayload(true)
    val disabled = WgProtocol.encodeReplayTogglePayload(false)

    assertEquals(1, enabled.size)
    assertEquals(1, enabled[0].toInt() and 0xFF)
    assertEquals(1, disabled.size)
    assertEquals(0, disabled[0].toInt() and 0xFF)
  }

  @Test
  fun `encode channel plan payload packs masks little endian`() {
    val payload = WgProtocol.encodeChannelPlanPayload(0x1555, 0x0AA0, 0x0000000011223344L)

    assertEquals(12, payload.size)
    assertEquals(0x55, payload[0].toInt() and 0xFF)
    assertEquals(0x15, payload[1].toInt() and 0xFF)
    assertEquals(0xA0, payload[2].toInt() and 0xFF)
    assertEquals(0x0A, payload[3].toInt() and 0xFF)
    assertEquals(0x44, payload[4].toInt() and 0xFF)
    assertEquals(0x11, payload[7].toInt() and 0xFF)
  }

  @Test
  fun `encode debug seed storage payload packs little endian bytes`() {
    val payload = WgProtocol.encodeDebugSeedStoragePayload(786432)

    assertEquals(4, payload.size)
    assertEquals(0x00, payload[0].toInt() and 0xFF)
    assertEquals(0x00, payload[1].toInt() and 0xFF)
    assertEquals(0x0C, payload[2].toInt() and 0xFF)
    assertEquals(0x00, payload[3].toInt() and 0xFF)
  }

  @Test
  fun `decode backlog blob meta payload parses fields`() {
    val payload = ByteArray(20)
    val sessionId = 0x1122334455667788L
    for (i in 0 until 8) {
      payload[i] = ((sessionId ushr (8 * i)) and 0xFF).toByte()
    }
    payload[8] = 0x00
    payload[9] = 0x04
    payload[10] = 0x00
    payload[11] = 0x00
    payload[12] = 0x10
    payload[13] = 0x00
    payload[14] = 0x00
    payload[15] = 0x00
    payload[16] = 0x40
    payload[17] = 0x00
    payload[18] = 0x00
    payload[19] = 0x00

    val meta = WgProtocol.decodeBacklogBlobMetaPayload(payload)

    assertEquals(sessionId, meta.sessionId)
    assertEquals(1024L, meta.totalBytes)
    assertEquals(16L, meta.ackedSeq)
    assertEquals(64L, meta.writtenSeq)
  }

  @Test
  fun `decode backlog blob chunk payload parses header and data`() {
    val payload = ByteArray(8 + 4 + 2 + 3)
    val sessionId = 0x1020304050607080L
    for (i in 0 until 8) {
      payload[i] = ((sessionId ushr (8 * i)) and 0xFF).toByte()
    }
    payload[8] = 0x2A
    payload[9] = 0x00
    payload[10] = 0x00
    payload[11] = 0x00
    payload[12] = 0x03
    payload[13] = 0x00
    payload[14] = 0x11
    payload[15] = 0x22
    payload[16] = 0x33

    val chunk = WgProtocol.decodeBacklogBlobChunkPayload(payload)

    assertEquals(sessionId, chunk.sessionId)
    assertEquals(42L, chunk.offset)
    assertTrue(chunk.data.contentEquals(byteArrayOf(0x11, 0x22, 0x33)))
  }

  @Test
  fun `decode backlog blob done payload parses summary`() {
    val payload = ByteArray(16)
    val sessionId = 0x0102030405060708L
    for (i in 0 until 8) {
      payload[i] = ((sessionId ushr (8 * i)) and 0xFF).toByte()
    }
    payload[8] = 0x00
    payload[9] = 0x04
    payload[10] = 0x00
    payload[11] = 0x00
    payload[12] = 0x7F
    payload[13] = 0x00
    payload[14] = 0x00
    payload[15] = 0x00

    val done = WgProtocol.decodeBacklogBlobDonePayload(payload)

    assertEquals(sessionId, done.sessionId)
    assertEquals(1024L, done.totalBytes)
    assertEquals(127L, done.writtenSeq)
  }
}
