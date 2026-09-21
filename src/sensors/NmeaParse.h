#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Streaming parser for the two NMEA-0183 sentences this project needs out of
// an ATGM336H (中科微电子 GPS+BD): GGA (fix, satellite count, altitude) and
// RMC (speed, heading, date). Unlike the MAX-M10S this board was designed
// around, the ATGM336H is not a u-blox part and does not speak UBX -- it is a
// different chipset that leaves the factory outputting plain NMEA text at
// 9600 baud, and there is no documented way to switch it to a binary
// protocol. See src/sensors/UbxParse.h for the MAX-M10S path; GPS_Reader.cpp
// picks one or the other per board.
//
// Deliberately free of Arduino/ESP dependencies, same split as UbxParse.c and
// BleHrParse.c: see test/host/test_nmea_parse.c.
//
// GGA carries fix quality, satellite count and altitude but no date; RMC
// carries speed, heading and date but no altitude. Neither alone is the full
// picture, so the parser caches the most recent RMC fields and merges them in
// whenever a GGA completes -- GGA is what triggers a publish, the same way
// NAV-PVT does for the UBX path, because it is the sentence with the fields
// nothing else here provides.
//
// Sentence layout: $ttSSS,field,field,...,field*CC\r\n
//   tt  = talker ID (GP, GN, GB, GL, GA, ... -- varies with constellation mix
//         and is ignored; only the 3-character sentence id after it matters)
//   SSS = sentence id ("GGA", "RMC")
//   CC  = two hex digits, XOR of every byte between '$' and '*'
//
// A line with a bad or missing checksum is dropped whole, same discipline as
// a bad UBX checksum: a corrupted line must never reach the UI as a position.

#define NMEA_MAX_LINE 96

#ifdef __cplusplus
extern "C" {
#endif

// One merged fix, in usable units. Mirrors UbxNavPvt_t's shape so GPS_Reader
// can fill a GPS_Info_t from either the same way.
typedef struct {
    bool fix_valid;      // GGA fix quality != 0 (field 6)
    uint8_t fix_quality; // raw GGA fix quality: 0 none, 1 GPS, 2 DGPS, 4 RTK fixed, 5 RTK float, 6 estimated, ...
    uint8_t num_sv;      // satellites used (GGA field 7)

    double lat_deg;
    double lon_deg;
    float alt_m;         // GGA altitude above mean sea level (field 9)

    // From the most recently completed RMC, cached across GGA sentences.
    // heading_valid is false whenever RMC leaves course-over-ground blank,
    // which a stationary receiver does -- 0.0 would silently claim "due
    // north" instead.
    float speed_mps;
    float heading_deg;
    bool heading_valid;

    bool time_valid; // RMC status field is 'A' (data valid)
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} NmeaFix_t;

// Parser state, including the RMC cache described above. Zero-initialise
// with Nmea_Init() before first use.
typedef struct {
    char line[NMEA_MAX_LINE];
    uint8_t len;
    bool in_line;
    bool overflowed;

    bool have_rmc;
    float rmc_speed_mps;
    float rmc_heading_deg;
    bool rmc_heading_valid;
    bool rmc_time_valid;
    uint16_t rmc_year;
    uint8_t rmc_month;
    uint8_t rmc_day;
    uint8_t rmc_hour;
    uint8_t rmc_minute;
    uint8_t rmc_second;
} NmeaParser_t;

void Nmea_Init(NmeaParser_t *parser);

// Feeds one byte. Returns true exactly when a GGA sentence has completed with
// a valid checksum and decoded successfully, in which case *out is filled
// (RMC-derived fields from whatever RMC was most recently seen, or zeroed /
// marked invalid if none has been). An RMC completing is cached internally
// and returns false -- it does not trigger a publish on its own, the same way
// a non-NAV-PVT UBX message does not.
bool Nmea_Feed(NmeaParser_t *parser, uint8_t byte, NmeaFix_t *out);

// Exposed for tests below. `sentence` is one full line, "$..." through the
// two checksum hex digits, with no trailing CR/LF; `length` excludes any
// trailing NUL.
bool Nmea_ChecksumOk(const char *sentence, size_t length);

// Decodes a GGA sentence. Fills fix_valid, fix_quality, num_sv, lat_deg,
// lon_deg and alt_m; leaves every other field untouched, so callers merging
// into an already-populated NmeaFix_t (as Nmea_Feed does) keep what was
// there. Does not check the checksum -- Nmea_Feed does that first.
bool Nmea_DecodeGGA(const char *sentence, size_t length, NmeaFix_t *out);

// Decodes an RMC sentence. Fills speed_mps, heading_deg, heading_valid,
// time_valid and the date/time fields; leaves fix/position fields untouched.
bool Nmea_DecodeRMC(const char *sentence, size_t length, NmeaFix_t *out);

#ifdef __cplusplus
}
#endif
