package com.pegasus.tbt

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * These lock the encoder to the firmware's src/navigation/TbtParse.h. If one
 * side changes the layout, this test should be what notices.
 */
class TbtFrameTest {

    @Test
    fun `known frame matches the firmware layout byte for byte`() {
        // 'T', v2, turn right, 7-byte name, 250 m little-endian, no exit,
        // "Main St". The exit byte at offset 8 is what version 2 added, and
        // it pushes the name from offset 8 to offset 9.
        val expected = byteArrayOf(
            0x54, 0x02, 0x03, 0x07,
            0xFA.toByte(), 0x00, 0x00, 0x00,
            0x00,
            0x4D, 0x61, 0x69, 0x6E, 0x20, 0x53, 0x74
        )
        assertArrayEquals(expected, TbtFrame.encode(3, 250, "Main St"))
    }

    @Test
    fun `distance is little endian across all four bytes`() {
        // 0x12345678 is far beyond any real distance, but it is the smallest
        // convenient value that puts a distinct byte in each position. Note it
        // must stay positive: Kotlin's Int is signed, so the top half of the
        // wire format's uint32 range is simply not reachable from here -- and
        // does not need to be, since it is 2 million kilometres.
        val frame = TbtFrame.encode(1, 0x12345678, "")
        assertEquals(0x78.toByte(), frame[4])
        assertEquals(0x56.toByte(), frame[5])
        assertEquals(0x34.toByte(), frame[6])
        assertEquals(0x12.toByte(), frame[7])
    }

    @Test(expected = IllegalArgumentException::class)
    fun `negative distance is refused rather than wrapped`() {
        // Without the guard this would go out as a ~4 billion metre uint32,
        // which the head unit has no way to recognise as wrong. -1 is the one
        // exception and has its own test below: it is the agreed sentinel.
        TbtFrame.encode(1, -2, "")
    }

    @Test
    fun `unknown distance goes out as the firmware sentinel`() {
        // TBT_DISTANCE_UNKNOWN in src/navigation/TbtParse.h is 0xFFFFFFFF, and
        // ManeuverParser.DISTANCE_UNKNOWN is -1: the same 32 bits. This is the
        // byte-for-byte pin between the two, which is the only thing that
        // notices if one side is changed alone.
        val frame = TbtFrame.encode(3, ManeuverParser.DISTANCE_UNKNOWN, "Richter Farm Rd")
        assertEquals(0xFF.toByte(), frame[4])
        assertEquals(0xFF.toByte(), frame[5])
        assertEquals(0xFF.toByte(), frame[6])
        assertEquals(0xFF.toByte(), frame[7])
        // The rest of the frame is unaffected: icon and name still travel.
        assertEquals(3.toByte(), frame[2])
        assertEquals("Richter Farm Rd".length.toByte(), frame[3])
    }

    @Test(expected = IllegalArgumentException::class)
    fun `icon id outside the firmware enum is refused`() {
        TbtFrame.encode(11, 100, "")
    }

    @Test
    fun `long names are cut to the firmware limit`() {
        val frame = TbtFrame.encode(1, 100, "A".repeat(80))
        assertEquals(TbtFrame.STREET_NAME_MAX_BYTES, frame[3].toInt())
        assertEquals(TbtFrame.HEADER_LEN + TbtFrame.STREET_NAME_MAX_BYTES, frame.size)
    }

    @Test
    fun `truncation never splits a utf8 character`() {
        // Each of these is 3 bytes, so a 31-byte cut lands mid-character.
        val name = "路".repeat(20)
        val cut = TbtFrame.truncateUtf8(name, TbtFrame.STREET_NAME_MAX_BYTES)
        assertEquals(0, cut.size % 3)
        assertEquals(30, cut.size)
        // Decodes cleanly: no replacement characters from a split sequence.
        assertEquals("路".repeat(10), String(cut, Charsets.UTF_8))
    }

    @Test
    fun `clear frame is a valid empty directive`() {
        val frame = TbtFrame.clearFrame()
        assertEquals(TbtFrame.HEADER_LEN, frame.size)
        assertEquals(ManeuverParser.Icon.NONE.toByte(), frame[2])
        assertEquals(0, frame[3].toInt())
    }

    @Test
    fun `version 2 puts the roundabout exit at offset 8`() {
        val frame = TbtFrame.encode(ManeuverParser.Icon.ROUNDABOUT, 120, "Ring Road", 3)
        assertEquals(0x54.toByte(), frame[0])
        assertEquals(0x02.toByte(), frame[1])
        assertEquals(3.toByte(), frame[8])
        // The name starts one byte later than it did in version 1.
        assertEquals("Ring Road", String(frame, 9, frame.size - 9, Charsets.UTF_8))
    }

    @Test
    fun `no exit means a zero, not a missing byte`() {
        val frame = TbtFrame.encode(ManeuverParser.Icon.TURN_LEFT, 50, "Main St")
        assertEquals(TbtFrame.HEADER_LEN + "Main St".toByteArray().size, frame.size)
        assertEquals(0.toByte(), frame[8])
    }

    @Test
    fun `an out of range exit is clamped rather than refused`() {
        // The firmware drops the field and keeps the turn; the encoder must
        // not be stricter than the thing it is encoding for.
        assertEquals(0.toByte(), TbtFrame.encode(9, 120, "Ring", 200)[8])
        assertEquals(0.toByte(), TbtFrame.encode(9, 120, "Ring", -1)[8])
        assertEquals(9.toByte(), TbtFrame.encode(9, 120, "Ring", 9)[8])
    }
}
