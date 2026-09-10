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
        // 'T', v1, turn right, 7-byte name, 250 m little-endian, "Main St"
        val expected = byteArrayOf(
            0x54, 0x01, 0x03, 0x07,
            0xFA.toByte(), 0x00, 0x00, 0x00,
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
        // which the head unit has no way to recognise as wrong.
        TbtFrame.encode(1, -1, "")
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
}
