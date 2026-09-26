#include "GpsFrame.h"

#include <string.h>

// Same shape as RouteParse's readers, and for the same reason: the shifts are
// spelled out so the decode does not depend on the host's endianness or on
// alignment, neither of which a BLE payload guarantees.
static uint16_t ReadU16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t ReadU32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t ReadI32(const uint8_t *p) {
    // Via uint32_t: a direct shift into a signed type is implementation
    // defined once the sign bit is set, and every coordinate west of Greenwich
    // or south of the equator sets it.
    return (int32_t)ReadU32(p);
}

static int16_t ReadI16(const uint8_t *p) {
    return (int16_t)ReadU16(p);
}

bool Gps_ParseFrame(const uint8_t *data, size_t length, GpsFrame_t *out) {
    if (data == NULL || out == NULL || length != GPS_FRAME_LEN) {
        return false;
    }
    if (data[0] != GPS_FRAME_MAGIC || data[1] != GPS_FRAME_VERSION) {
        return false;
    }

    const uint8_t flags = data[2];
    // Reserved bits must be zero. A sender that starts using them is a sender
    // this build does not understand, and guessing at the rest of its frame is
    // how a position nobody reported ends up on the panel.
    if ((flags & (uint8_t)~0x03u) != 0) {
        return false;
    }

    GpsFrame_t f;
    memset(&f, 0, sizeof(f));
    f.fix_valid = (flags & 0x01u) != 0;
    f.time_valid = (flags & 0x02u) != 0;
    f.num_sv = data[3];

    const int32_t lat_e7 = ReadI32(data + 4);
    const int32_t lon_e7 = ReadI32(data + 8);
    // Out of range is a corrupt frame, not a strange place. Checked before the
    // divide so the bound is on the integer the wire actually carried.
    if (lat_e7 < -900000000 || lat_e7 > 900000000) {
        return false;
    }
    if (lon_e7 < -1800000000 || lon_e7 > 1800000000) {
        return false;
    }
    f.lat = (double)lat_e7 / 1e7;
    f.lon = (double)lon_e7 / 1e7;

    f.speed = (float)ReadU16(data + 12) / 100.0f;
    f.alt = (float)ReadI16(data + 14);

    const uint16_t heading_cd = ReadU16(data + 16);
    if (heading_cd > 35999) {
        return false;
    }
    f.heading = (float)heading_cd / 100.0f;

    f.year = ReadU16(data + 18);
    f.month = data[20];
    f.day = data[21];
    f.hour = data[22];
    f.minute = data[23];
    f.second = data[24];

    // Only checked when the sender claims the time is good. While it is
    // acquiring, the date fields are whatever the phone had -- often zeroes --
    // and rejecting the whole frame for that would throw away a position that
    // is perfectly usable. The clock consumer already gates on time_valid.
    if (f.time_valid) {
        if (f.month < 1 || f.month > 12 || f.day < 1 || f.day > 31 || f.hour > 23 ||
            f.minute > 59 || f.second > 60) {
            return false;
        }
    }

    *out = f;
    return true;
}
