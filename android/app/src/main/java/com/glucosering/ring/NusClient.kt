package com.glucosering.ring

import android.annotation.SuppressLint
import android.bluetooth.*
import android.content.Context
import android.util.Log
import java.util.ArrayDeque
import java.util.UUID

/**
 * BLE client for the ring's Nordic UART Service.
 *
 * Two things this class exists to get right, both of which look like "it just
 * doesn't work" when wrong:
 *
 *  1. Android allows exactly ONE outstanding GATT operation per connection.
 *     A second write issued before onCharacteristicWrite fires is dropped
 *     silently. Everything therefore goes through a queue.
 *  2. A message may span several writes at the negotiated MTU. At the default
 *     23-byte MTU the payload is 20 bytes and a typical reading is 23, so
 *     chunking is the normal case, not an edge case.
 */
@SuppressLint("MissingPermission")
class NusClient(
    private val ctx: Context,
    private val onLine: (String) -> Unit,
    private val onState: (String) -> Unit
) {
    companion object {
        val NUS_SERVICE: UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
        val NUS_RX: UUID      = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
        val NUS_TX: UUID      = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")
        val CCCD: UUID        = UUID.fromString("00002902-0000-1000-8000-00805F9B34FB")
        private const val TAG = "RingNus"
        private const val DEFAULT_MTU = 23
    }

    private var gatt: BluetoothGatt? = null
    private var rx: BluetoothGattCharacteristic? = null
    private var mtu = DEFAULT_MTU
    private val assembler = RingProtocol.LineAssembler()

    private val queue = ArrayDeque<ByteArray>()
    private var busy = false

    @Volatile var ready = false
        private set

    fun connect(device: BluetoothDevice, autoConnect: Boolean = false) {
        close()
        gatt = device.connectGatt(ctx, autoConnect, callback, BluetoothDevice.TRANSPORT_LE)
    }

    /** Always close before retrying: leaked GATT clients exhaust a small
     *  system-wide pool and eventually no app on the phone can connect. */
    fun close() {
        ready = false
        busy = false
        queue.clear()
        gatt?.close()
        gatt = null
        rx = null
        mtu = DEFAULT_MTU
    }

    fun writeLine(line: String) {
        val bytes = line.toByteArray(Charsets.US_ASCII)
        val chunk = (mtu - 3).coerceAtLeast(20)   // ATT opcode + handle
        var off = 0
        synchronized(queue) {
            while (off < bytes.size) {
                val n = minOf(chunk, bytes.size - off)
                queue.add(bytes.copyOfRange(off, off + n))
                off += n
            }
        }
        pump()
    }

    private fun pump() {
        synchronized(queue) {
            if (busy || queue.isEmpty()) return
            val g = gatt ?: return
            val c = rx ?: return
            val next = queue.poll() ?: return
            busy = true
            @Suppress("DEPRECATION")
            run {
                c.value = next
                c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                if (!g.writeCharacteristic(c)) {
                    busy = false
                    Log.w(TAG, "writeCharacteristic returned false")
                }
            }
        }
    }

    private val callback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                onState("connected")
                g.requestMtu(247)          // discovery happens after the MTU reply
            } else {
                // status 133 (GATT_ERROR) is Android's catch-all; treat it as a
                // normal retry case, not a bug.
                onState("disconnected(status=$status)")
                close()
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            mtu = if (status == BluetoothGatt.GATT_SUCCESS) newMtu else DEFAULT_MTU
            g.discoverServices()
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val svc = g.getService(NUS_SERVICE)
            if (svc == null) { onState("no NUS service"); return }
            rx = svc.getCharacteristic(NUS_RX)
            val tx = svc.getCharacteristic(NUS_TX)
            if (rx == null || tx == null) { onState("NUS characteristics missing"); return }

            // Both steps are required: setCharacteristicNotification alone does
            // nothing on the wire without the CCCD write.
            g.setCharacteristicNotification(tx, true)
            val d = tx.getDescriptor(CCCD)
            if (d != null) {
                @Suppress("DEPRECATION")
                run {
                    d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    g.writeDescriptor(d)
                }
            } else {
                ready = true
                onState("ready (no CCCD)")
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            ready = true
            onState("ready")
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int
        ) {
            synchronized(queue) { busy = false }
            pump()
        }

        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (c.uuid != NUS_TX) return
            val data = c.value ?: return
            for (line in assembler.feed(String(data, Charsets.US_ASCII))) onLine(line)
        }
    }
}
