#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Pure parser for the clock frame the phone writes over BLE, free of any
// NimBLE or Arduino dependency so it builds and tests on the host (see
// test/host/test_clock_frame.c).
//
// It exists because this head unit has no other way to know the time until a
// GNSS module is fitted. Bluetooth defines a Current Time Service for exactly
// this, and it is unusable here: that service needs the PHONE to act as the
// server, and Android does not implement one. So the time rides on the
// service we already own.
//
// Wire format (little-endian, one BLE write == one reading):
//
//   off  size  field
//   0    1     magic       0x43 ('C')
//   1    1     version     0x01
//   2    4     utc_seconds uint32, seconds since the Unix epoch, UTC
//   6    2     offset_min  int16, local time minus UTC, in minutes
//   8    1     zone_len    0..7 bytes of ASCII abbreviation that follow
//   9    n     zone        "EDT", "CEST", ... NOT NUL-terminated on the wire
//
// Total 9..16 bytes.
//
// The offset travels separately from the time rather than the phone simply
// sending local time. A head unit given local time cannot tell what it is
// looking at: it cannot log UTC, cannot compare against a GPS fix, and cannot
// notice that the rider crossed into another zone. Sending UTC plus the offset
// keeps one canonical clock and makes the display a presentation choice.
//
// The zone abbreviation is carried because it cannot be derived. A +60 minute
// offset is BST in London and CET in Paris, and the caption under the clock
// says which -- so the displayed hour is attributable rather than asserted.

#define CLOCK_FRAME_MAGIC 0x43
#define CLOCK_FRAME_VERSION 0x01
#define CLOCK_FRAME_HEADER_LEN 9

// Longest abbreviation carried, excluding the NUL this parser adds. Seven
// covers every IANA abbreviation in use; the longest in common circulation is
// five characters.
#define CLOCK_ZONE_LEN 7

// Offsets outside this are not real places. The widest in use are -12:00 and
// +14:00, and a frame claiming more has been corrupted rather than sent from
// somewhere exotic.
#define CLOCK_OFFSET_MIN_LOWEST (-12 * 60)
#define CLOCK_OFFSET_MIN_HIGHEST (14 * 60)

// Earliest time this firmware will believe: 2024-01-01T00:00:00Z. A phone that
// has not finished booting its own clock reports 1970, and a head unit that
// accepted it would stamp a ride file half a century ago and then refuse to
// take a correction that looked like a smaller number.
#define CLOCK_EPOCH_FLOOR 1704067200u

#ifdef __cplusplus
extern "C" {
#endif

// Decodes one frame. On success writes every output and returns true; on any
// malformation returns false and leaves the outputs untouched.
// `out_zone_size` must be at least CLOCK_ZONE_LEN + 1.
bool Clock_ParseFrame(const uint8_t *data,
                      size_t length,
                      uint32_t *out_utc_seconds,
                      int16_t *out_offset_min,
                      char *out_zone,
                      size_t out_zone_size);

#ifdef __cplusplus
}
#endif
