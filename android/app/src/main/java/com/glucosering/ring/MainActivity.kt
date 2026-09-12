package com.glucosering.ring

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.bluetooth.BluetoothManager
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.ActivityNotFoundException
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.os.PowerManager
import android.provider.Settings
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/**
 * Permissions, choosing the ring, and a view of the relay's status.
 *
 * Refusing a permission never crashes anything: the relay keeps running, and
 * its status says what it cannot do.
 */
@SuppressLint("MissingPermission", "BatteryLife", "SetTextI18n") // scan is behind hasScanPermission()
class MainActivity : Activity() {

    companion object {
        private const val REQ_PERMS = 1
        private const val SCAN_MS = 10_000L
        private const val RING_NAME = "GlucoseRing"   // glucose-ring.ino DEVICE_NAME, sent in the scan response
    }

    private lateinit var statusView: TextView
    private lateinit var noteView: TextView
    private val main = Handler(Looper.getMainLooper())
    private var scanner: BluetoothLeScanner? = null

    private val refresher = object : Runnable {
        override fun run() {
            val s = RingService.current
            statusView.text = if (s == null) "Relay not running." else "${s.title}\n\n${s.text}"
            main.postDelayed(this, 5_000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        statusView = TextView(this).apply { textSize = 16f }
        noteView = TextView(this).apply { setPadding(0, 32, 0, 0) }
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 96, 48, 48)
            addView(statusView)
            addView(button("Find ring") { findRing() })
            addView(button("Start relay") { RingService.start(this@MainActivity) })
            addView(button("Allow running in background") { askBatteryExemption() })
            addView(noteView)
        }
        setContentView(ScrollView(this).apply { addView(root) })

        val missing = missingPermissions()
        if (missing.isEmpty()) RingService.start(this) else requestPermissions(missing.toTypedArray(), REQ_PERMS)
    }

    override fun onResume() {
        super.onResume()
        main.post(refresher)
    }

    override fun onPause() {
        main.removeCallbacks(refresher)
        stopScan()
        super.onPause()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, results: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, results)
        val missing = missingPermissions()
        noteView.text = buildString {
            if (missing.any { it != Manifest.permission.POST_NOTIFICATIONS })
                append("Bluetooth permission refused: the relay cannot reach the ring. " +
                    "Grant it in Settings > Apps > Glucose Ring > Permissions.\n\n")
            if (Manifest.permission.POST_NOTIFICATIONS in missing)
                append("Notifications refused: the relay's status is hidden outside this screen, " +
                    "so you will not see it when relaying stops.\n\n")
        }
        RingService.start(this)   // runs regardless; its status reports what it cannot do
    }

    private fun button(label: String, onClick: () -> Unit) =
        Button(this).apply { text = label; setOnClickListener { onClick() } }

    private fun neededPermissions(): List<String> = buildList {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            add(Manifest.permission.BLUETOOTH_CONNECT)
            add(Manifest.permission.BLUETOOTH_SCAN)
        } else {
            add(Manifest.permission.ACCESS_FINE_LOCATION)   // BLE scanning on API 30 and below
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) add(Manifest.permission.POST_NOTIFICATIONS)
    }

    private fun missingPermissions() =
        neededPermissions().filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }

    private fun hasScanPermission() = missingPermissions().none { it != Manifest.permission.POST_NOTIFICATIONS }

    // ---- choosing the ring ----

    private fun findRing() {
        if (!hasScanPermission()) {
            noteView.text = "Finding the ring needs the Bluetooth permission."
            requestPermissions(missingPermissions().toTypedArray(), REQ_PERMS)
            return
        }
        val adapter = getSystemService(BluetoothManager::class.java).adapter
        if (adapter == null || !adapter.isEnabled) {
            noteView.text = "Turn Bluetooth on first."
            return
        }
        val s = adapter.bluetoothLeScanner ?: run { noteView.text = "Bluetooth scanner unavailable."; return }
        stopScan()
        scanner = s
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(NusClient.NUS_SERVICE)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        s.startScan(listOf(filter), settings, scanCallback)
        noteView.text = "Looking for the ring for 10 s. Keep it close."
        main.postDelayed(scanTimeout, SCAN_MS)
    }

    private val scanTimeout = Runnable {
        stopScan()
        noteView.text = "No ring found. Is it charged, nearby, and not connected to another phone?"
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            // The service filter matches any Nordic UART device; require the ring's name too.
            if (result.scanRecord?.deviceName != RING_NAME) return
            stopScan()
            getSharedPreferences(RingService.PREFS, MODE_PRIVATE).edit()
                .putString(RingService.PREF_MAC, result.device.address).apply()
            noteView.text = "Found the ring (${result.device.address}). Connecting; accept the pairing request if Android asks."
            RingService.start(this@MainActivity, reconnect = true)
        }

        override fun onScanFailed(errorCode: Int) {
            stopScan()
            noteView.text = "Scan failed (error $errorCode). Try again."
        }
    }

    private fun stopScan() {
        main.removeCallbacks(scanTimeout)
        try {
            scanner?.stopScan(scanCallback)
        } catch (_: IllegalStateException) {   // Bluetooth was turned off mid-scan
        }
        scanner = null
    }

    // ---- background survival ----

    /** Without this, OEM power management can kill the relay with no warning (PHONE_APP.md section 8). */
    private fun askBatteryExemption() {
        if (getSystemService(PowerManager::class.java).isIgnoringBatteryOptimizations(packageName)) {
            noteView.text = "Already allowed to run in the background."
            return
        }
        try {
            startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName")))
        } catch (_: ActivityNotFoundException) {
            startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
        }
    }
}
