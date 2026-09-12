package com.pegasus.tbt

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
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
class BleLink(context: Context) {

    // Defensive: this object is owned by TbtService and outlives every
    // Activity, so it must never pin one. Taking applicationContext here means
    // a caller passing an Activity by mistake cannot leak it.
    private val context: Context = context.applicationContext

    companion object {
        private const val TAG = "PegasusBle"
        private const val RECONNECT_DELAY_MS = 3_000L
        private const val SCAN_TIMEOUT_MS = 20_000L

        /**
         * Floor between BLE writes. Riding a bike, a turn prompt that lags by
         * half a second is a bike length of error, so this is deliberately
         * tighter than the 500ms it started at. It exists at all only to stop
         * a burst of identical updates saturating the link -- identical frames
         * are dropped separately, so in practice this rarely bites.
         */
        const val MIN_WRITE_INTERVAL_MS = 150L

        // Re-send an unchanged turn at least this often.
        //
        // The head unit drops a turn it has not heard about for 30 seconds
        // (TBT_STALE_MS in Page_Dashboard.cpp), so silence is not neutral --
        // it actively clears the display. Suppressing every identical frame
        // meant a rider stopped at a light, where the turn and the distance
        // do not change, went quiet and had the turn blanked while waiting at
        // the junction to make it.
        //
        // Well inside the firmware's window, so a couple of lost writes still
        // do not clear a live route.
        const val KEEPALIVE_INTERVAL_MS = 10_000L

        /**
         * MTU to ask for. A route chunk is 186 bytes on the wire and ATT
         * spends 3 bytes of the MTU on its own header, so 189 is the minimum
         * that carries one whole. Asking for a little more costs nothing and
         * leaves room if ROUTE_CHUNK_PAYLOAD ever grows.
         */
        const val ROUTE_MTU = 200

        /** Client Characteristic Configuration, the standard notify switch. */
        val CCCD_UUID: java.util.UUID =
            java.util.UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }

    private val handler = Handler(Looper.getMainLooper())
    private val scanning = AtomicBoolean(false)

    private var gatt: BluetoothGatt? = null
    private var characteristic: BluetoothGattCharacteristic? = null
    private var routeCharacteristic: BluetoothGattCharacteristic? = null
    private var statusCharacteristic: BluetoothGattCharacteristic? = null
    private var lastWriteAt = 0L
    private var lastFrame: ByteArray? = null

    // The route upload in progress, if any. The state machine is in
    // RouteTransfer; this class only performs the writes and feeds back what
    // the head unit reports.
    private var transfer: RouteTransfer? = null

    @Volatile
    var isConnected = false
        private set

    /** Called on state changes so the UI can show something honest. */
    var onStatus: ((String) -> Unit)? = null

    /** Route upload progress, 0..100, and whether it finished. */
    var onRouteProgress: ((percent: Int, done: Boolean) -> Unit)? = null

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
        routeCharacteristic = null
        statusCharacteristic = null
        transfer = null
        isConnected = false
    }

    /**
     * Uploads a planned route for the head unit to navigate from on its own.
     *
     * Replaces any transfer already running: the rider re-planning mid-ride
     * means the old route is not merely stale, it is wrong, and finishing its
     * upload would waste the link on geometry nobody will follow.
     */
    fun sendRoute(encoded: RouteFrame.Encoded) {
        transfer = RouteTransfer(encoded.chunks)
        report("Sending route (\${encoded.chunks.size} chunks)")
        pumpTransfer()
    }

    /**
     * Writes as much of the route as the window allows, then stops. Called
     * again from the status notification and from the stall tick, which is
     * what keeps the transfer moving without a thread of its own.
     */
    private fun pumpTransfer() {
        val t = transfer ?: return
        val chr = routeCharacteristic ?: return
        val g = gatt ?: return

        while (true) {
            val chunk = t.nextChunk(System.currentTimeMillis()) ?: break
            if (!writeChunk(g, chr, chunk)) {
                // The stack refused the write, which on Android means one is
                // already in flight. Stop and let the next notification or
                // tick resume; retrying here would spin.
                break
            }
        }

        onRouteProgress?.invoke(t.percent, t.state == RouteTransfer.State.COMPLETE)

        when (t.state) {
            RouteTransfer.State.COMPLETE -> {
                report("Route sent")
                transfer = null
            }
            RouteTransfer.State.FAILED -> {
                report("Route upload failed")
                transfer = null
            }
            else -> handler.postDelayed(::tickTransfer, RouteTransfer.STALL_TIMEOUT_MS)
        }
    }

    private fun tickTransfer() {
        val t = transfer ?: return
        if (!t.onTick(System.currentTimeMillis())) {
            report("Route upload failed")
            transfer = null
            return
        }
        pumpTransfer()
    }

    private fun writeChunk(
        g: BluetoothGatt,
        chr: BluetoothGattCharacteristic,
        chunk: ByteArray,
    ): Boolean {
        // WRITE_TYPE_DEFAULT, not NO_RESPONSE: a dropped route chunk is a
        // permanent hole, where a dropped turn is corrected a second later.
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(
                chr, chunk, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            ) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                chr.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                chr.value = chunk
                g.writeCharacteristic(chr)
            }
        }
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
                routeCharacteristic = null
                statusCharacteristic = null
                // The transfer survives the drop and resumes on reconnect --
                // see RouteTransfer.onDisconnected, which deliberately does
                // not spend one of its attempts on a reconnect.
                transfer?.onDisconnected()
                g.close()
                gatt = null
                report("Disconnected; retrying")
                handler.postDelayed({ start() }, RECONNECT_DELAY_MS)
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val service = g.getService(TbtFrame.SERVICE_UUID)
            val chr = service?.getCharacteristic(TbtFrame.CHARACTERISTIC_UUID)
            if (chr == null) {
                report("TBT characteristic missing")
                g.disconnect()
                return
            }
            characteristic = chr

            // Both optional: a head unit on older firmware has the turn
            // characteristic and not these, and live turns must keep working
            // against it rather than the whole link being refused.
            routeCharacteristic = service.getCharacteristic(RouteFrame.ROUTE_CHARACTERISTIC_UUID)
            statusCharacteristic = service.getCharacteristic(RouteFrame.STATUS_CHARACTERISTIC_UUID)

            isConnected = true
            report(if (routeCharacteristic != null) "Ready" else "Ready (no route support)")

            statusCharacteristic?.let { subscribeToStatus(g, it) }

            // A larger MTU matters more now than it did for turns alone. A
            // route chunk is 186 bytes on the wire, and at the 23-byte default
            // every one of them would be rejected.
            g.requestMtu(ROUTE_MTU)
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            // Only now is the link able to carry a full chunk, so a transfer
            // queued before this point starts here.
            if (transfer != null) {
                handler.post(::pumpTransfer)
            }
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            chr: BluetoothGattCharacteristic,
            status: Int,
        ) {
            // Android allows one outstanding write at a time, so this is the
            // signal that the queue has room again. Without it the transfer
            // would move only at the stall timeout.
            if (chr.uuid == RouteFrame.ROUTE_CHARACTERISTIC_UUID) {
                handler.post(::pumpTransfer)
            }
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            chr: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            if (chr.uuid == RouteFrame.STATUS_CHARACTERISTIC_UUID) {
                handleStatus(value)
            }
        }

        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            chr: BluetoothGattCharacteristic,
        ) {
            // Pre-Tiramisu callback. Both are needed: the platform calls the
            // one matching the device's API level, and a head unit talking to
            // an older phone would otherwise never report progress.
            if (chr.uuid == RouteFrame.STATUS_CHARACTERISTIC_UUID) {
                handleStatus(chr.value ?: return)
            }
        }
    }

    /** Four bytes: received u16, total u16, little-endian. */
    private fun handleStatus(value: ByteArray) {
        if (value.size < 4) return
        val received = (value[0].toInt() and 0xFF) or ((value[1].toInt() and 0xFF) shl 8)

        val t = transfer ?: return
        t.onProgress(received, System.currentTimeMillis())
        handler.post(::pumpTransfer)
    }

    private fun subscribeToStatus(g: BluetoothGatt, chr: BluetoothGattCharacteristic) {
        g.setCharacteristicNotification(chr, true)
        // Enabling notifications locally is not enough: the descriptor write
        // is what tells the head unit to send them. Skipping it is a classic
        // silent failure -- everything looks connected and nothing arrives.
        val cccd = chr.getDescriptor(CCCD_UUID) ?: return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
        } else {
            @Suppress("DEPRECATION")
            run {
                cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                g.writeDescriptor(cccd)
            }
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
            // Identical frames are suppressed, but only until the keepalive
            // is due: the head unit reads continued silence as "no route".
            if (lastFrame != null && lastFrame.contentEquals(frame) &&
                (now - lastWriteAt) < KEEPALIVE_INTERVAL_MS
            ) {
                return false
            }
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
