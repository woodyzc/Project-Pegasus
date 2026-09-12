#include "ClockFrame.h"

#include <string.h>

bool Clock_ParseFrame(const uint8_t *data,
                      size_t length,
                      uint32_t *out_utc_seconds,
                      int16_t *out_offset_min,
                      char *out_zone,
                      size_t out_zone_size) {
    if (data == NULL || out_utc_seconds == NULL || out_offset_min == NULL || out_zone == NULL) {
        return false;
    }
    if (out_zone_size < (size_t)CLOCK_ZONE_LEN + 1) {
        return false;
    }
    if (length < (size_t)CLOCK_FRAME_HEADER_LEN) {
        return false;
    }
    if (data[0] != CLOCK_FRAME_MAGIC || data[1] != CLOCK_FRAME_VERSION) {
        return false;
    }

    {
        const uint8_t zone_len = data[8];
        uint32_t seconds;
        int16_t offset;

        if (zone_len > CLOCK_ZONE_LEN) {
            return false;
        }
        /* The declared length must match what arrived, or the copy below
           would read past the end of the frame. */
        if (length != (size_t)CLOCK_FRAME_HEADER_LEN + (size_t)zone_len) {
            return false;
        }

        /* Byte by byte rather than a cast: little-endian by contract, and an
           unaligned 32-bit read is not free on Xtensa. */
        seconds = (uint32_t)data[2] | ((uint32_t)data[3] << 8) | ((uint32_t)data[4] << 16) |
                  ((uint32_t)data[5] << 24);
        offset = (int16_t)((uint16_t)data[6] | ((uint16_t)data[7] << 8));

        if (seconds < CLOCK_EPOCH_FLOOR) {
            return false;
        }
        if (offset < CLOCK_OFFSET_MIN_LOWEST || offset > CLOCK_OFFSET_MIN_HIGHEST) {
            return false;
        }

        *out_utc_seconds = seconds;
        *out_offset_min = offset;
        memcpy(out_zone, data + CLOCK_FRAME_HEADER_LEN, zone_len);
        out_zone[zone_len] = '\0';
    }
    return true;
}
