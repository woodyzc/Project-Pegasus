package com.pegasus.tbt

import java.util.Calendar
import java.util.TimeZone

/**
 * The clock the head unit runs on until a GNSS module is fitted.
 *
 * Must stay byte-for-byte identical to src/system/ClockFrame.h in the firmware
 * -- that file is the authority, this is the encoder for it.
 *
 *   off  size  field
 *   0    1     magic       0x43 ('C')
 *   1    1     version     0x01
 *   2    4     utc_seconds uint32, seconds since the Unix epoch, UTC
 *   6    2     offset_min  int16, local time minus UTC, in minutes
 *   8    1     zone_len    0..7 bytes of ASCII that follow
 *   9    n     zone        "EDT", "CEST", ...
 *
 * Bluetooth defines a Current Time Service for exactly this job and it is
 * unusable here: that service needs the PHONE to be the server, and Android
 * does not implement one. So the time rides on the service this app already
 * writes to.
 *
 * UTC and the offset travel separately rather than sending local time. A head
 * unit handed local time cannot log UTC, cannot compare itself against a GNSS
 * fix, and cannot tell that the rider has crossed a zone.
 */
object ClockFrame {

    val CHARACTERISTIC_UUID: java.util.UUID =
        java.util.UUID.fromString("a3c87504-8ed3-4bdf-8a39-a01bebede295")

    const val MAGIC: Byte = 0x43
    const val VERSION: Byte = 0x01
    const val HEADER_LEN = 9
    const val ZONE_MAX_BYTES = 7

    /**
     * How often to re-send. The head unit keeps counting on its own tick
     * between writes, so this is about correcting that tick's drift and about
     * catching a daylight-saving change, not about keeping the clock alive.
     */
    const val RESEND_INTERVAL_MS = 5L * 60L * 1000L

    /** The current time, ready to write. */
    fun now(): ByteArray = encode(System.currentTimeMillis(), TimeZone.getDefault())

    fun encode(epochMillis: Long, zone: TimeZone): ByteArray {
        // getOffset, not getRawOffset: the raw offset ignores daylight saving,
        // which would put the head unit an hour out for half the year in most
        // of the world.
        val offsetMin = zone.getOffset(epochMillis) / 60_000

        // The abbreviation for the date in question, so a frame sent in
        // summer says BST rather than GMT. Truncated rather than refused:
        // a few zones have long names and the abbreviation is a caption.
        val name = zone.getDisplayName(zone.inDaylightTime(java.util.Date(epochMillis)), TimeZone.SHORT)
        val zoneBytes = truncateAscii(name, ZONE_MAX_BYTES)

        val frame = ByteArray(HEADER_LEN + zoneBytes.size)
        frame[0] = MAGIC
        frame[1] = VERSION

        val seconds = epochMillis / 1000L
        frame[2] = (seconds and 0xFF).toByte()
        frame[3] = ((seconds shr 8) and 0xFF).toByte()
        frame[4] = ((seconds shr 16) and 0xFF).toByte()
        frame[5] = ((seconds shr 24) and 0xFF).toByte()

        frame[6] = (offsetMin and 0xFF).toByte()
        frame[7] = ((offsetMin shr 8) and 0xFF).toByte()

        frame[8] = zoneBytes.size.toByte()
        zoneBytes.copyInto(frame, HEADER_LEN)
        return frame
    }

    /**
     * ASCII only, and never longer than the firmware's buffer.
     *
     * Android returns names like "GMT+05:30" for zones with no abbreviation,
     * and those exceed seven characters. The firmware rejects a frame whose
     * declared zone length is too long, so a name that does not fit is dropped
     * entirely rather than truncated into something misleading -- the head
     * unit then prints the numeric offset itself, which is what "GMT+05:30"
     * was trying to say anyway.
     */
    private fun truncateAscii(text: String, maxBytes: Int): ByteArray {
        val ascii = text.filter { it.code in 33..126 }
        if (ascii.isEmpty() || ascii.length > maxBytes) return ByteArray(0)
        return ascii.toByteArray(Charsets.US_ASCII)
    }

    /** Midnight-safe local time-of-day, for tests and for a display. */
    fun localSecondsOfDay(epochMillis: Long, zone: TimeZone): Int {
        val cal = Calendar.getInstance(zone)
        cal.timeInMillis = epochMillis
        return cal.get(Calendar.HOUR_OF_DAY) * 3600 +
            cal.get(Calendar.MINUTE) * 60 +
            cal.get(Calendar.SECOND)
    }
}
