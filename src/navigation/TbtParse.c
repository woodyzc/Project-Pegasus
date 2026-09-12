#include "TbtParse.h"

#include <string.h>

bool TBT_ParseFrame(const uint8_t *data,
                    size_t length,
                    uint8_t *out_icon_id,
                    uint32_t *out_distance_m,
                    char *out_street_name,
                    size_t out_street_size,
                    uint8_t *out_exit_number) {
    size_t header_len;
    uint8_t exit_number = 0;

    if (data == NULL || out_icon_id == NULL || out_distance_m == NULL ||
        out_street_name == NULL) {
        return false;
    }
    if (out_street_size < (size_t)TBT_STREET_NAME_LEN + 1) {
        return false;
    }
    if (length < (size_t)TBT_FRAME_HEADER_LEN_V1) {
        return false;
    }
    if (data[0] != TBT_FRAME_MAGIC) {
        return false;
    }

    /* Both versions are accepted. The phone and the head unit are flashed
       separately, so a pair that disagrees by one version should lose the
       exit number and keep navigating, not go dark. */
    if (data[1] == TBT_FRAME_VERSION) {
        header_len = (size_t)TBT_FRAME_HEADER_LEN_V2;
    } else if (data[1] == TBT_FRAME_VERSION_LEGACY) {
        header_len = (size_t)TBT_FRAME_HEADER_LEN_V1;
    } else {
        return false;
    }
    if (length < header_len) {
        return false;
    }

    {
        const uint8_t icon = data[2];
        const uint8_t name_len = data[3];
        uint32_t distance;

        if (icon > TBT_ICON_MAX_ID) {
            return false;
        }
        if (name_len > TBT_STREET_NAME_LEN) {
            return false;
        }
        /* The declared name length must match what actually arrived: a frame
           claiming more than it carries would otherwise read past the end. */
        if (length != header_len + (size_t)name_len) {
            return false;
        }

        if (header_len == (size_t)TBT_FRAME_HEADER_LEN_V2) {
            exit_number = data[8];
            /* A number past the maximum is a decoding error, not a junction.
               Rejecting the whole frame would throw away a usable turn over a
               field that is decoration, so the exit alone is discarded. */
            if (exit_number > TBT_EXIT_NUMBER_MAX) {
                exit_number = 0;
            }
        }

        /* Assembled byte by byte rather than cast: the payload is
           little-endian by contract, and an unaligned uint32 read is not free
           on Xtensa. */
        distance = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                   ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);

        *out_icon_id = icon;
        *out_distance_m = distance;
        memcpy(out_street_name, data + header_len, name_len);
        out_street_name[name_len] = '\0';
        if (out_exit_number != NULL) {
            *out_exit_number = exit_number;
        }
    }
    return true;
}
