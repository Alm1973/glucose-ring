package com.glucosering.ring

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/** Restart the relay after a reboot. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return
        RingService.start(ctx)
    }
}
