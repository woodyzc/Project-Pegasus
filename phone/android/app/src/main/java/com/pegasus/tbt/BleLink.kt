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
import android.bluetooth.BluetoothStatusCodes
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

        /**
         * How long one connect-time GATT step may take before the chain gives
         * up on it and moves on.
         *
         * The chain must never wedge: a head unit that simply never answers a
         * descriptor write would otherwise leave the link permanently without
         * an MTU and without a clock, and the only symptom would be route
         * uploads failing at "0 of n".
         */
        const val SETUP_STEP_TIMEOUT_MS = 3000L

        /** How soon to try the clock again after the GATT queue refused it. */
        const val CLOCK_RETRY_MS = 2_000L

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
    private var clockCharacteristic: BluetoothGattCharacteristic? = null
    private var gpsCharacteristic: BluetoothGattCharacteristic? = null
    private var alertCharacteristic: BluetoothGattCharacteristic? = null
    private var lastWriteAt = 0L
    private var lastFrame: ByteArray? = null

    // The route upload in progress, if any. The state machine is in
    // RouteTransfer; this class only performs the writes and feeds back what
    // the head unit reports.
    private var transfer: RouteTransfer? = null

    /**
     * The connect-time GATT chain.
     *
     * ⚠️ Android runs ONE GATT operation at a time and silently drops any
     * issued while another is outstanding -- no exception, no callback, no
     * error anywhere. These two used to be fired back to back in
     * onServicesDiscovered, so one of them was always lost:
     *
     *   - lose the MTU request and the link stays at 23 bytes, where every
     *     186-byte route chunk is refused;
     *   - lose the descriptor write and the head unit is never told to notify
     *     progress, so every chunk may land and none is ever acknowledged.
     *
     * Both land on "Route upload failed (0 of n chunks)", and neither touches
     * turns or alerts -- those are small enough for the default MTU and need
     * nothing notified back -- which is exactly how this survived so long:
     * navigation worked, so the link looked healthy.
     *
     * So the steps are chained, each started by the previous one's callback.
     */
    private var cccdPending = false
    private var mtuPending = false

    /**
     * What the MTU exchange actually settled on, so a chunk that cannot fit
     * is reported rather than written into a refusal. 23 is the ATT default
     * and the value in force if the exchange never happened.
     */
    private var negotiatedMtu = 23

    /**
     * Upload counters, for the failure message.
     *
     * "0 of n chunks" cannot say whether the writes were attempted, whether
     * the stack took them, whether they completed, or whether anything came
     * back -- and the head unit's own counter says it has never been written
     * to at all. One of the two is wrong about something and these four
     * numbers are what separate them.
     */
    private var wAttempt = 0
    private var wAccept = 0
    private var wDone = 0
    private var nNotify = 0

    @Volatile
    var isConnected = false
        private set

    // Whether the link is meant to be running at all.
    //
    // Every retry in this class is a postDelayed, and a delayed post outlives
    // the thing that scheduled it. Without this, stopping while a scan was in
    // flight left a pending runnable that called start() a few seconds later
    // and brought the whole link back -- which is why the app could be stopped
    // in TBT mode and not in GPX mode. In TBT the head unit advertises, the
    // scan succeeds, and no retry is ever pending; in GPX it advertises
    // nothing, so the app is permanently mid-retry and stopping never caught
    // it at a moment when there was nothing queued.
    @Volatile
    private var running = false

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
        running = true
        scanForDevice()
    }

    fun stop() {
        // First, so anything that slips through below finds the door shut.
        running = false
        // Everything this class schedules goes through this handler, so one
        // call cancels the lot: the scan timeout, the reconnect, the clock.
        // Naming each runnable and removing them one by one is the version
        // that quietly misses the next one somebody adds.
        handler.removeCallbacksAndMessages(null)
        stopScan()
        gatt?.close()
        gatt = null
        characteristic = null
        routeCharacteristic = null
        statusCharacteristic = null
        clockCharacteristic = null
        gpsCharacteristic = null
        alertCharacteristic = null
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
        // A chunk write needs three bytes of ATT header on top of the frame.
        // Checked here because the alternative is writing 186 bytes into a
        // 23-byte MTU fifteen times and reporting "0 of 15 chunks", which
        // says nothing about why.
        val needed = RouteFrame.CHUNK_HEADER_LEN + RouteFrame.CHUNK_PAYLOAD + 3
        if (negotiatedMtu < needed) {
            report("Route needs MTU $needed, link has $negotiatedMtu")
            return
        }
        wAttempt = 0
        wAccept = 0
        wDone = 0
        nNotify = 0
        transfer = RouteTransfer(encoded.chunks)
        report("Sending route (${encoded.chunks.size} chunks)")
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
                // already in flight. Give the chunk back before stopping:
                // nextChunk has already advanced past it, so without this it
                // is simply never sent, and a burst loses roughly every other
                // one. Then let the write callback or the tick resume;
                // retrying here would spin.
                t.onWriteRefused()
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
                report(uploadFailureReport(t))
                transfer = null
            }
            else -> handler.postDelayed(::tickTransfer, RouteTransfer.STALL_TIMEOUT_MS)
        }
    }

    /**
     * Why the upload gave up, in the only terms available after the fact.
     *
     * "0 of n" and "k of n" are different faults and the bare message could
     * not tell them apart: zero means the head unit never acknowledged
     * anything -- no progress notification, or every chunk rejected -- while a
     * partial count means the transfer was moving and then stopped.
     */
    private fun uploadFailureReport(t: RouteTransfer): String =
        "Route upload failed (${t.acknowledged} of ${t.totalChunks} chunks," +
            " pass ${t.pass}) try=$wAttempt ok=$wAccept done=$wDone notif=$nNotify" +
            " mtu=$negotiatedMtu conn=$isConnected chr=${routeCharacteristic != null}"


    private fun tickTransfer() {
        val t = transfer ?: return
        if (!t.onTick(System.currentTimeMillis())) {
            report(uploadFailureReport(t))
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
        wAttempt++
        // WRITE_TYPE_DEFAULT, not NO_RESPONSE: a dropped route chunk is a
        // permanent hole, where a dropped turn is corrected a second later.
        val ok = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(
                chr, chunk, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            ) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                chr.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                chr.value = chunk
                g.writeCharacteristic(chr)
            }
        }
        if (ok) wAccept++
        return ok
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
            if (running && scanning.get() && !isConnected) {
                stopScan()
                report("Not found; retrying")
                handler.postDelayed({ if (running) start() }, RECONNECT_DELAY_MS)
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
            handler.postDelayed({ if (running) start() }, RECONNECT_DELAY_MS)
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
                clockCharacteristic = null
                // These two were missing from the list. Harmless today,
                // because `gatt` is nulled below and every sender checks it
                // first -- but a list that is right for four of six entries
                // invites the next reader to trust it.
                gpsCharacteristic = null
                alertCharacteristic = null
                handler.removeCallbacks(clockTick)
                // The chain does not survive the link: a callback for the old
                // connection must not advance a new one's setup.
                handler.removeCallbacks(setupWatchdog)
                cccdPending = false
                mtuPending = false
                negotiatedMtu = 23
                // The transfer survives the drop and resumes on reconnect --
                // see RouteTransfer.onDisconnected, which deliberately does
                // not spend one of its attempts on a reconnect.
                transfer?.onDisconnected()
                g.close()
                gatt = null
                report("Disconnected; retrying")
                handler.postDelayed({ if (running) start() }, RECONNECT_DELAY_MS)
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
            clockCharacteristic = service.getCharacteristic(ClockFrame.CHARACTERISTIC_UUID)
            alertCharacteristic = service.getCharacteristic(AlertFrame.CHARACTERISTIC_UUID)
            // Absent on firmware older than the position feature. Null is the
            // whole handling: sendGps() returns and the head unit uses its own
            // receiver, exactly as it did before this existed.
            gpsCharacteristic = service.getCharacteristic(GpsFrame.CHARACTERISTIC_UUID)

            isConnected = true
            report(if (routeCharacteristic != null) "Ready" else "Ready (no route support)")

            // Step one of the chain, and nothing else may be issued until its
            // callback arrives. The MTU request follows from onDescriptorWrite
            // and the clock from onMtuChanged; see cccdPending above for what
            // happens when they are issued together instead.
            cccdPending = statusCharacteristic?.let { subscribeToStatus(g, it) } ?: false
            if (cccdPending) {
                armSetupWatchdog()
            } else {
                // No status characteristic, or the write could not be issued.
                // Either way there is nothing outstanding, so go straight on.
                requestMtuStep(g)
            }
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int,
        ) {
            if (descriptor.uuid != CCCD_UUID || !cccdPending) {
                return
            }
            cccdPending = false
            handler.removeCallbacks(setupWatchdog)
            if (status != BluetoothGatt.GATT_SUCCESS) {
                // Said out loud rather than swallowed: without the CCCD the
                // head unit sends no progress and every upload ends at "0 of
                // n" however well the chunks themselves go out.
                report("Route progress unavailable (CCCD $status)")
            }
            requestMtuStep(g)
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            if (!mtuPending) {
                return
            }
            mtuPending = false
            handler.removeCallbacks(setupWatchdog)
            negotiatedMtu = if (status == BluetoothGatt.GATT_SUCCESS) mtu else 23
            finishConnectSetup()
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
                wDone++
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
        nNotify++
        if (value.size < 4) return
        val received = (value[0].toInt() and 0xFF) or ((value[1].toInt() and 0xFF) shl 8)

        val t = transfer ?: return
        t.onProgress(received, System.currentTimeMillis())
        handler.post(::pumpTransfer)
    }

    /**
     * Returns true when a descriptor write was actually issued, and the
     * caller must therefore wait for onDescriptorWrite before issuing
     * anything else. False means nothing is outstanding and the chain can
     * move on immediately.
     *
     * The return value is the point. Issuing this and then not knowing
     * whether it is in flight is what let the MTU request be fired on top of
     * it.
     */
    private fun subscribeToStatus(g: BluetoothGatt, chr: BluetoothGattCharacteristic): Boolean {
        g.setCharacteristicNotification(chr, true)
        // Enabling notifications locally is not enough: the descriptor write
        // is what tells the head unit to send them. Skipping it is a classic
        // silent failure -- everything looks connected and nothing arrives.
        val cccd = chr.getDescriptor(CCCD_UUID) ?: return false
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) ==
                BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                g.writeDescriptor(cccd)
            }
        }
    }

    /**
     * Step two: ask for an MTU big enough to carry a route chunk.
     *
     * A route chunk is 186 bytes on the wire and the ATT default carries 20,
     * so without this the route path cannot work at all -- while turns and
     * alerts, which fit, carry on as though the link were healthy.
     */
    private fun requestMtuStep(g: BluetoothGatt) {
        mtuPending = true
        if (g.requestMtu(ROUTE_MTU)) {
            armSetupWatchdog()
        } else {
            // Could not even be issued. Carry on at whatever MTU is in force
            // rather than wedging the link: turns will work, and sendRoute()
            // will say plainly why the route cannot.
            mtuPending = false
            report("MTU request refused")
            finishConnectSetup()
        }
    }

    /**
     * Step three, once nothing is outstanding: the clock, and any transfer
     * that was queued while the chain was still running.
     *
     * The head unit has no clock of its own until a GNSS module is fitted,
     * and sending one here rather than on the first timer tick is what stops
     * the panel sitting at dashes for the five minutes to the next tick.
     */
    private fun finishConnectSetup() {
        if (transfer != null) {
            handler.post(::pumpTransfer)
        }
        handler.post { sendClock() }
    }

    private fun armSetupWatchdog() {
        handler.removeCallbacks(setupWatchdog)
        handler.postDelayed(setupWatchdog, SETUP_STEP_TIMEOUT_MS)
    }

    /**
     * Moves the chain on when a step's callback never arrives.
     *
     * A chain is only better than a race if it cannot stall. A head unit that
     * accepts a descriptor write and never confirms it would otherwise leave
     * the link with no MTU and no clock for ever.
     */
    private val setupWatchdog = Runnable {
        val g = gatt
        if (g == null) {
            cccdPending = false
            mtuPending = false
        } else if (cccdPending) {
            cccdPending = false
            report("Status subscription unconfirmed; continuing")
            requestMtuStep(g)
        } else if (mtuPending) {
            mtuPending = false
            report("MTU exchange unconfirmed; continuing")
            finishConnectSetup()
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

    /**
     * Writes the current time to the head unit.
     *
     * Unacknowledged: a lost clock frame costs nothing, because the head unit
     * keeps counting on its own tick and another arrives minutes later. That
     * is the opposite of a route chunk, where a lost write is a permanent
     * hole, and it is why this does not go through the route path's machinery.
     */
    /**
     * Lends the head unit this phone's position.
     *
     * Unacknowledged, like the clock and for a sharper version of the same
     * reason: a fix is worthless a second after it is taken, and another is
     * already on its way. Waiting for an ack would delay every fix to avoid
     * losing the occasional one, which is the wrong trade for data that
     * expires.
     *
     * Nothing here decides whether the head unit should listen. It ignores us
     * outright once its own receiver has ever had a fix, so the phone can keep
     * sending without knowing what hardware is at the other end.
     */
    fun sendGps(frame: ByteArray): Boolean {
        val chr = gpsCharacteristic ?: return false
        val g = gatt ?: return false

        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
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
    }

    /**
     * One call, text or chat message, for the head unit's banner.
     *
     * WRITE_NO_RESPONSE, like the position fix and for a milder version of the
     * same reason: an alert that does not arrive is one the rider reads on the
     * phone at the next stop. Paying a round trip per notification to
     * guarantee delivery would buy very little, and the write happens while
     * the rider may be mid-junction.
     *
     * Returns false when the head unit is not connected or is running firmware
     * without this characteristic, so the caller can say which.
     */
    fun sendAlert(frame: ByteArray): Boolean {
        val chr = alertCharacteristic ?: return false
        val g = gatt ?: return false

        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
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
    }

    private fun sendClock() {
        val chr = clockCharacteristic ?: return
        val g = gatt ?: return
        val frame = ClockFrame.now()

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

        // A refusal means the queue was busy, not that the head unit said no.
        // Waiting the full interval after one would leave the panel blank for
        // five minutes over a collision that clears in milliseconds.
        scheduleClock(if (ok) ClockFrame.RESEND_INTERVAL_MS else CLOCK_RETRY_MS)
    }

    private val clockTick = Runnable { sendClock() }

    private fun scheduleClock(delayMs: Long) {
        handler.removeCallbacks(clockTick)
        handler.postDelayed(clockTick, delayMs)
    }

    private fun report(message: String) {
        Log.i(TAG, message)
        handler.post { onStatus?.invoke(message) }
    }
}
