package com.glucosering.ring

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothProfile
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import java.util.UUID

/**
 * BLE client for the ring's Nordic UART Service.
 *
 * The link counts as up only once the ring has ANSWERED (#BOOT or #VER). The
 * ring discards every byte from an unencrypted link without any error, so a
 * connection, a discovery or a successful GATT write proves nothing; only a
 * reply does (PROTOCOL.md section 6). Order: connect, bond, MTU, discover,
 * subscribe, ask !VER until answered.
 *
 * GATT callbacks arrive on a binder thread and are posted to the main thread,
 * so all state here is touched from one thread.
 *
 * Android allows one outstanding GATT operation per connection and silently
 * drops the rest, so writes go through a queue, chunked to the MTU. At the
 * default 23-byte MTU a typical 32-byte reading is two writes.
 */
@SuppressLint("MissingPermission") // RingService checks BLUETOOTH_CONNECT before connect()
class NusClient(
    private val ctx: Context,
    private val onLink: (RingLink) -> Unit,
    private val onLine: (String) -> Unit,
) {
    companion object {
        // PROTOCOL.md, cross-checked against the core's BLEUart.cpp:45-60.
        val NUS_SERVICE: UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
        val NUS_RX: UUID = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")  // phone writes
        val NUS_TX: UUID = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")  // ring notifies
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805F9B34FB")
        private const val TAG = "RingNus"
        private const val DEFAULT_MTU = 23
        private const val CONNECT_TIMEOUT_MS = 40_000L
        private const val PAIRING_TIMEOUT_MS = 60_000L   // the user may have to accept a dialog
        private const val ANSWER_TIMEOUT_MS = 10_000L
        private const val ANSWER_TRIES = 3
    }

    /** How far the connection has got. Only CONFIRMED means the ring reads what we write. */
    var link = RingLink.DISCONNECTED
        private set

    private val main = Handler(Looper.getMainLooper())
    private var device: BluetoothDevice? = null
    private var gatt: BluetoothGatt? = null
    private var rx: BluetoothGattCharacteristic? = null
    private var subscribed = false
    private var mtu = DEFAULT_MTU
    private var assembler = RingProtocol.LineAssembler()
    private val queue = ArrayDeque<ByteArray>()
    private var busy = false
    private var asks = 0

    /** One timer covers every stage, so no stage can hang forever. */
    private val timeout = Runnable {
        if (link == RingLink.HANDSHAKE && subscribed && asks < ANSWER_TRIES) askVersion()
        else fail("no progress while ${link.name.lowercase()}")
    }

    private val bondReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            val d = IntentCompat.getParcelableExtra(i, BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
            if (link != RingLink.PAIRING || d?.address != device?.address) return
            when (i.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.ERROR)) {
                BluetoothDevice.BOND_BONDED -> afterBonded()
                BluetoothDevice.BOND_NONE -> fail("pairing failed or was refused")
            }
        }
    }

    init {
        // Exported because the sender is the Bluetooth stack, not this app. The
        // action is a protected broadcast, so other apps cannot fake it.
        ContextCompat.registerReceiver(
            ctx, bondReceiver, IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED),
            ContextCompat.RECEIVER_EXPORTED
        )
    }

    fun connect(d: BluetoothDevice) {
        close()
        device = d
        setLink(RingLink.CONNECTING)
        armTimeout(CONNECT_TIMEOUT_MS)
        gatt = d.connectGatt(ctx, false, callback, BluetoothDevice.TRANSPORT_LE)
        if (gatt == null) fail("connectGatt returned null")
    }

    /** Queue one protocol line. Dropped unless subscribed; callers send readings only when CONFIRMED. */
    fun writeLine(line: String) {
        if (rx == null || !subscribed) return
        val bytes = line.toByteArray(Charsets.US_ASCII)
        val chunk = (mtu - 3).coerceAtLeast(20)   // ATT opcode + handle
        var off = 0
        while (off < bytes.size) {
            val n = minOf(chunk, bytes.size - off)
            queue.addLast(bytes.copyOfRange(off, off + n))
            off += n
        }
        pump()
    }

    /** Always close() a GATT client before retrying: leaked clients exhaust a small system-wide pool. */
    fun close() {
        main.removeCallbacks(timeout)
        queue.clear()
        busy = false
        gatt?.close()
        gatt = null
        rx = null
        subscribed = false
        mtu = DEFAULT_MTU
        asks = 0
        assembler = RingProtocol.LineAssembler()
    }

    /** Final teardown from Service.onDestroy; deliberately does not call onLink. */
    fun shutdown() {
        close()
        link = RingLink.DISCONNECTED
        try {
            ctx.unregisterReceiver(bondReceiver)
        } catch (_: IllegalArgumentException) {
        }
    }

    private fun fail(why: String) {
        Log.i(TAG, "link down: $why")
        close()
        setLink(RingLink.DISCONNECTED)
    }

    private fun setLink(l: RingLink) {
        if (l == link) return
        link = l
        onLink(l)
    }

    private fun armTimeout(ms: Long) {
        main.removeCallbacks(timeout)
        main.postDelayed(timeout, ms)
    }

    private fun onConnected() {
        val d = device ?: return
        when (d.bondState) {
            BluetoothDevice.BOND_BONDED -> afterBonded()
            BluetoothDevice.BOND_BONDING -> { setLink(RingLink.PAIRING); armTimeout(PAIRING_TIMEOUT_MS) }
            else -> {
                setLink(RingLink.PAIRING)
                armTimeout(PAIRING_TIMEOUT_MS)
                // The ring also requests pairing, so the OS may already be bonding.
                if (!d.createBond() && d.bondState != BluetoothDevice.BOND_BONDING) fail("could not start pairing")
            }
        }
    }

    /** Discovery only after bonding: discovering across a bonding transition can return a stale table. */
    private fun afterBonded() {
        setLink(RingLink.HANDSHAKE)
        armTimeout(ANSWER_TIMEOUT_MS * ANSWER_TRIES)   // covers MTU, discovery and subscribe
        val g = gatt ?: return
        if (!g.requestMtu(247) && !g.discoverServices()) fail("service discovery failed to start")
    }

    /** A !VER sent before the link is encrypted is discarded by the ring, hence the retries. */
    private fun askVersion() {
        asks++
        writeLine(RingProtocol.cmdVersion())
        armTimeout(ANSWER_TIMEOUT_MS)
    }

    @Suppress("DEPRECATION") // value-on-characteristic write API: still works at targetSdk 34
    private fun pump() {
        if (busy) return
        val g = gatt ?: return
        val c = rx ?: return
        val next = queue.removeFirstOrNull() ?: return
        busy = true
        c.value = next
        c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT   // with response
        if (!g.writeCharacteristic(c)) fail("write could not be started")
    }

    private val callback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            main.post {
                if (g !== gatt) return@post   // late callback from a client already closed
                // Status 133 is Android's catch-all; an ordinary retry, not a bug.
                if (newState == BluetoothProfile.STATE_CONNECTED) onConnected()
                else fail("disconnected (status $status)")
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            main.post {
                if (g !== gatt) return@post
                mtu = if (status == BluetoothGatt.GATT_SUCCESS) newMtu else DEFAULT_MTU
                if (!g.discoverServices()) fail("service discovery failed to start")
            }
        }

        @Suppress("DEPRECATION")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            main.post {
                if (g !== gatt) return@post
                val svc = g.getService(NUS_SERVICE)
                val tx = svc?.getCharacteristic(NUS_TX)
                val cccd = tx?.getDescriptor(CCCD)
                rx = svc?.getCharacteristic(NUS_RX)
                if (rx == null || cccd == null) {
                    fail("ring's UART service not found")
                    return@post
                }
                // Both steps are required: the first alone does nothing on the wire.
                g.setCharacteristicNotification(tx, true)
                cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                if (!g.writeDescriptor(cccd)) fail("could not subscribe to the ring")
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            main.post {
                if (g !== gatt) return@post
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    fail("subscribe failed (status $status)")
                    return@post
                }
                subscribed = true
                askVersion()
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            main.post {
                if (g !== gatt) return@post
                busy = false
                // A failed with-response write means part of a line never
                // arrived. Drop the link; the relay resends after reconnecting.
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    fail("write failed (status $status)")
                    return@post
                }
                pump()
            }
        }

        // The framework's newer 3-argument overload delegates here by default.
        @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (c.uuid != NUS_TX) return
            val data = c.value?.copyOf() ?: return   // copy before leaving the binder thread
            main.post {
                if (g !== gatt) return@post
                for (line in assembler.feed(String(data, Charsets.US_ASCII))) {
                    if (link == RingLink.HANDSHAKE && (line.startsWith("#BOOT") || line.startsWith("#VER"))) {
                        main.removeCallbacks(timeout)
                        setLink(RingLink.CONFIRMED)
                    }
                    onLine(line)
                }
            }
        }
    }
}
