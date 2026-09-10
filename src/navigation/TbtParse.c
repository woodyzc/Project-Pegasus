#include "TbtParse.h"

#include <string.h>

bool TBT_ParseFrame(const uint8_t *data,
                    size_t length,
                    uint8_t *out_icon_id,
                    uint32_t *out_distance_m,
                    char *out_street_name,
                    size_t out_street_size) {
    if (data == NULL || out_icon_id == NULL || out_distance_m == NULL ||
        out_street_name == NULL) {
        return false;
    }
    if (out_street_size < (size_t)TBT_STREET_NAME_LEN + 1) {
        return false;
    }
    if (length < (size_t)TBT_FRAME_HEADER_LEN) {
        return false;
    }
    if (data[0] != TBT_FRAME_MAGIC || data[1] != TBT_FRAME_VERSION) {
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
        if (length != (size_t)TBT_FRAME_HEADER_LEN + (size_t)name_len) {
            return false;
        }

        /* Assembled byte by byte rather than cast: the payload is
           little-endian by contract, and an unaligned uint32 read is not free
           on Xtensa. */
        distance = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                   ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);

        *out_icon_id = icon;
        *out_distance_m = distance;
        memcpy(out_street_name, data + TBT_FRAME_HEADER_LEN, name_len);
        out_street_name[name_len] = '\0';
    }
    return true;
}
