#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Local time from a GPS position, with no network and no user setting.
//
// Two halves, and only the first is ours:
//
//   1. Position -> POSIX TZ string. Done here, with a curated region table.
//   2. TZ string -> local time, including daylight saving. Done by newlib's
//      setenv("TZ")/tzset()/localtime_r(), which already implements the POSIX
//      rule syntax ("EST5EDT,M3.2.0,M11.1.0" = EST, EDT, second Sunday in
//      March to first Sunday in November). Hand-rolling DST arithmetic would
//      be a large amount of code that is wrong twice a year.
//
// ---------------------------------------------------------------------------
// Accuracy, stated plainly
// ---------------------------------------------------------------------------
// Real timezone borders are political polygons; this uses rectangles. Inside
// the listed regions the answer is right, including DST. Near a border --
// notably the jagged US zone boundaries, and anywhere in the overlap of two
// large countries -- it can be an hour out. Outside every listed region it
// falls back to solar time from longitude, which has no DST and is wrong by up
// to an hour or two in places whose civil time is deliberately offset from the
// sun (Spain, most of China, all of India).
//
// A full fix means embedding tz boundary data, which is megabytes. For a bike
// computer that spends its life in one metropolitan area, the table is the
// better trade -- but do not mistake it for correct everywhere.
// ---------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

// Returns a POSIX TZ string for the position. Never returns NULL: an unlisted
// position yields a fixed solar offset with no DST.
// `out_approximate`, when non-NULL, is set true for that fallback so the UI can
// mark the time as a guess rather than presenting it as authoritative.
const char *TimeZone_PosixFor(double lat_deg, double lon_deg, bool *out_approximate);

// Civil UTC -> Unix epoch seconds. Pure, so the conversion the firmware feeds
// to localtime_r() is itself testable. Uses the days-from-civil algorithm
// rather than mktime(), which interprets its input as LOCAL time and would
// silently apply the very offset being calculated.
int64_t TimeZone_UtcToEpoch(uint16_t year, uint8_t month, uint8_t day,
                            uint8_t hour, uint8_t minute, uint8_t second);

#ifdef __cplusplus
}
#endif
