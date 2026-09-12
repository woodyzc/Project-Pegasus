package com.pegasus.tbt

import java.util.UUID

/**
 * The wire contract with the head unit. Must stay byte-for-byte identical to
 * src/navigation/TbtParse.h in the firmware repo -- that file is the
 * authority, this is the encoder for it.
 *
 *   off  size  field
 *   0    1     magic       0x54 ('T')
 *   1    1     version     0x02
 *   2    1     icon_id     0..10
 *   3    1     name_len    0..31 bytes of UTF-8 that follow
 *   4    4     distance_m  uint32, little-endian
 *   8    1     exit_number roundabout exit 1..9, or 0 for none
 *   9    n     street_name UTF-8, no NUL terminator on the wire
 *
 * Version 2 added the exit number. Mapbox states it outright and every
 * roundabout shares one arrow, so it is the only thing that tells them apart.
 * The firmware still accepts version 1 and reports exit 0 for it, so a head
 * unit on older firmware keeps navigating rather than going dark.
 */
object TbtFrame {

    val SERVICE_UUID: UUID = UUID.fromString("a3c87500-8ed3-4bdf-8a39-a01bebede295")
    val CHARACTERISTIC_UUID: UUID = UUID.fromString("a3c87501-8ed3-4bdf-8a39-a01bebede295")

    /** Advertised name the firmware passes to NimBLEDevice::init(). */
    const val DEVICE_NAME = "pegasus"

    const val MAGIC: Byte = 0x54
    const val VERSION: Byte = 0x02
    const val HEADER_LEN = 9
    const val STREET_NAME_MAX_BYTES = 31

    /** Largest exit the firmware will accept; beyond it the field is dropped. */
    const val EXIT_NUMBER_MAX = 9

    fun encode(
        iconId: Int,
        distanceMetres: Int,
        streetName: String,
        exitNumber: Int = 0,
    ): ByteArray {
        require(iconId in 0..10) { "icon id out of range: $iconId" }
        require(distanceMetres >= 0 || distanceMetres == ManeuverParser.DISTANCE_UNKNOWN) {
            "negative distance: $distanceMetres"
        }

        val nameBytes = truncateUtf8(streetName, STREET_NAME_MAX_BYTES)
        val frame = ByteArray(HEADER_LEN + nameBytes.size)

        frame[0] = MAGIC
        frame[1] = VERSION
        frame[2] = iconId.toByte()
        frame[3] = nameBytes.size.toByte()

        // DISTANCE_UNKNOWN is -1 here and TBT_DISTANCE_UNKNOWN is 0xFFFFFFFF
        // on the wire, which is the same 32 bits; masking to 32 bits carries
        // it across without a special case.
        val d = distanceMetres.toLong() and 0xFFFFFFFFL
        frame[4] = (d and 0xFF).toByte()
        frame[5] = ((d shr 8) and 0xFF).toByte()
        frame[6] = ((d shr 16) and 0xFF).toByte()
        frame[7] = ((d shr 24) and 0xFF).toByte()

        // Clamped rather than rejected. An exit outside the range is a
        // decoration this frame can do without, and refusing the frame over it
        // would throw away a real turn -- which is the firmware's rule too.
        frame[8] = if (exitNumber in 1..EXIT_NUMBER_MAX) exitNumber.toByte() else 0

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
