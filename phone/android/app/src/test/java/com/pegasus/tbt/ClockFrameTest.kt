package com.pegasus.tbt

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.TimeZone

/**
 * Pins the clock encoder to the firmware's layout in src/system/ClockFrame.h.
 * If either side drifts, this is what should notice.
 */
class ClockFrameTest {

    // 2026-09-16T18:30:00Z.
    private val epochMillis = 1789583400_000L

    private fun le32(frame: ByteArray, off: Int): Long =
        (frame[off].toLong() and 0xFF) or
            ((frame[off + 1].toLong() and 0xFF) shl 8) or
            ((frame[off + 2].toLong() and 0xFF) shl 16) or
            ((frame[off + 3].toLong() and 0xFF) shl 24)

    private fun le16(frame: ByteArray, off: Int): Int =
        ((frame[off].toInt() and 0xFF) or ((frame[off + 1].toInt() and 0xFF) shl 8)).toShort().toInt()

    @Test
    fun `header matches the firmware layout byte for byte`() {
        val frame = ClockFrame.encode(epochMillis, TimeZone.getTimeZone("UTC"))
        assertEquals(0x43.toByte(), frame[0])
        assertEquals(0x01.toByte(), frame[1])
        assertEquals(1789583400L, le32(frame, 2))
        assertEquals(0, le16(frame, 6))
        assertEquals("UTC", String(frame, 9, frame[8].toInt(), Charsets.US_ASCII))
    }

    @Test
    fun `a western offset is negative and little-endian`() {
        // New York in September is on daylight time, four hours behind UTC.
        val frame = ClockFrame.encode(epochMillis, TimeZone.getTimeZone("America/New_York"))
        assertEquals(-240, le16(frame, 6))
        assertEquals("EDT", String(frame, 9, frame[8].toInt(), Charsets.US_ASCII))
    }

    @Test
    fun `daylight saving is applied, not the raw offset`() {
        // London is +60 in September and +0 in January. Sending the raw
        // offset would put the head unit an hour out for half the year.
        val london = TimeZone.getTimeZone("Europe/London")
        assertEquals(60, le16(ClockFrame.encode(epochMillis, london), 6))

        val january = 1767225600_000L // 2026-01-01T00:00:00Z
        assertEquals(0, le16(ClockFrame.encode(january, london), 6))
    }

    @Test
    fun `a half hour offset survives`() {
        val frame = ClockFrame.encode(epochMillis, TimeZone.getTimeZone("Asia/Kolkata"))
        assertEquals(330, le16(frame, 6))
    }

    @Test
    fun `a zone with no abbreviation sends none rather than a bad one`() {
        // A zone with no name of its own displays as "GMT+05:45", nine
        // characters against the firmware's seven. Dropping it makes the head
        // unit print the numeric offset itself, which is what that name was
        // trying to say. Named zones are unaffected: Kathmandu, on the same
        // offset, has the abbreviation NPT and keeps it.
        val unnamed = ClockFrame.encode(epochMillis, TimeZone.getTimeZone("GMT+05:45"))
        assertEquals(0, unnamed[8].toInt())
        assertEquals(ClockFrame.HEADER_LEN, unnamed.size)
        assertEquals(345, le16(unnamed, 6))

        val named = ClockFrame.encode(epochMillis, TimeZone.getTimeZone("Asia/Kathmandu"))
        assertEquals("NPT", String(named, 9, named[8].toInt(), Charsets.US_ASCII))
        assertEquals(345, le16(named, 6))
    }

    @Test
    fun `the zone never exceeds the firmware's buffer`() {
        for (id in TimeZone.getAvailableIDs()) {
            val frame = ClockFrame.encode(epochMillis, TimeZone.getTimeZone(id))
            val len = frame[8].toInt()
            assertTrue("$id declared $len bytes", len <= ClockFrame.ZONE_MAX_BYTES)
            assertEquals("$id length disagrees with size", ClockFrame.HEADER_LEN + len, frame.size)
        }
    }

    @Test
    fun `a known frame is exactly these bytes`() {
        val expected = byteArrayOf(
            0x43, 0x01,
            0x28, 0xE0.toByte(), 0xAA.toByte(), 0x6A, // 1789583400 seconds
            0x10, 0xFF.toByte(),                      // -240 minutes
            0x03,
            0x45, 0x44, 0x54                          // "EDT"
        )
        assertArrayEquals(
            expected,
            ClockFrame.encode(epochMillis, TimeZone.getTimeZone("America/New_York"))
        )
    }

    @Test
    fun `now produces a frame the firmware would accept`() {
        val frame = ClockFrame.now()
        assertEquals(0x43.toByte(), frame[0])
        assertEquals(0x01.toByte(), frame[1])
        // CLOCK_EPOCH_FLOOR in the firmware: anything earlier is refused.
        assertTrue(le32(frame, 2) >= 1704067200L)
        assertTrue(le16(frame, 6) in -720..840)
        assertTrue(frame[8].toInt() <= ClockFrame.ZONE_MAX_BYTES)
    }
}
