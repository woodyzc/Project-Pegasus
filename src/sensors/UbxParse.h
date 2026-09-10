#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Streaming parser for the u-blox UBX binary protocol, scoped to the one
// message this project needs: NAV-PVT (class 0x01, id 0x07), which carries
// position, ground speed, heading and UTC time in a single frame.
//
// Deliberately free of Arduino/ESP dependencies so it can be built and tested
// on the host (see test/host/test_ubx_parse.c) -- the same split BleHrParse.c
// and TbtParse.c use. GPS_Reader.cpp does nothing but pump UART bytes through
// it and publish the results.
//
// Written by hand rather than delegating to deps/SparkFun_u-blox_GNSS: the
// parsing is the part worth testing, and a library that only runs on target
// cannot be tested at all. The SparkFun submodule stays as reference.
//
// Frame layout:
//   B5 62 | class | id | len_lo len_hi | payload[len] | CK_A CK_B
// Checksum is 8-bit Fletcher over class, id, length and payload.
//
// CLAUDE.md §2 requires UBX only, with NMEA disabled -- NMEA is bulky ASCII
// and would cost parse time for data this already provides.

#define UBX_NAV_PVT_CLASS 0x01
#define UBX_NAV_PVT_ID 0x07
#define UBX_NAV_PVT_LEN 92

// Longest payload accepted. NAV-PVT is 92; anything larger is a message we do
// not care about and is skipped without buffering.
#define UBX_MAX_PAYLOAD 92

#ifdef __cplusplus
extern "C" {
#endif

// One decoded NAV-PVT, in usable units rather than the wire's scaled integers.
typedef struct {
    bool fix_valid;    // gnssFixOK set AND fixType is 2D or better
    uint8_t fix_type;  // raw fixType: 0 none, 2 2D, 3 3D, 4 GNSS+DR
    uint8_t num_sv;    // satellites used in the solution

    double lat_deg;
    double lon_deg;
    float alt_m;       // height above mean sea level
    float speed_mps;   // ground speed
    float heading_deg; // heading of motion, 0-360

    bool time_valid;   // date and time both valid and fully resolved
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} UbxNavPvt_t;

// Parser state. Zero-initialise with Ubx_Init() before first use.
typedef struct {
    uint8_t state;
    uint8_t msg_class;
    uint8_t msg_id;
    uint16_t length;
    uint16_t index;
    uint8_t ck_a;
    uint8_t ck_b;
    bool oversized; // payload too large for the buffer; still checksummed, then dropped
    uint8_t payload[UBX_MAX_PAYLOAD];
} UbxParser_t;

void Ubx_Init(UbxParser_t *parser);

// Feeds one byte. Returns true exactly when a NAV-PVT frame has completed with
// a valid checksum, in which case *out is filled. Any other message, or a
// frame that fails its checksum, returns false and resets cleanly -- a
// corrupted frame must never reach the UI as a position.
bool Ubx_Feed(UbxParser_t *parser, uint8_t byte, UbxNavPvt_t *out);

// Decodes a bare 92-byte NAV-PVT payload. Exposed for tests; Ubx_Feed() calls
// it once a frame has passed its checksum.
bool Ubx_DecodeNavPvt(const uint8_t *payload, size_t length, UbxNavPvt_t *out);

#ifdef __cplusplus
}
#endif
