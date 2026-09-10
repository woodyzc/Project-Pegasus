package com.pegasus.tbt

import java.util.UUID

/**
 * The wire contract with the head unit. Must stay byte-for-byte identical to
 * src/navigation/TbtParse.h in the firmware repo -- that file is the
 * authority, this is the encoder for it.
 *
 *   off  size  field
 *   0    1     magic       0x54 ('T')
 *   1    1     version     0x01
 *   2    1     icon_id     0..10
 *   3    1     name_len    0..31 bytes of UTF-8 that follow
 *   4    4     distance_m  uint32, little-endian
 *   8    n     street_name UTF-8, no NUL terminator on the wire
 */
object TbtFrame {

    val SERVICE_UUID: UUID = UUID.fromString("a3c87500-8ed3-4bdf-8a39-a01bebede295")
    val CHARACTERISTIC_UUID: UUID = UUID.fromString("a3c87501-8ed3-4bdf-8a39-a01bebede295")

    /** Advertised name the firmware passes to NimBLEDevice::init(). */
    const val DEVICE_NAME = "pegasus"

    const val MAGIC: Byte = 0x54
    const val VERSION: Byte = 0x01
    const val HEADER_LEN = 8
    const val STREET_NAME_MAX_BYTES = 31

    fun encode(iconId: Int, distanceMetres: Int, streetName: String): ByteArray {
        require(iconId in 0..10) { "icon id out of range: $iconId" }
        require(distanceMetres >= 0) { "negative distance: $distanceMetres" }

        val nameBytes = truncateUtf8(streetName, STREET_NAME_MAX_BYTES)
        val frame = ByteArray(HEADER_LEN + nameBytes.size)

        frame[0] = MAGIC
        frame[1] = VERSION
        frame[2] = iconId.toByte()
        frame[3] = nameBytes.size.toByte()

        val d = distanceMetres.toLong()
        frame[4] = (d and 0xFF).toByte()
        frame[5] = ((d shr 8) and 0xFF).toByte()
        frame[6] = ((d shr 16) and 0xFF).toByte()
        frame[7] = ((d shr 24) and 0xFF).toByte()

        nameBytes.copyInto(frame, HEADER_LEN)
        return frame
    }

    /** A frame that clears the head unit's route panel. */
    fun clearFrame(): ByteArray = encode(ManeuverParser.Icon.NONE, 0, "")

    /**
     * Cuts UTF-8 to at most [maxBytes] without splitting a character. A split
     * would put an invalid byte sequence on the wire, and the firmware copies
     * the name verbatim -- it has no way to detect or repair that.
     */
    fun truncateUtf8(value: String, maxBytes: Int): ByteArray {
        val encoded = value.toByteArray(Charsets.UTF_8)
        if (encoded.size <= maxBytes) return encoded

        var end = maxBytes
        // Continuation bytes are 10xxxxxx; walk back off any partial sequence.
        while (end > 0 && (encoded[end].toInt() and 0xC0) == 0x80) {
            end--
        }
        return encoded.copyOf(end)
    }
}
