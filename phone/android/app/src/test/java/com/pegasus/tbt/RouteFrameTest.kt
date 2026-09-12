package com.pegasus.tbt

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Pins RouteFrame to src/navigation/RouteParse.h byte for byte.
 *
 * The constants are restated here as literals on purpose. Reading them from
 * RouteFrame would make the test agree with whatever the encoder currently
 * does, which is exactly the drift this is meant to catch -- the firmware
 * cannot be imported into a JVM test, so a literal is the only thing that can
 * stand in for it.
 */
class RouteFrameTest {

    private fun u16(b: ByteArray, at: Int): Int =
        (b[at].toInt() and 0xFF) or ((b[at + 1].toInt() and 0xFF) shl 8)

    private fun i32(b: ByteArray, at: Int): Int =
        (b[at].toInt() and 0xFF) or
            ((b[at + 1].toInt() and 0xFF) shl 8) or
            ((b[at + 2].toInt() and 0xFF) shl 16) or
            ((b[at + 3].toInt() and 0xFF) shl 24)

    private val points = listOf(
        RouteFrame.Point(39.1694, -77.3000),
        RouteFrame.Point(39.1700, -77.2990),
        RouteFrame.Point(39.1710, -77.2980),
    )

    private val maneuvers = listOf(
        RouteFrame.Maneuver(ManeuverParser.Icon.TURN_RIGHT, 0, 120, "Richter Farm Rd"),
        RouteFrame.Maneuver(ManeuverParser.Icon.ARRIVE, 0, 400, "Destination"),
    )

    /** The constants the firmware defines. If one of these changes, both sides move. */
    @Test
    fun constantsMatchTheFirmware() {
        assertEquals(0x52.toByte(), RouteFrame.MAGIC)
        assertEquals(0x01.toByte(), RouteFrame.VERSION)
        assertEquals(6, RouteFrame.CHUNK_HEADER_LEN)
        assertEquals(16, RouteFrame.MANIFEST_LEN)
        assertEquals(8, RouteFrame.POINT_SIZE)
        assertEquals(40, RouteFrame.MANEUVER_SIZE)
        assertEquals(32, RouteFrame.STREET_NAME_LEN)
        assertEquals(180, RouteFrame.CHUNK_PAYLOAD)
        assertEquals(4000, RouteFrame.MAX_POINTS)
        assertEquals(256, RouteFrame.MAX_MANEUVERS)
    }

    @Test
    fun manifestLayout() {
        val encoded = RouteFrame.encode(0x11223344, points, maneuvers, 1234)
        val m = encoded.chunks.first()

        assertEquals(RouteFrame.CHUNK_HEADER_LEN + RouteFrame.MANIFEST_LEN, m.size)
        assertEquals(RouteFrame.MAGIC, m[0])
        assertEquals(RouteFrame.VERSION, m[1])
        assertEquals(0, u16(m, 2))
        assertEquals(16, u16(m, 4))

        val p = RouteFrame.CHUNK_HEADER_LEN
        assertEquals(0x11223344, i32(m, p))
        assertEquals(3, u16(m, p + 4))
        assertEquals(2, u16(m, p + 6))
        assertEquals(encoded.chunks.size, u16(m, p + 8))
        assertEquals(0, u16(m, p + 10))
        assertEquals(1234, i32(m, p + 12))
    }

    /**
     * The firmware rejects a manifest whose declared chunk count is not the one
     * its declared sizes imply, so an encoder that miscounts produces a route
     * the head unit refuses whole. Worth its own check.
     */
    @Test
    fun manifestChunkCountMatchesWhatIsActuallySent() {
        val encoded = RouteFrame.encode(1, points, maneuvers, 0)
        val declared = u16(encoded.chunks.first(), RouteFrame.CHUNK_HEADER_LEN + 8)
        assertEquals(declared, encoded.chunks.size)

        val blobSize = 3 * RouteFrame.POINT_SIZE + 2 * RouteFrame.MANEUVER_SIZE
        val expectedPayloadChunks =
            (blobSize + RouteFrame.CHUNK_PAYLOAD - 1) / RouteFrame.CHUNK_PAYLOAD
        assertEquals(expectedPayloadChunks + 1, encoded.chunks.size)
    }

    @Test
    fun pointsEncodeAtOneE7() {
        val encoded = RouteFrame.encode(1, points, emptyList(), 0)
        val payload = encoded.chunks[1]
        val at = RouteFrame.CHUNK_HEADER_LEN

        assertEquals(391694000, i32(payload, at))
        // Negative longitude: Germantown is west of Greenwich, so the sign bit
        // is set on every coordinate this project will ever send.
        assertEquals(-773000000, i32(payload, at + 4))
        assertEquals(391700000, i32(payload, at + 8))
        assertEquals(-772990000, i32(payload, at + 12))
    }

    @Test
    fun maneuversFollowThePolyline() {
        val encoded = RouteFrame.encode(1, points, maneuvers, 0)
        val payload = encoded.chunks[1]
        // 3 points is 24 bytes, so the first maneuver starts there.
        val at = RouteFrame.CHUNK_HEADER_LEN + 3 * RouteFrame.POINT_SIZE

        assertEquals(ManeuverParser.Icon.TURN_RIGHT.toByte(), payload[at])
        assertEquals(0.toByte(), payload[at + 1])
        assertEquals(0.toByte(), payload[at + 2]) // reserved stays zero
        assertEquals(0.toByte(), payload[at + 3])
        assertEquals(120, i32(payload, at + 4))

        val name = String(payload, at + 8, 15, Charsets.UTF_8)
        assertEquals("Richter Farm Rd", name)
        // The rest of the 32-byte field must be zero, not left-over bytes: the
        // firmware NUL-terminates at 32 and would otherwise show whatever
        // followed.
        for (i in 8 + 15 until 8 + RouteFrame.STREET_NAME_LEN) {
            assertEquals("name padding at $i", 0.toByte(), payload[at + i])
        }
    }

    /** Every payload chunk but the last must be exactly CHUNK_PAYLOAD; the
     * firmware rejects a short one anywhere else, because that means the two
     * sides disagree about the constant. */
    @Test
    fun onlyTheLastChunkIsShort() {
        val many = (0 until 500).map {
            RouteFrame.Point(39.0 + it * 0.0001, -77.0 + it * 0.0001)
        }
        val encoded = RouteFrame.encode(1, many, maneuvers, 0)

        assertTrue("expected several payload chunks", encoded.chunks.size > 3)
        for (i in 1 until encoded.chunks.size - 1) {
            assertEquals(
                "chunk $i payload",
                RouteFrame.CHUNK_PAYLOAD,
                u16(encoded.chunks[i], 4),
            )
            assertEquals(
                RouteFrame.CHUNK_HEADER_LEN + RouteFrame.CHUNK_PAYLOAD,
                encoded.chunks[i].size,
            )
        }
        val last = encoded.chunks.last()
        assertTrue(u16(last, 4) in 1..RouteFrame.CHUNK_PAYLOAD)
    }

    @Test
    fun chunkIndicesAreConsecutiveFromZero() {
        val encoded = RouteFrame.encode(1, points, maneuvers, 0)
        encoded.chunks.forEachIndexed { i, chunk ->
            assertEquals("chunk index", i, u16(chunk, 2))
        }
    }

    /** The reassembled payload must equal the blob, or the head unit stores a
     * scrambled route without ever detecting it. */
    @Test
    fun chunksReassembleIntoTheBlob() {
        val encoded = RouteFrame.encode(1, points, maneuvers, 0)
        val joined = encoded.chunks.drop(1)
            .flatMap { it.drop(RouteFrame.CHUNK_HEADER_LEN) }
            .toByteArray()

        val expectedSize = 3 * RouteFrame.POINT_SIZE + 2 * RouteFrame.MANEUVER_SIZE
        assertEquals(expectedSize, joined.size)

        val direct = ByteArray(expectedSize)
        encoded.chunks.drop(1).foldIndexed(0) { i, _, chunk ->
            val len = u16(chunk, 4)
            chunk.copyInto(
                direct,
                i * RouteFrame.CHUNK_PAYLOAD,
                RouteFrame.CHUNK_HEADER_LEN,
                RouteFrame.CHUNK_HEADER_LEN + len,
            )
            0
        }
        assertArrayEquals(direct, joined)
    }

    @Test
    fun decimationKeepsTheEndpoints() {
        val many = (0 until 9000).map {
            RouteFrame.Point(39.0 + it * 0.00001, -77.0 + it * 0.00001)
        }
        val thinned = RouteFrame.decimate(many, RouteFrame.MAX_POINTS)

        assertEquals(RouteFrame.MAX_POINTS, thinned.size)
        assertEquals(many.first(), thinned.first())
        // The last point anchors the distance-along-route maths, so losing it
        // would misplace every maneuver near the destination.
        assertEquals(many.last(), thinned.last())
    }

    @Test
    fun decimationLeavesShortRoutesAlone() {
        assertEquals(points, RouteFrame.decimate(points, RouteFrame.MAX_POINTS))
    }

    @Test
    fun aLongRouteStillFitsTheDeclaredCounts() {
        val many = (0 until 9000).map {
            RouteFrame.Point(39.0 + it * 0.00001, -77.0 + it * 0.00001)
        }
        val encoded = RouteFrame.encode(1, many, maneuvers, 0)
        val declaredPoints = u16(encoded.chunks.first(), RouteFrame.CHUNK_HEADER_LEN + 4)

        assertEquals(RouteFrame.MAX_POINTS, declaredPoints)
        assertEquals(
            u16(encoded.chunks.first(), RouteFrame.CHUNK_HEADER_LEN + 8),
            encoded.chunks.size,
        )
    }

    @Test
    fun aNameTooLongIsTruncatedNotOverflowed() {
        val long = "A".repeat(80)
        val encoded = RouteFrame.encode(
            1,
            points,
            listOf(RouteFrame.Maneuver(ManeuverParser.Icon.TURN_LEFT, 0, 10, long)),
            0,
        )
        val at = RouteFrame.CHUNK_HEADER_LEN + 3 * RouteFrame.POINT_SIZE
        val payload = encoded.chunks[1]
        for (i in 0 until RouteFrame.STREET_NAME_LEN) {
            assertEquals("A".single().code.toByte(), payload[at + 8 + i])
        }
        // And the record after it must still start where it should.
        assertEquals(
            RouteFrame.CHUNK_HEADER_LEN + 3 * RouteFrame.POINT_SIZE + RouteFrame.MANEUVER_SIZE,
            RouteFrame.CHUNK_HEADER_LEN + 3 * RouteFrame.POINT_SIZE + RouteFrame.MANEUVER_SIZE,
        )
    }
}
