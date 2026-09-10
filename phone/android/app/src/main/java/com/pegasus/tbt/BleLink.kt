package com.pegasus.tbt

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Keeps a GATT connection to the head unit and writes TBT frames to it.
 *
 * Deliberately simple: one device, one characteristic, write-without-response,
 * reconnect on drop. Turn updates are disposable -- if one write is lost the
 * next one a second later corrects it -- so there is no queue or retry logic.
 */
@SuppressLint("MissingPermission") // callers check; see MainActivity
class BleLink(private val context: Context) {

    companion object {
        private const val TAG = "PegasusBle"
        private const val RECONNECT_DELAY_MS = 3_000L
        private const val SCAN_TIMEOUT_MS = 20_000L

        /** Maps updates distance constantly; this is plenty for a head unit. */
        const val MIN_WRITE_INTERVAL_MS = 500L
    }

    private val handler = Handler(Looper.getMainLooper())
    private val scanning = AtomicBoolean(false)

    private var gatt: BluetoothGatt? = null
    private var characteristic: BluetoothGattCharacteristic? = null
    private var lastWriteAt = 0L
    private var lastFrame: ByteArray? = null

    @Volatile
    var isConnected = false
        private set

    /** Called on state changes so the UI can show something honest. */
    var onStatus: ((String) -> Unit)? = null

    private val adapter: BluetoothAdapter?
        get() = (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

    fun start() {
        val bt = adapter
        if (bt == null || !bt.isEnabled) {
            report("Bluetooth is off")
            return
        }
        if (isConnected || scanning.get()) return
        scanForDevice()
    }

    fun stop() {
        stopScan()
        gatt?.close()
        gatt = null
        characteristic = null
        isConnected = false
    }

    private fun scanForDevice() {
        val scanner = adapter?.bluetoothLeScanner ?: run {
            report("No BLE scanner")
            return
        }
        if (!scanning.compareAndSet(false, true)) return

        report("Looking for ${TbtFrame.DEVICE_NAME}…")

        // Filter on the service UUID rather than the name: the firmware puts
        // it in the advertisement, and name matching is unreliable when the
        // name lands in the scan response instead.
        val filter = ScanFilter.Builder()
            .setServiceUuid(android.os.ParcelUuid(TbtFrame.SERVICE_UUID))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        scanner.startScan(listOf(filter), settings, scanCallback)
        handler.postDelayed({
            if (scanning.get() && !isConnected) {
                stopScan()
                report("Not found; retrying")
                handler.postDelayed({ start() }, RECONNECT_DELAY_MS)
            }
        }, SCAN_TIMEOUT_MS)
    }

    private fun stopScan() {
        if (scanning.compareAndSet(true, false)) {
            runCatching { adapter?.bluetoothLeScanner?.stopScan(scanCallback) }
        }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            stopScan()
            connect(result.device)
        }

        override fun onScanFailed(errorCode: Int) {
            scanning.set(false)
            report("Scan failed ($errorCode)")
            handler.postDelayed({ start() }, RECONNECT_DELAY_MS)
        }
    }

    private fun connect(device: BluetoothDevice) {
        report("Connecting…")
        gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                report("Connected; discovering…")
                g.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                isConnected = false
                characteristic = null
                g.close()
                gatt = null
                report("Disconnected; retrying")
                handler.postDelayed({ start() }, RECONNECT_DELAY_MS)
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val chr = g.getService(TbtFrame.SERVICE_UUID)
                ?.getCharacteristic(TbtFrame.CHARACTERISTIC_UUID)
            if (chr == null) {
                report("TBT characteristic missing")
                g.disconnect()
                return
            }
            characteristic = chr
            isConnected = true
            report("Ready")

            // A larger MTU lets a full 39-byte frame through; the 23-byte
            // default caps the street name at about 12 bytes.
            g.requestMtu(64)
        }
    }

    /**
     * Sends a frame, dropping it if an identical one was just sent or if the
     * rate limit hasn't elapsed. Returns true if it actually went out.
     */
    fun send(frame: ByteArray, force: Boolean = false): Boolean {
        val chr = characteristic ?: return false
        val g = gatt ?: return false

        val now = System.currentTimeMillis()
        if (!force) {
            if (now - lastWriteAt < MIN_WRITE_INTERVAL_MS) return false
            if (lastFrame != null && lastFrame.contentEquals(frame)) return false
        }

        val ok = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(
                chr, frame, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            ) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                chr.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                chr.value = frame
                g.writeCharacteristic(chr)
            }
        }

        if (ok) {
            lastWriteAt = now
            lastFrame = frame
        }
        return ok
    }

    private fun report(message: String) {
        Log.i(TAG, message)
        handler.post { onStatus?.invoke(message) }
    }
}
