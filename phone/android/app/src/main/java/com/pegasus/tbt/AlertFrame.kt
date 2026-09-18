package com.pegasus.tbt

import java.util.UUID

/**
 * A call, a text or a chat message, on its way to the head unit's banner.
 *
 * Must stay byte-for-byte identical to src/system/AlertFrame.h in the firmware
 * repo -- that file is the authority, this is the encoder for it, and
 * AlertFrameTest pins the pair together.
 *
 *   off  size  field
 *   0    1     magic       0x41 ('A')
 *   1    1     version     0x01
 *   2    1     kind        KIND_CALL / KIND_SMS / KIND_CHAT
 *   3    1     nameLength  0..NAME_MAX bytes of UTF-8 that follow
 *   4    1     count       messages this one alert stands for, 1..255
 *   5    1     reserved    0
 *   6    n     name        UTF-8, NOT NUL-terminated on the wire
 *
 * ---------------------------------------------------------------------------
 * There is no field for the message body, and that is the design
 * ---------------------------------------------------------------------------
 * A rider cannot reply and should not be reading prose in traffic. The only
 * question worth answering at 25km/h is whether this is worth stopping for,
 * and a name answers it where two lines of text do not. Keeping the body off
 * the wire also means the most private part of a notification never leaves the
 * phone, and the whole alert fits one BLE write even in Chinese.
 */
object AlertFrame {

    val CHARACTERISTIC_UUID: UUID = UUID.fromString("a3c87506-8ed3-4bdf-8a39-a01bebede295")

    const val MAGIC: Byte = 0x41
    const val VERSION: Byte = 0x01
    const val HEADER_LEN = 6

    /** Bytes, not characters -- a Chinese name is three bytes per character. */
    const val NAME_MAX = 48

    const val KIND_CALL = 0
    const val KIND_SMS = 1
    const val KIND_CHAT = 2

    /**
     * Encodes one alert. [count] is how many messages were folded into it and
     * is clamped to 1..255; the firmware refuses a zero, since an alert
     * standing for no messages is not something this phone can honestly send.
     */
    fun encode(kind: Int, count: Int, name: String): ByteArray {
        require(kind in KIND_CALL..KIND_CHAT) { "unknown alert kind $kind" }

        val nameBytes = truncateUtf8(name, NAME_MAX)
        val out = ByteArray(HEADER_LEN + nameBytes.size)
        out[0] = MAGIC
        out[1] = VERSION
        out[2] = kind.toByte()
        out[3] = nameBytes.size.toByte()
        out[4] = count.coerceIn(1, 255).toByte()
        out[5] = 0
        nameBytes.copyInto(out, HEADER_LEN)
        return out
    }

    /**
     * UTF-8 bytes of [text], cut to at most [maxBytes] on a character
     * boundary.
     *
     * The boundary is the point. `text.take(48)` counts characters, so a
     * 20-character Chinese group name would produce 60 bytes and the firmware
     * would refuse the whole frame -- the rider would simply never hear about
     * that conversation. Cutting at byte 48 regardless would be worse: it
     * splits a character, and the head unit draws the leftover bytes as a box.
     */
    internal fun truncateUtf8(text: String, maxBytes: Int): ByteArray {
        val bytes = text.toByteArray(Charsets.UTF_8)
        if (bytes.size <= maxBytes) return bytes

        // Walk back off any continuation bytes (0b10xxxxxx) so the cut lands
        // at the start of a character rather than inside one.
        var end = maxBytes
        while (end > 0 && (bytes[end].toInt() and 0xC0) == 0x80) {
            end--
        }
        return bytes.copyOf(end)
    }
}
