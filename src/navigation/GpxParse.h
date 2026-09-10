#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Streaming extractor for track points in a GPX file (CLAUDE.md §5: "Read
// .gpx files from SD card and render breadcrumb trails").
//
// Byte-at-a-time, so a file is never held in memory: a recorded ride is
// routinely several megabytes of XML, and only its coordinates matter. Free of
// Arduino/SD dependencies so it can be tested on the host, like UbxParse.c and
// TbtParse.c.
//
// Deliberately not a real XML parser. GPX puts what we need in the attributes
// of one element:
//
//   <trkpt lat="38.8821" lon="-77.0194"><ele>105</ele></trkpt>
//
// so this scans for <trkpt> and <rtept> and reads lat/lon off them. It ignores
// namespaces, entities, comments and every other element, because a breadcrumb
// trail needs none of them. A file that is not valid XML but does contain
// well-formed track points will still parse, which is the right trade for a
// device that cannot report a parse error to anyone.

// Longest tag body accepted, e.g. `trkpt lat="-77.12345678" lon="..."`.
// Anything longer is skipped rather than truncated, so a malformed file cannot
// yield half a coordinate.
#define GPX_TAG_BUFFER 192

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t state;
    uint16_t index;
    bool overflowed;
    char buffer[GPX_TAG_BUFFER];
} GpxParser_t;

void Gpx_Init(GpxParser_t *parser);

// Feeds one character. Returns true exactly when a track point has completed,
// writing its coordinates. Points missing either attribute, or carrying values
// outside the valid ranges, are rejected -- a breadcrumb drawn to a corrupt
// coordinate would be a line across the map to nowhere.
bool Gpx_Feed(GpxParser_t *parser, char c, double *out_lat, double *out_lon);

// Reads a lat/lon pair out of a tag body such as
// `trkpt lat="38.8" lon="-77.0"`. Exposed for tests; Gpx_Feed() uses it.
bool Gpx_ParseTagAttributes(const char *tag, double *out_lat, double *out_lon);

#ifdef __cplusplus
}
#endif
