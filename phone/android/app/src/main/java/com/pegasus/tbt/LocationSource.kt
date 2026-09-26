package com.pegasus.tbt

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Build
import android.os.Looper
import android.util.Log
import androidx.core.content.ContextCompat
import java.util.Calendar
import java.util.TimeZone

/**
 * The phone's own position, forwarded to the head unit.
 *
 * ---------------------------------------------------------------------------
 * Why LocationManager and not FusedLocationProviderClient
 * ---------------------------------------------------------------------------
 * Fused is the usual answer and is better at power and at indoor accuracy. It
 * also drags in Google Play Services, and this project has already paid once
 * for a dependency that turned out to need an account -- the Mapbox SDK sits
 * behind a flag for exactly that reason. LocationManager is in AOSP, needs
 * nothing, and GPS_PROVIDER on a bike outdoors is the raw GNSS fix, which is
 * what the head unit wants anyway.
 *
 * It is deliberately NOT tied to the Mapbox route source. That path has a
 * location stream of its own through the Navigation SDK, but it is opt-in and
 * unbuilt by default -- binding the whole position feature to it would mean
 * the head unit has no fix on precisely the build that works today.
 */
class LocationSource(private val context: Context) {

    companion object {
        private const val TAG = "PegasusGps"

        /**
         * Log one fix in this many, after the first.
         *
         * Fixes arrive at 1Hz for the length of a ride, and logging every one
         * would bury everything else in logcat within a minute. The first is
         * always logged, because "did it ever start" is the question that
         * actually gets asked.
         */
        private const val LOG_EVERY = 30
    }

    private var fixCount = 0L

    /** Called on the main looper with each fix, already encoded for the wire. */
    var onFrame: ((ByteArray) -> Unit)? = null

    private val manager =
        context.getSystemService(Context.LOCATION_SERVICE) as? LocationManager

    private var listening = false

    private val listener = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            val frame = encode(location)
            fixCount++
            if (fixCount == 1L || fixCount % LOG_EVERY == 0L) {
                Log.i(
                    TAG,
                    "fix #$fixCount lat=%.6f lon=%.6f spd=%.1fm/s alt=%.0f hdg=%.0f acc=%.0fm"
                        .format(
                            location.latitude, location.longitude,
                            if (location.hasSpeed()) location.speed else 0f,
                            if (location.hasAltitude()) location.altitude else 0.0,
                            if (location.hasBearing()) location.bearing else 0f,
                            if (location.hasAccuracy()) location.accuracy else -1f,
                        ),
                )
            }
            onFrame?.invoke(frame)
        }

        // Required on API < 30 or the platform throws on some OEM builds.
        @Deprecated("Deprecated in API 29, still called below it")
        override fun onStatusChanged(provider: String?, status: Int, extras: android.os.Bundle?) {
        }

        override fun onProviderEnabled(provider: String) {}
        override fun onProviderDisabled(provider: String) {}
    }

    fun hasPermission(): Boolean =
        ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED

    /**
     * Starts listening, if permission is held and the provider exists.
     *
     * Safe to call repeatedly: the service calls it on every connect, and the
     * rider may grant permission long after the app started.
     */
    fun start() {
        if (listening) return
        val lm = manager
        // Each refusal says which one it was. Silence here is the failure mode
        // that wastes the most time: nothing appears on the head unit and
        // there is no way to tell a missing permission from a provider that
        // never produced a fix.
        if (lm == null) {
            Log.w(TAG, "no LocationManager")
            return
        }
        if (!hasPermission()) {
            Log.w(TAG, "ACCESS_FINE_LOCATION not granted -- nothing will be sent")
            return
        }
        if (!lm.allProviders.contains(LocationManager.GPS_PROVIDER)) {
            Log.w(TAG, "no GPS_PROVIDER on this device")
            return
        }
        if (!lm.isProviderEnabled(LocationManager.GPS_PROVIDER)) {
            // Not a refusal: the rider may switch location on at any moment
            // and the callback starts arriving without anything re-running.
            Log.w(TAG, "GPS_PROVIDER is switched off -- fixes start when it is on")
        }

        // 1000ms and 0m. The head unit's own receiver would give it 1Hz, the
        // ride log thins to five metres itself, and asking for a distance
        // filter here would silence the stream at a traffic light -- where the
        // firmware's "have we stopped" logic still wants to hear a fix saying
        // zero rather than hearing nothing at all.
        try {
            lm.requestLocationUpdates(
                LocationManager.GPS_PROVIDER,
                1000L,
                0f,
                listener,
                Looper.getMainLooper(),
            )
            listening = true
            Log.i(TAG, "listening on GPS_PROVIDER at 1Hz")
        } catch (e: SecurityException) {
            Log.w(TAG, "permission revoked between check and request: ${e.message}")
            // Permission revoked between the check and the call. The head
            // unit falls back to its own receiver.
        }
    }

    fun stop() {
        if (!listening) return
        listening = false
        Log.i(TAG, "stopped after $fixCount fixes")
        try {
            manager?.removeUpdates(listener)
        } catch (_: SecurityException) {
        }
    }

    /**
     * One Location to one wire frame.
     *
     * Internal rather than private so GpsFrameTest can exercise the field
     * mapping -- the encoding itself is GpsFrame's and tested there, but
     * "which Android field goes where" is the part that silently rots.
     */
    internal fun encode(location: Location): ByteArray {
        val cal = Calendar.getInstance(TimeZone.getTimeZone("UTC"))
        cal.timeInMillis = location.time

        // Android reports 0f for "no bearing" and "due north" alike, which is
        // harmless: the head unit only uses heading above walking pace, and a
        // stationary receiver's bearing is noise whatever it says.
        val satellites = location.extras?.getInt("satellites", 0) ?: 0

        return GpsFrame.encode(
            latDegrees = location.latitude,
            lonDegrees = location.longitude,
            speedMetresPerSecond = if (location.hasSpeed()) location.speed else 0f,
            altitudeMetres = if (location.hasAltitude()) location.altitude.toFloat() else 0f,
            headingDegrees = if (location.hasBearing()) location.bearing else 0f,
            numSatellites = satellites,
            // A Location that arrived at all is a fix. The provider does not
            // hand out positions it does not have -- unlike the head unit's
            // receiver, which publishes while still acquiring so the panel can
            // show satellites climbing.
            fixValid = true,
            // location.time is UTC from the provider and always set.
            timeValid = true,
            year = cal.get(Calendar.YEAR),
            month = cal.get(Calendar.MONTH) + 1,
            day = cal.get(Calendar.DAY_OF_MONTH),
            hour = cal.get(Calendar.HOUR_OF_DAY),
            minute = cal.get(Calendar.MINUTE),
            second = cal.get(Calendar.SECOND),
        )
    }
}
