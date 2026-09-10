package com.glucosering.ring

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

/**
 * xDrip+ local broadcast -> Reading.
 *
 * !! THE KEY NAMES BELOW ARE UNVERIFIED. !!  They were not checked against the
 * xDrip+ source. If one is wrong this receiver silently never fires, which is
 * indistinguishable from "no readings yet" -- the worst failure mode there is.
 *
 * Build with DUMP_EXTRAS = true first, read logcat, confirm every key against
 * what actually arrives, and only then set it back to false.
 *
 * Requires xDrip+: Settings -> Inter-app settings -> "Broadcast locally".
 */
class XdripReceiver(private val onReading: (Reading) -> Unit) : BroadcastReceiver() {

    companion object {
        const val ACTION = "com.eveningoutpost.dexdrip.BgEstimate"
        const val EXTRA_BG    = "com.eveningoutpost.dexdrip.Extras.BgEstimate"
        const val EXTRA_TIME  = "com.eveningoutpost.dexdrip.Extras.Time"
        const val EXTRA_SLOPE = "com.eveningoutpost.dexdrip.Extras.BgSlopeName"

        /** Leave true until the contract is confirmed against real traffic. */
        const val DUMP_EXTRAS = true
        private const val TAG = "RingXdrip"
    }

    override fun onReceive(ctx: Context, intent: Intent) {
        if (DUMP_EXTRAS) {
            Log.i(TAG, "action=${intent.action}")
            val b = intent.extras
            if (b == null) Log.i(TAG, "  (no extras)")
            else for (k in b.keySet()) {
                @Suppress("DEPRECATION")
                val v = b.get(k)
                Log.i(TAG, "  $k = $v  (${v?.javaClass?.simpleName})")
            }
        }

        val mgdl = intent.getDoubleExtra(EXTRA_BG, Double.NaN)
        val time = intent.getLongExtra(EXTRA_TIME, 0L)
        val slope = intent.getStringExtra(EXTRA_SLOPE) ?: ""
        if (mgdl.isNaN() || time == 0L) {
            Log.w(TAG, "reading missing expected extras -- check the key names")
            return
        }
        onReading(Reading(mgdl, time, slope))
    }
}
