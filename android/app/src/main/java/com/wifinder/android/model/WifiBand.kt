package com.wifinder.android.model

object WifiBand {
  const val BAND_UNKNOWN = "unknown"
  const val BAND_24_GHZ = "2.4ghz"
  const val BAND_5_GHZ = "5ghz"
  val CHANNELS_24_GHZ: List<Int> = (1..13).toList()
  val NODE_CHANNELS_5_GHZ: List<Int> =
    listOf(
      36, 40, 44, 48,
      52, 56, 60, 64,
      100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
      149, 153, 157, 161, 165,
    )
  private val node5GhzBitByChannel: Map<Int, Int> =
    NODE_CHANNELS_5_GHZ.withIndex().associate { indexed ->
      indexed.value to indexed.index
    }
  val NODE_CHANNELS_5_GHZ_ALL_MASK: Long = (1L shl NODE_CHANNELS_5_GHZ.size) - 1L

  fun fromChannel(channel: Int): String = when {
    channel in 1..14 -> BAND_24_GHZ
    node5GhzBitByChannel.containsKey(channel) -> BAND_5_GHZ
    else -> BAND_UNKNOWN
  }

  fun frequencyMhzFromChannel(channel: Int): Int? = when {
    channel in 1..13 -> 2407 + channel * 5
    channel == 14 -> 2484
    node5GhzBitByChannel.containsKey(channel) -> 5000 + channel * 5
    else -> null
  }

  fun conciseChannelLabel(channel: Int): String = when (fromChannel(channel)) {
    BAND_24_GHZ -> "$channel/2.4G"
    BAND_5_GHZ -> "$channel/5G"
    else -> channel.toString()
  }

  fun node5GhzChannelEnabled(mask: Long, channel: Int): Boolean {
    val bit = node5GhzBitByChannel[channel] ?: return false
    return ((mask ushr bit) and 1L) != 0L
  }

  fun setNode5GhzChannel(mask: Long, channel: Int, enabled: Boolean): Long {
    val bit = node5GhzBitByChannel[channel] ?: return mask
    val flag = 1L shl bit
    val next = if (enabled) mask or flag else mask and flag.inv()
    return sanitizeNode5GhzMask(next)
  }

  fun sanitizeNode5GhzMask(mask: Long): Long = mask and NODE_CHANNELS_5_GHZ_ALL_MASK
}
