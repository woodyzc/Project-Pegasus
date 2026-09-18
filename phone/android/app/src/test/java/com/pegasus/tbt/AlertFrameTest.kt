package com.pegasus.tbt

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Pins this encoder to the firmware's decoder.
 *
 * REFERENCE_FRAME is byte-for-byte the same array as the one in the firmware
 * repo's test/host/test_alert_frame.c. If a field moves on either side, one of
 * the two suites fails, which is the entire point of writing it down twice.
 *
 * Note every expected byte is written `.toByte()`. Without it Kotlin resolves
 * assertEquals(Int, Byte) to the (Object, Object) overload, boxes an Integer
 * against a Byte, and the assertion fails for every value including the
 * correct one -- a trap this project has already been caught by once.
 */
class AlertFrameTest {

    private companion object {
        /** A WeChat group that said six things, called 张三. */
        val REFERENCE_FRAME = byteArrayOf(
            0x41.toByte(), // magic 'A'
            0x01.toByte(), // version
            0x02.toByte(), // KIND_CHAT
            0x06.toByte(), // name length, in BYTES
            0x06.toByte(), // count
            0x00.toByte(), // reserved
            0xE5.toByte(), 0xBC.toByte(), 0xA0.toByte(), // 张
            0xE4.toByte(), 0xB8.toByte(), 0x89.toByte(), // 三
        )
    }

    @Test
    fun `encodes the reference frame the firmware test decodes`() {
        val frame = AlertFrame.encode(AlertFrame.KIND_CHAT, 6, "张三")
        assertArrayEquals(REFERENCE_FRAME, frame)
    }

    @Test
    fun `header is fixed and the length counts bytes not characters`() {
        val frame = AlertFrame.encode(AlertFrame.KIND_CALL, 1, "Mum")
        assertEquals(AlertFrame.MAGIC, frame[0])
        assertEquals(AlertFrame.VERSION, frame[1])
        assertEquals(AlertFrame.KIND_CALL.toByte(), frame[2])
        assertEquals(3.toByte(), frame[3])
        assertEquals(1.toByte(), frame[4])
        assertEquals(0.toByte(), frame[5])
        assertEquals(AlertFrame.HEADER_LEN + 3, frame.size)
    }

    @Test
    fun `a three byte character counts as three`() {
        // The distinction that makes the whole frame work: two characters,
        // six bytes, and the firmware refuses any frame whose declared length
        // disagrees with what arrived.
        val frame = AlertFrame.encode(AlertFrame.KIND_CHAT, 1, "张三")
        assertEquals(6.toByte(), frame[3])
        assertEquals(AlertFrame.HEADER_LEN + 6, frame.size)
    }

    @Test
    fun `an empty name is a legal frame`() {
        // A call from a number not in the address book. The head unit still
        // has to ring; it draws a placeholder in place of the name.
        val frame = AlertFrame.encode(AlertFrame.KIND_CALL, 1, "")
        assertEquals(0.toByte(), frame[3])
        assertEquals(AlertFrame.HEADER_LEN, frame.size)
    }

    @Test
    fun `count is clamped into what the firmware will accept`() {
        // Zero would mean an alert standing for no messages, which the
        // firmware refuses outright -- so the rider would lose the alert
        // entirely rather than see a wrong number.
        assertEquals(1.toByte(), AlertFrame.encode(AlertFrame.KIND_SMS, 0, "a")[4])
        assertEquals(1.toByte(), AlertFrame.encode(AlertFrame.KIND_SMS, -5, "a")[4])
        assertEquals(255.toByte(), AlertFrame.encode(AlertFrame.KIND_SMS, 300, "a")[4])
        assertEquals(255.toByte(), AlertFrame.encode(AlertFrame.KIND_SMS, 255, "a")[4])
    }

    @Test
    fun `a long latin name is cut to the maximum`() {
        val frame = AlertFrame.encode(AlertFrame.KIND_SMS, 1, "A".repeat(100))
        assertEquals(AlertFrame.NAME_MAX.toByte(), frame[3])
        assertEquals(AlertFrame.HEADER_LEN + AlertFrame.NAME_MAX, frame.size)
    }

    @Test
    fun `a long chinese name is cut on a character boundary`() {
        // 20 characters is 60 bytes, so this must lose characters. The cut
        // has to land between them: 48 is not a multiple of 3, so a naive
        // copyOf(48) would leave two thirds of a character on the end and the
        // head unit would draw a box there.
        val name = "周".repeat(20)
        val frame = AlertFrame.encode(AlertFrame.KIND_CHAT, 1, name)

        // 48 happens to divide by 3, so this particular name needs no
        // walk-back -- the boundary case where it does is covered by
        // `truncation keeps a mixed name valid` below.
        assertEquals(48.toByte(), frame[3])
        assertEquals(AlertFrame.HEADER_LEN + 48, frame.size)

        // The proof that it is still valid UTF-8: decoding it back gives whole
        // characters and no replacement character.
        val decoded = String(frame, AlertFrame.HEADER_LEN, frame[3].toInt(), Charsets.UTF_8)
        assertEquals(16, decoded.length)
        assertTrue(decoded.all { it == '周' })
    }

    @Test
    fun `a name that exactly fills the maximum is untouched`() {
        val name = "B".repeat(AlertFrame.NAME_MAX)
        val frame = AlertFrame.encode(AlertFrame.KIND_SMS, 1, name)
        assertEquals(AlertFrame.NAME_MAX.toByte(), frame[3])
        assertEquals(name, String(frame, AlertFrame.HEADER_LEN, AlertFrame.NAME_MAX, Charsets.UTF_8))
    }

    @Test
    fun `truncation keeps a mixed name valid`() {
        // Latin then Chinese, arranged so the byte limit falls inside a
        // multi-byte character rather than neatly between two.
        val name = "A".repeat(47) + "周末"
        val bytes = AlertFrame.truncateUtf8(name, AlertFrame.NAME_MAX)
        assertEquals(47, bytes.size)
        assertEquals("A".repeat(47), String(bytes, Charsets.UTF_8))
    }

    @Test
    fun `every kind encodes as its own value`() {
        assertEquals(0.toByte(), AlertFrame.encode(AlertFrame.KIND_CALL, 1, "x")[2])
        assertEquals(1.toByte(), AlertFrame.encode(AlertFrame.KIND_SMS, 1, "x")[2])
        assertEquals(2.toByte(), AlertFrame.encode(AlertFrame.KIND_CHAT, 1, "x")[2])
    }

    @Test(expected = IllegalArgumentException::class)
    fun `an unknown kind is refused here rather than on the wire`() {
        // The firmware rejects the whole frame for an unknown kind, so a bug
        // that sent one would show up as silence on a ride. Failing loudly in
        // a test run is the better place for it.
        AlertFrame.encode(9, 1, "x")
    }
}
