package com.pegasus.tbt

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * These lock the encoder to the firmware's src/sensors/GpsFrame.h. If one side
 * changes the layout, this test should be what notices.
 *
 * The firmware's own test/host/test_gps_frame.c decodes the same bytes from
 * the other direction; between them the wire format is pinned at both ends.
 */
class GpsFrameTest {

    /** The frame test_gps_frame.c builds, so the two suites agree on one case. */
    private fun reference(): ByteArray = GpsFrame.encode(
        latDegrees = 41.8823,
        lonDegrees = -88.1234,
        speedMetresPerSecond = 6.17f,
        altitudeMetres = 232f,
        headingDegrees = 273.5f,
        numSatellites = 9,
        fixValid = true,
        timeValid = true,
        year = 2026, month = 9, day = 17, hour = 20, minute = 3, second = 45,
    )

    @Test
    fun `known frame matches the firmware layout byte for byte`() {
        val expected = byteArrayOf(
            0x47, 0x01, 0x03, 0x09,
            // lat  418823000 = 0x18F6_BB58 little-endian
            0x58, 0xBB.toByte(), 0xF6.toByte(), 0x18,
            // lon -881234000 = 0xCB79_6FB0 little-endian
            0xB0.toByte(), 0x6F, 0x79, 0xCB.toByte(),
            // speed 617 cm/s
            0x69, 0x02,
            // altitude 232 m
            0xE8.toByte(), 0x00,
            // heading 27350 centidegrees
            0xD6.toByte(), 0x6A,
            // year 2026
            0xEA.toByte(), 0x07,
            9, 17, 20, 3, 45,
        )
        assertArrayEquals(expected, reference())
    }

    @Test
    fun `the frame is always exactly the declared length`() {
        // Fixed, unlike the turn frame. The firmware rejects any other length
        // outright, which is only safe because there is no optional tail.
        assertEquals(GpsFrame.LEN, reference().size)
        val empty = GpsFrame.encode(
            0.0, 0.0, 0f, 0f, 0f, 0, false, false, 0, 0, 0, 0, 0, 0,
        )
        assertEquals(GpsFrame.LEN, empty.size)
    }

    @Test
    fun `flags carry the two states independently`() {
        val fixOnly = GpsFrame.encode(
            1.0, 1.0, 0f, 0f, 0f, 0, fixValid = true, timeValid = false,
            year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0,
        )
        assertEquals(GpsFrame.FLAG_FIX_VALID.toByte(), fixOnly[2])
        // The firmware accepts this and skips its date checks, which is what
        // lets a phone still acquiring send a usable position with a junk date.
        assertEquals(0.toByte(), fixOnly[20])
    }

    @Test
    fun `southern and western coordinates keep their sign`() {
        val f = GpsFrame.encode(
            -33.7, -70.4, 0f, 0f, 0f, 0, true, false, 0, 0, 0, 0, 0, 0,
        )
        // -337000000 and -704000000 both set the top bit; the firmware reads
        // them back through uint32_t for exactly that reason.
        assertEquals(0xEB.toByte(), f[7])
        assertEquals(0xD6.toByte(), f[11])
    }

    @Test
    fun `heading wraps rather than clamping`() {
        // 360 is 0, and encoding it as 35999 would point the map a hundredth
        // of a degree west of north for no reason. The firmware rejects 36000.
        val north = GpsFrame.encode(
            0.0, 0.0, 0f, 0f, 360f, 0, true, false, 0, 0, 0, 0, 0, 0,
        )
        assertEquals(0.toByte(), north[16])
        assertEquals(0.toByte(), north[17])

        val wrapped = GpsFrame.encode(
            0.0, 0.0, 0f, 0f, 725f, 0, true, false, 0, 0, 0, 0, 0, 0,
        )
        // 725 - 720 = 5 degrees = 500 centidegrees
        assertEquals(0xF4.toByte(), wrapped[16])
        assertEquals(0x01.toByte(), wrapped[17])
    }

    @Test
    fun `out of range inputs are clamped rather than thrown`() {
        // This runs on every location callback while the rider is moving. A
        // dropped fix is a worse answer than a saturated one.
        val f = GpsFrame.encode(
            latDegrees = 95.0,
            lonDegrees = -200.0,
            speedMetresPerSecond = 10000f,
            altitudeMetres = 90000f,
            headingDegrees = 0f,
            numSatellites = 999,
            fixValid = true, timeValid = false,
            year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0,
        )
        assertEquals(GpsFrame.LEN, f.size)
        assertEquals(0xFF.toByte(), f[3])
        // 90.0 exactly, which the firmware accepts as a real place.
        assertEquals(0x00.toByte(), f[4])
        assertEquals(0xE9.toByte(), f[5])
        assertEquals(0xA4.toByte(), f[6])
        assertEquals(0x35.toByte(), f[7])
        // Speed saturates at 65535 cm/s rather than wrapping to something slow.
        assertEquals(0xFF.toByte(), f[12])
        assertEquals(0xFF.toByte(), f[13])
    }

    @Test
    fun `a stationary rider still produces a valid fix`() {
        val f = GpsFrame.encode(
            41.0, -88.0, 0f, 200f, 0f, 7, true, true,
            2026, 9, 17, 12, 0, 0,
        )
        assertTrue((f[2].toInt() and GpsFrame.FLAG_FIX_VALID) != 0)
        assertEquals(0.toByte(), f[12])
        assertEquals(0.toByte(), f[13])
    }
}
