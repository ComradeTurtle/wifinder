package com.wifinder.android.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class WifiBandTest {
  @Test
  fun node5ChannelsUsePrimary20MhzOnly() {
    assertEquals(25, WifiBand.NODE_CHANNELS_5_GHZ.size)
    assertTrue(WifiBand.NODE_CHANNELS_5_GHZ.contains(36))
    assertTrue(WifiBand.NODE_CHANNELS_5_GHZ.contains(165))
    assertFalse(WifiBand.NODE_CHANNELS_5_GHZ.contains(32))
    assertFalse(WifiBand.NODE_CHANNELS_5_GHZ.contains(177))
    assertFalse(WifiBand.NODE_CHANNELS_5_GHZ.contains(182))
  }

  @Test
  fun classify24GHz() {
    assertEquals(WifiBand.BAND_24_GHZ, WifiBand.fromChannel(1))
    assertEquals(WifiBand.BAND_24_GHZ, WifiBand.fromChannel(13))
  }

  @Test
  fun classify5GHz() {
    assertEquals(WifiBand.BAND_5_GHZ, WifiBand.fromChannel(36))
    assertEquals(WifiBand.BAND_5_GHZ, WifiBand.fromChannel(149))
  }

  @Test
  fun unknownBandForOutOfRange() {
    assertEquals(WifiBand.BAND_UNKNOWN, WifiBand.fromChannel(0))
    assertEquals(WifiBand.BAND_UNKNOWN, WifiBand.fromChannel(255))
  }

  @Test
  fun frequencyFromChannel() {
    assertEquals(2412, WifiBand.frequencyMhzFromChannel(1))
    assertEquals(2484, WifiBand.frequencyMhzFromChannel(14))
    assertEquals(5180, WifiBand.frequencyMhzFromChannel(36))
    assertEquals(5745, WifiBand.frequencyMhzFromChannel(149))
    assertNull(WifiBand.frequencyMhzFromChannel(0))
  }

  @Test
  fun conciseLabel() {
    assertEquals("1/2.4G", WifiBand.conciseChannelLabel(1))
    assertEquals("36/5G", WifiBand.conciseChannelLabel(36))
    assertEquals("255", WifiBand.conciseChannelLabel(255))
  }

  @Test
  fun node5MaskHelpers() {
    var mask = 0L
    mask = WifiBand.setNode5GhzChannel(mask, 36, true)
    assertTrue(WifiBand.node5GhzChannelEnabled(mask, 36))
    mask = WifiBand.setNode5GhzChannel(mask, 36, false)
    assertFalse(WifiBand.node5GhzChannelEnabled(mask, 36))
    assertEquals(mask, WifiBand.sanitizeNode5GhzMask(mask))
  }
}
