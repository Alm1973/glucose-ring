package com.glucosering.ring

import android.Manifest
import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.util.Log
import androidx.core.app.ServiceCompat
import java.util.concurrent.Executors

/**
 * Foreground relay: polls Nightscout every 60 s and writes each new, fresh
 * reading to the ring.
 *
 * Holds NO threshold or alarm logic; that is the ring's job. It does own
 * honesty: the notification is the only way to see whether this works, so it
 * always says, in plain words, whether a current reading is reaching the ring
 * and, if not, why (see Relay.status).
 */
@SuppressLint("MissingPermission", "WakelockTimeout") // Bluetooth calls are behind hasBluetoothPermission()
class RingService : Service() {

    companion object {
        private const val TAG = "RingService"
        private const val CHANNEL = "ring_link"
        private const val NOTIF_ID = 1
        private const val POLL_MS = 60_000L
        const val PREFS = "ring"
        const val PREF_MAC = "mac"
        private const val EXTRA_RECONNECT = "reconnect"

        /** Latest status, for MainActivity. Main thread only. */
        var current: Relay.Status? = null
            private set

        fun start(ctx: Context, reconnect: Boolean = false) {
            ctx.startForegroundService(Intent(ctx, RingService::class.java).putExtra(EXTRA_RECONNECT, reconnect))
        }
    }

    private val main = Handler(Looper.getMainLooper())
    private val io = Executors.newSingleThreadExecutor()
    private val configured = Relay.configProblem(BuildConfig.NS_URL, BuildConfig.NS_TOKEN) == null
    private val relay = Relay(Relay.configProblem(BuildConfig.NS_URL, BuildConfig.NS_TOKEN))
    private val nightscout = NightscoutClient(BuildConfig.NS_URL, BuildConfig.NS_TOKEN)
    private val backoff = Backoff()
    private lateinit var nus: NusClient
    private var fetching = false
    private var stopped = false
    private var wakeLock: PowerManager.WakeLock? = null

    private val tick = object : Runnable {
        override fun run() {
            poll()
            refresh()   // re-evaluates staleness even when nothing arrives
            main.postDelayed(this, POLL_MS)
        }
    }
    private val reconnect = Runnable { connectRing() }

    override fun onCreate() {
        super.onCreate()
        nus = NusClient(this, ::onLink, ::onRingLine)
        createChannel()
        try {
            // Within 5 s of startForegroundService(), or the system kills us.
            ServiceCompat.startForeground(
                this, NOTIF_ID, buildNotification(Relay.Status(false, "Glucose Ring", "Starting.")),
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
            )
        } catch (e: RuntimeException) {
            Log.e(TAG, "foreground start refused: ${e.javaClass.simpleName}")
            stopped = true
            stopSelf()
            return
        }
        // Handler timers do not run while the CPU sleeps, and a 60 s poll must
        // keep running with the screen off. This costs battery; it is the price
        // of not silently stopping (see PHONE_APP_REPORT.md, Untested).
        wakeLock = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "glucosering:relay")
            .apply { setReferenceCounted(false); acquire() }
        main.post(tick)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (stopped) return START_NOT_STICKY
        if (intent?.getBooleanExtra(EXTRA_RECONNECT, false) == true || nus.link == RingLink.DISCONNECTED) {
            backoff.reset()
            connectRing()
        }
        return START_STICKY
    }

    override fun onDestroy() {
        stopped = true
        main.removeCallbacksAndMessages(null)
        nus.shutdown()
        io.shutdownNow()
        wakeLock?.let { if (it.isHeld) it.release() }
        current = null
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun now() = System.currentTimeMillis()

    // ---- Nightscout ----

    private fun poll() {
        if (!configured || fetching) return
        fetching = true
        io.execute {
            val f = nightscout.fetch()
            main.post {
                fetching = false
                if (stopped) return@post
                relay.onFetch(f, now())
                if (f !is Fetch.Ok) Log.w(TAG, "poll: $f")   // Fetch never contains the token
                send()
                refresh()
            }
        }
    }

    // ---- ring ----

    private fun send() {
        val line = relay.takeLine(now(), nus.link) ?: return
        nus.writeLine(line)
        // Readings are never acknowledged, and a write can succeed without the
        // ring accepting it. Ask the ring what it now holds.
        nus.writeLine(RingProtocol.cmdGetStatus())
    }

    private fun hasBluetoothPermission() = Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
        checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

    private fun savedMac(): String? = getSharedPreferences(PREFS, MODE_PRIVATE).getString(PREF_MAC, null)

    /** The link as the user should see it, including why we cannot even try. */
    private fun ringLink(): RingLink {
        if (!hasBluetoothPermission()) return RingLink.NO_PERMISSION
        val adapter = getSystemService(BluetoothManager::class.java).adapter
        if (adapter == null || !adapter.isEnabled) return RingLink.BLUETOOTH_OFF
        if (savedMac() == null) return RingLink.NO_DEVICE
        return nus.link
    }

    private fun connectRing() {
        main.removeCallbacks(reconnect)
        val state = ringLink()
        if (state == RingLink.NO_PERMISSION || state == RingLink.BLUETOOTH_OFF || state == RingLink.NO_DEVICE) {
            scheduleReconnect()   // look again later; granting or choosing a ring also restarts us
            refresh()
            return
        }
        val adapter = getSystemService(BluetoothManager::class.java).adapter
        val device = try {
            adapter.getRemoteDevice(savedMac())
        } catch (e: IllegalArgumentException) {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().remove(PREF_MAC).apply()
            refresh()
            return
        }
        nus.connect(device)
    }

    private fun scheduleReconnect() {
        main.removeCallbacks(reconnect)
        main.postDelayed(reconnect, backoff.next())
    }

    private fun onLink(l: RingLink) {
        Log.i(TAG, "ring link: $l")
        when (l) {
            // Reset backoff only once the ring has answered, not on a bare
            // connection: a link that never reaches #BOOT/#VER is not working.
            RingLink.CONFIRMED -> {
                backoff.reset()
                relay.onLinkConfirmed()
                send()
            }
            RingLink.DISCONNECTED -> if (!stopped) scheduleReconnect()
            else -> {}
        }
        refresh()
    }

    private fun onRingLine(line: String) {
        val p = RingProtocol.parseRingLine(line) ?: return
        if (p.kind == "BOOT" && p.fields["wdt_reset"] == "1") Log.w(TAG, "ring reports a watchdog reset")
        relay.onRingLine(p)
        refresh()
    }

    // ---- notification ----

    private fun refresh() {
        if (stopped) return
        val s = relay.status(now(), ringLink())
        current = s
        getSystemService(NotificationManager::class.java).notify(NOTIF_ID, buildNotification(s))
    }

    private fun createChannel() {
        // LOW = silent. This is a status display, not an alarm: alarms belong
        // to the ring and to the phone's own CGM app (PHONE_APP.md section 7).
        val ch = NotificationChannel(CHANNEL, "Relay status", NotificationManager.IMPORTANCE_LOW)
        ch.description = "Whether glucose readings are reaching the ring"
        getSystemService(NotificationManager::class.java).createNotificationChannel(ch)
    }

    private fun buildNotification(s: Relay.Status): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE
        )
        return Notification.Builder(this, CHANNEL)
            .setContentTitle(s.title)
            .setContentText(s.text)
            .setStyle(Notification.BigTextStyle().bigText(s.text))
            .setSmallIcon(
                if (s.relaying) android.R.drawable.stat_sys_data_bluetooth
                else android.R.drawable.stat_notify_error
            )
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .build()
    }
}
