#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Formats the GPX a ride is recorded into (CLAUDE.md §4 Task: ride logging).
//
// Pure string building, no SD_MMC and no Arduino, so the file layout can be
// tested on the host the same way GpxParse.c covers reading -- and the two
// suites together check that what this writes, that parses.
//
// Every function returns the number of bytes written, excluding the NUL, and
// returns 0 if the buffer was too small. 0 is never a valid length here, so a
// caller that ignores the result cannot silently emit a truncated tag: the
// worst it can do is write nothing.

#ifdef __cplusplus
extern "C" {
#endif

// Enough for the longest single line this emits (a trkpt with elevation and
// timestamp is about 110 bytes).
#define GPX_WRITE_MAX_LINE 192

// Longest track name accepted; longer names are truncated rather than refused,
// since a name is cosmetic and losing the ride to it would not be.
#define GPX_WRITE_MAX_NAME 63

// The opening declaration through to <trkseg>, ready for points.
size_t GpxWrite_Header(char *out, size_t out_size, const char *track_name);

// One <trkpt>. `has_time` false omits the <time> element, which is valid GPX --
// a fix can be positionally good before the receiver reports time resolved, and
// a wrong timestamp is worse than none.
size_t GpxWrite_Point(char *out,
                      size_t out_size,
                      double lat,
                      double lon,
                      float alt_m,
                      bool has_time,
                      uint16_t year,
                      uint8_t month,
                      uint8_t day,
                      uint8_t hour,
                      uint8_t minute,
                      uint8_t second);

// The closing tags. Written after every flush and seeked back over before the
// next point, so the file on the card is a complete GPX at all times -- see
// RideLog.cpp, where that matters because a bike computer is switched off by
// its battery running out, not by a shutdown.
size_t GpxWrite_Footer(char *out, size_t out_size);

// Escapes the five XML metacharacters. Exposed for testing; the header uses it
// for the track name.
size_t GpxWrite_XmlEscape(char *out, size_t out_size, const char *in);

// Builds "/rides/YYYY-MM-DD_HHMMSS.gpx" from a UTC timestamp. Falls back to
// "/rides/ride-NNNN.gpx" using `sequence` when the receiver has no time yet,
// because a ride that starts underground still has to go somewhere.
size_t GpxWrite_FileName(char *out,
                         size_t out_size,
                         bool has_time,
                         uint16_t year,
                         uint8_t month,
                         uint8_t day,
                         uint8_t hour,
                         uint8_t minute,
                         uint8_t second,
                         unsigned sequence);

#ifdef __cplusplus
}
#endif
