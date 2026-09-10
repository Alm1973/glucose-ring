package com.glucosering.ring

data class Reading(val mgdl: Double, val epochMs: Long, val slopeName: String)

/**
 * Guards against re-sending. xDrip+ re-broadcasts the same reading on backfill,
 * calibration and app resume; the ring drops any timestamp that is not strictly
 * newer, and a duplicate does NOT refresh staleness. So filter here rather than
 * relying on the ring to ignore us.
 */
class ReadingGate {
    var lastSentEpoch: Long = 0L
        private set

    fun shouldSend(epochMs: Long): Boolean = epochMs > lastSentEpoch

    fun markSent(epochMs: Long) {
        if (epochMs > lastSentEpoch) lastSentEpoch = epochMs
    }
}
