package com.pegasus.tbt

import java.util.UUID

/**
 * Encoder for the route download the head unit assembles.
 *
 * The wire contract is src/navigation/RouteParse.h in the firmware repo --
 * that file is the authority and this is the encoder for it, the same
 * arrangement TbtFrame.kt has with TbtParse.h. RouteFrameTest pins the two
 * together; change one side and that test should be what notices.
 *
 * Chunk, every one:
 *   off  size  field
 *   0    1     magic        0x52 ('R')
 *   1    1     version      0x01
 *   2    2     chunkIndex   u16, 0 is the manifest
 *   4    2     payloadLen   u16
 *   6    n     payload
 *
 * Manifest payload (chunk 0), 16 bytes:
 *   0    4     routeId        u32
 *   4    2     pointCount     u16
 *   6    2     maneuverCount  u16
 *   8    2     chunkCount     u16, including the manifest
 *   10   2     reserved       u16, zero
 *   12   4     totalLengthM   u32
 *
 * Payload chunks carry consecutive slices of one blob: the polyline first as
 * 8-byte lat/lon pairs, then the maneuvers as 40-byte records. A chunk's
 * offset is implied by its index, which is why CHUNK_PAYLOAD has to match the
 * firmware exactly -- a mismatch would scatter the route through the head
 * unit's buffer rather than failing outright.
 */
object RouteFrame {

    val ROUTE_CHARACTERISTIC_UUID: UUID =
        UUID.fromString("a3c87502-8ed3-4bdf-8a39-a01bebede295")
    val STATUS_CHARACTERISTIC_UUID: UUID =
        UUID.fromString("a3c87503-8ed3-4bdf-8a39-a01bebede295")

    const val MAGIC: Byte = 0x52
    const val VERSION: Byte = 0x01
    const val CHUNK_HEADER_LEN = 6
    const val MANIFEST_LEN = 16

    const val POINT_SIZE = 8
    const val MANEUVER_SIZE = 40
    const val STREET_NAME_LEN = 32

    /** Must equal ROUTE_CHUNK_PAYLOAD in RouteParse.h. */
    const val CHUNK_PAYLOAD = 180

    /** Must equal ROUTE_MAX_POINTS / ROUTE_MAX_MANEUVERS in RouteParse.h. */
    const val MAX_POINTS = 4000
    const val MAX_MANEUVERS = 256

    /** Coordinates go on the wire as integers at 1e7 degrees. */
    const val COORD_SCALE = 1e7

    data class Point(val lat: Double, val lon: Double)

    data class Maneuver(
        val iconId: Int,
        val exitNumber: Int,
        val distanceAlongRouteM: Int,
        val streetName: String,
    )

    /**
     * A whole route ready to send. [chunks] is in order, manifest first.
     */
    data class Encoded(val routeId: Int, val chunks: List<ByteArray>)

    /**
     * Builds every chunk for a route.
     *
     * Decimates the polyline if it exceeds MAX_POINTS rather than refusing or
     * truncating. Truncating would send a route that ends in the middle of
     * nowhere and looks complete; dropping the whole route would leave a rider
     * who planned a long ride with no offline fallback at all. Thinning loses
     * shape fidelity the head unit's 240px panel cannot render anyway.
     */
    fun encode(
        routeId: Int,
        points: List<Point>,
        maneuvers: List<Maneuver>,
        totalLengthM: Int,
    ): Encoded {
        require(points.size >= 2) { "a route needs at least two points" }
        require(maneuvers.size <= MAX_MANEUVERS) {
            "too many maneuvers: ${maneuvers.size}"
        }

        val thinned = decimate(points, MAX_POINTS)

        val blob = ByteArray(thinned.size * POINT_SIZE + maneuvers.size * MANEUVER_SIZE)
        var at = 0
        for (p in thinned) {
            putI32(blob, at, Math.round(p.lat * COORD_SCALE).toInt())
            putI32(blob, at + 4, Math.round(p.lon * COORD_SCALE).toInt())
            at += POINT_SIZE
        }
        for (m in maneuvers) {
            blob[at] = m.iconId.toByte()
            blob[at + 1] = m.exitNumber.coerceIn(0, 255).toByte()
            // at + 2, at + 3 stay zero: reserved.
            putI32(blob, at + 4, m.distanceAlongRouteM)
            val name = TbtFrame.truncateUtf8(m.streetName, STREET_NAME_LEN)
            name.copyInto(blob, at + 8)
            at += MANEUVER_SIZE
        }

        val payloadChunks = (blob.size + CHUNK_PAYLOAD - 1) / CHUNK_PAYLOAD
        val chunkCount = payloadChunks + 1

        val out = ArrayList<ByteArray>(chunkCount)
        out.add(manifest(routeId, thinned.size, maneuvers.size, chunkCount, totalLengthM))

        for (i in 0 until payloadChunks) {
            val from = i * CHUNK_PAYLOAD
            val len = minOf(CHUNK_PAYLOAD, blob.size - from)
            val chunk = ByteArray(CHUNK_HEADER_LEN + len)
            putChunkHeader(chunk, i + 1, len)
            blob.copyInto(chunk, CHUNK_HEADER_LEN, from, from + len)
            out.add(chunk)
        }
        return Encoded(routeId, out)
    }

    private fun manifest(
        routeId: Int,
        pointCount: Int,
        maneuverCount: Int,
        chunkCount: Int,
        totalLengthM: Int,
    ): ByteArray {
        val chunk = ByteArray(CHUNK_HEADER_LEN + MANIFEST_LEN)
        putChunkHeader(chunk, 0, MANIFEST_LEN)
        val p = CHUNK_HEADER_LEN
        putI32(chunk, p, routeId)
        putU16(chunk, p + 4, pointCount)
        putU16(chunk, p + 6, maneuverCount)
        putU16(chunk, p + 8, chunkCount)
        putU16(chunk, p + 10, 0) // reserved
        putI32(chunk, p + 12, totalLengthM)
        return chunk
    }

    private fun putChunkHeader(buf: ByteArray, index: Int, payloadLen: Int) {
        buf[0] = MAGIC
        buf[1] = VERSION
        putU16(buf, 2, index)
        putU16(buf, 4, payloadLen)
    }

    private fun putU16(buf: ByteArray, at: Int, value: Int) {
        buf[at] = (value and 0xFF).toByte()
        buf[at + 1] = ((value shr 8) and 0xFF).toByte()
    }

    private fun putI32(buf: ByteArray, at: Int, value: Int) {
        buf[at] = (value and 0xFF).toByte()
        buf[at + 1] = ((value shr 8) and 0xFF).toByte()
        buf[at + 2] = ((value shr 16) and 0xFF).toByte()
        buf[at + 3] = ((value shr 24) and 0xFF).toByte()
    }

    /**
     * Keeps the first and last point and evenly samples between them.
     *
     * Deliberately not Douglas-Peucker. That would preserve shape far better
     * for the same point budget, but it is only reached by routes longer than
     * about 200km, where the head unit is drawing the line at a scale where
     * neither algorithm's output is distinguishable. Even sampling is a dozen
     * lines that are obviously correct, and the endpoints -- which anchor the
     * distance-along-route maths -- are exact either way.
     */
    fun decimate(points: List<Point>, max: Int): List<Point> {
        if (points.size <= max) return points

        val out = ArrayList<Point>(max)
        val step = (points.size - 1).toDouble() / (max - 1).toDouble()
        for (i in 0 until max - 1) {
            out.add(points[Math.round(i * step).toInt()])
        }
        out.add(points.last())
        return out
    }
}
