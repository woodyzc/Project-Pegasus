package com.pegasus.tbt

import java.util.UUID
import kotlin.math.roundToInt
import kotlin.math.roundToLong

/**
 * The position fix this phone lends the head unit.
 *
 * Must stay byte-for-byte identical to src/sensors/GpsFrame.h in the firmware
 * repo -- that file is the authority, this is the encoder for it, and
 * GpsFrameTest pins the pair together.
 *
 *   off  size  field
 *   0    1     magic       0x47 ('G')
 *   1    1     version     0x01
 *   2    1     flags       bit0 fixValid, bit1 timeValid
 *   3    1     numSatellites
 *   4    4     lat         int32, degrees * 1e7, little-endian
 *   8    4     lon         int32, degrees * 1e7
 *   12   2     speed       uint16, centimetres per second
 *   14   2     altitude    int16, metres
 *   16   2     heading     uint16, degrees * 100, 0..35999
 *   18   2     year        uint16
 *   20   1..5  month, day, hour, minute, second
 *
 * Fixed 25 bytes, unlike the turn frame's 8..40. Every field is always
 * present, so a short write is unambiguously a truncated one.
 *
 * ---------------------------------------------------------------------------
 * Why the phone sends this at all
 * ---------------------------------------------------------------------------
 * The head unit's MAX-M10S has never been fitted, so everything downstream of
 * a position -- speed, the odometer, the ride log, the map, route snapping,
 * onboard turn-by-turn -- has never run against a real fix. This phone is
 * already connected and already holds location permission.
 *
 * The head unit's own receiver wins permanently once it has ever had a valid
 * fix, so nothing here needs to stand down: it is safe to keep sending, and
 * the firmware simply ignores us.
 */
object GpsFrame {

    val CHARACTERISTIC_UUID: UUID = UUID.fromString("a3c87505-8ed3-4bdf-8a39-a01bebede295")

    const val MAGIC: Byte = 0x47
    const val VERSION: Byte = 0x01
    const val LEN = 25

    const val FLAG_FIX_VALID = 0x01
    const val FLAG_TIME_VALID = 0x02

    /**
     * Encodes one fix. Out-of-range inputs are clamped rather than thrown on:
     * this runs on every location callback while the rider is moving, and a
     * dropped fix is a worse answer than a saturated one.
     *
     * The exception is [heading], which wraps instead of clamping -- 360 and 0
     * are the same direction, and clamping it to 359.99 would be a lie in a
     * way that clamping a speed is not. The firmware refuses 36000 outright.
     */
    fun encode(
        latDegrees: Double,
        lonDegrees: Double,
        speedMetresPerSecond: Float,
        altitudeMetres: Float,
        headingDegrees: Float,
        numSatellites: Int,
        fixValid: Boolean,
        timeValid: Boolean,
        year: Int,
        month: Int,
        day: Int,
        hour: Int,
        minute: Int,
        second: Int,
    ): ByteArray {
        val out = ByteArray(LEN)
        out[0] = MAGIC
        out[1] = VERSION

        var flags = 0
        if (fixValid) flags = flags or FLAG_FIX_VALID
        if (timeValid) flags = flags or FLAG_TIME_VALID
        out[2] = flags.toByte()
        out[3] = numSatellites.coerceIn(0, 255).toByte()

        putI32(out, 4, (latDegrees.coerceIn(-90.0, 90.0) * 1e7).roundToLong().toInt())
        putI32(out, 8, (lonDegrees.coerceIn(-180.0, 180.0) * 1e7).roundToLong().toInt())

        // Negative speeds are not a thing a receiver reports, but Android's
        // Location returns 0f for "unknown" and a synthetic source could
        // return anything.
        val speedCms = (speedMetresPerSecond * 100f).roundToInt().coerceIn(0, 65535)
        putU16(out, 12, speedCms)

        putU16(out, 14, altitudeMetres.roundToInt().coerceIn(-32768, 32767) and 0xFFFF)

        // Wrapped into [0, 360) before scaling, so 360.0 becomes 0 rather than
        // 35999. Android reports bearing in [0, 360), but a wrap here costs
        // nothing and the firmware rejects the frame if we get it wrong.
        var heading = headingDegrees % 360f
        if (heading < 0f) heading += 360f
        putU16(out, 16, (heading * 100f).roundToInt().coerceIn(0, 35999))

        putU16(out, 18, year.coerceIn(0, 65535))
        out[20] = month.coerceIn(0, 255).toByte()
        out[21] = day.coerceIn(0, 255).toByte()
        out[22] = hour.coerceIn(0, 255).toByte()
        out[23] = minute.coerceIn(0, 255).toByte()
        out[24] = second.coerceIn(0, 255).toByte()
        return out
    }

    private fun putU16(buf: ByteArray, offset: Int, value: Int) {
        buf[offset] = (value and 0xFF).toByte()
        buf[offset + 1] = ((value ushr 8) and 0xFF).toByte()
    }

    private fun putI32(buf: ByteArray, offset: Int, value: Int) {
        buf[offset] = (value and 0xFF).toByte()
        buf[offset + 1] = ((value ushr 8) and 0xFF).toByte()
        buf[offset + 2] = ((value ushr 16) and 0xFF).toByte()
        buf[offset + 3] = ((value ushr 24) and 0xFF).toByte()
    }
}
