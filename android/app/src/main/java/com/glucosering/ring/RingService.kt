package com.glucosering.ring

import android.annotation.SuppressLint
import android.app.*
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log

/**
 * Foreground courier service. Holds the GATT client, listens for xDrip+
 * broadcasts, forwards readings.
 *
 * Deliberately holds NO alerting logic. Staleness is the ring's job -- it
 * buzzes on its own after its stale window with no fresh data. Duplicating
 * that here would produce two uncoordinated alarms.
 */
@SuppressLint("MissingPermission")
class RingService : Service() {

    companion object {
        private const val TAG = "RingService"
        private const val CHANNEL = "ring_link"
        private const val NOTIF_ID = 1
        const val EXTRA_MAC = "mac"
        private const val BACKOFF_START_MS = 1_000L
        private const val BACKOFF_MAX_MS = 60_000L
    }

    private lateinit var nus: NusClient
    private val gate = ReadingGate()
    private val handler = Handler(Looper.getMainLooper())
    private var backoff = BACKOFF_START_MS
    private var mac: String? = null
    private var status = "starting"
    private var lastReading = "none"

    private val xdrip = XdripReceiver { r -> onReading(r) }

    override fun onCreate() {
        super.onCreate()
        createChannel()
        // Must be inside the 5-second window or the system kills us.
        startForeground(NOTIF_ID, buildNotification())

        nus = NusClient(this, ::onRingLine, ::onLinkState)

        // Registered at runtime, not in the manifest: implicit broadcasts from
        // other apps to manifest-declared receivers are restricted from API 26.
        val filter = IntentFilter(XdripReceiver.ACTION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(xdrip, filter, Context.RECEIVER_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            registerReceiver(xdrip, filter)
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        intent?.getStringExtra(EXTRA_MAC)?.let { mac = it }
        connectNow()
        return START_STICKY
    }

    override fun onDestroy() {
        try { unregisterReceiver(xdrip) } catch (_: IllegalArgumentException) {}
        nus.close()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    // ---- link ----

    private fun connectNow() {
        val address = mac ?: run { onLinkState("no device configured"); return }
        val mgr = getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val adapter: BluetoothAdapter? = mgr.adapter
        if (adapter == null || !adapter.isEnabled) { scheduleReconnect(); return }
        val dev: BluetoothDevice = adapter.getRemoteDevice(address)
        // autoConnect=true hands reconnection to the system, far cheaper than
        // app-driven scanning over long idle periods.
        nus.connect(dev, autoConnect = true)
    }

    private fun scheduleReconnect() {
        handler.postDelayed({ connectNow() }, backoff)
        backoff = (backoff * 2).coerceAtMost(BACKOFF_MAX_MS)
    }

    private fun onLinkState(s: String) {
        status = s
        Log.i(TAG, "link: $s")
        if (s.startsWith("disconnected") || s.startsWith("no ")) scheduleReconnect()
        updateNotification()
    }

    private fun onRingLine(line: String) {
        Log.i(TAG, "ring: $line")
        val p = RingProtocol.parseRingLine(line) ?: return
        when (p.kind) {
            // Reset backoff only on a CONFIRMED link, not merely on
            // STATE_CONNECTED -- a connection that never reaches #BOOT is not
            // a working one.
            "BOOT", "VER" -> {
                backoff = BACKOFF_START_MS
                if (p.fields["wdt_reset"] == "1") {
                    Log.w(TAG, "ring recovered from a watchdog reset")
                    status = "ring restarted by watchdog"
                }
            }
            "ALERT" -> status = "alert: ${p.fields.keys.firstOrNull() ?: "?"}"
        }
        updateNotification()
    }

    // ---- readings ----

    private fun onReading(r: Reading) {
        if (!gate.shouldSend(r.epochMs)) return          // xDrip+ re-broadcasts
        val line = RingProtocol.formatReading(r.mgdl, r.epochMs, r.slopeName) ?: return
        if (!nus.ready) { Log.w(TAG, "link not ready; dropping"); return }
        nus.writeLine(line)
        gate.markSent(r.epochMs)
        lastReading = "${Math.round(r.mgdl)} ${RingProtocol.sanitizeDirection(r.slopeName)}"
        updateNotification()
    }

    // ---- notification ----

    private fun createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val ch = NotificationChannel(CHANNEL, "Ring link", NotificationManager.IMPORTANCE_LOW)
        ch.description = "Keeps the glucose ring connected"
        (getSystemService(NotificationManager::class.java)).createNotificationChannel(ch)
    }

    private fun buildNotification(): Notification {
        val b = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
            Notification.Builder(this, CHANNEL) else @Suppress("DEPRECATION") Notification.Builder(this)
        return b.setContentTitle("Glucose Ring")
            .setContentText("$status - last: $lastReading")
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setOngoing(true)
            .build()
    }

    private fun updateNotification() {
        (getSystemService(NotificationManager::class.java))
            .notify(NOTIF_ID, buildNotification())
    }
}
