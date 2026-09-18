#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Decodes a position fix sent from the phone over BLE.
//
// ---------------------------------------------------------------------------
// Why the phone sends a fix at all
// ---------------------------------------------------------------------------
// The MAX-M10S has never been fitted (CLAUDE.md §2), and almost everything
// downstream of a position is therefore written, host-tested and never once
// run against a real fix: speed, the odometer, the ride log's whole lifecycle,
// the map, track-up, route snapping, onboard turn-by-turn, ascent, and the
// clock. The phone is already connected, already has location permission, and
// already writes three other characteristics. One more closes that gap without
// waiting for hardware.
//
// It is not only a bring-up tool. A head unit that can borrow the phone's fix
// has a position the moment the app connects, where its own receiver needs a
// cold start -- so this stays whether or not the module ever arrives, and is
// built to the same standard as the rest: a pure decoder with no Arduino and
// no NimBLE, covered by test/host/test_gps_frame.c, byte-matched against the
// app's encoder.
//
// ---------------------------------------------------------------------------
// Wire format (little-endian, one BLE write == one fix)
// ---------------------------------------------------------------------------
//
//   off  size  field
//   0    1     magic       0x47 ('G')
//   1    1     version     0x01
//   2    1     flags       bit0 fix_valid, bit1 time_valid; others reserved 0
//   3    1     num_sv      satellites in the solution, 0 when unknown
//   4    4     lat         int32, degrees * 1e7   (same scale as RouteParse)
//   8    4     lon         int32, degrees * 1e7
//   12   2     speed_cms   uint16, centimetres per second
//   14   2     alt_m       int16, metres above mean sea level
//   16   2     heading_cd  uint16, degrees * 100, 0..35999
//   18   2     year        uint16
//   20   1     month       1..12
//   21   1     day         1..31
//   22   1     hour        0..23
//   23   1     minute      0..59
//   24   1     second      0..60   (60 permitted: a leap second is real)
//
// Total 25 bytes, fixed. No optional tail, unlike the turn frame -- every
// field here is always present, and a fixed length means a short write is
// unambiguously a truncated one.
//
// Scales chosen so nothing a bicycle does can overflow and nothing it needs is
// lost: 1e7 degrees is ~1cm, a uint16 of cm/s reaches 655 m/s, and hundredths
// of a degree of heading are finer than any consumer-grade receiver reports.

#ifdef __cplusplus
extern "C" {
#endif

#define GPS_FRAME_LEN 25
#define GPS_FRAME_MAGIC 0x47
#define GPS_FRAME_VERSION 1

// The decoded fix, in the units DataCenter's GPS_Info_t wants, so the caller
// copies fields across rather than converting them.
typedef struct {
    bool fix_valid;
    bool time_valid;
    uint8_t num_sv;
    double lat;     // degrees
    double lon;     // degrees
    float speed;    // m/s
    float alt;      // metres
    float heading;  // degrees, 0..360
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} GpsFrame_t;

// Returns false and touches nothing on a frame whose magic, version, length or
// any field is out of range.
//
// Rejected whole rather than partially applied, for the same reason the turn
// frame is: a fix assembled from half a frame is a position the phone never
// reported, and every consumer downstream treats a published fix as true.
//
// ⚠️ `fix_valid` false is NOT a rejection. The phone says so while it is still
// acquiring, and the caller needs that to tell "the phone is there but has no
// position" from "the phone is not talking" -- which are different states with
// different fixes.
bool Gps_ParseFrame(const uint8_t *data, size_t length, GpsFrame_t *out);

#ifdef __cplusplus
}
#endif
