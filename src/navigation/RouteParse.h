#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// The route the phone pushes once at ride start, so the head unit can keep
// navigating after the phone is gone.
//
// This is the offline half of turn-by-turn. TbtParse.h carries the LIVE
// directive -- one maneuver, pushed when it changes, useless the moment the
// link drops. This carries the whole route up front: the polyline to draw and
// snap to, and every maneuver keyed to a distance along that line. With both,
// a dropped connection costs the rider nothing until they go off-route.
//
// Pure decoding only, no NimBLE and no Arduino, so test/host can compile it --
// the same split TbtParse.c and BleHrParse.c already use. The PSRAM buffer and
// the GATT plumbing live in NavRoute.h.
//
// ---------------------------------------------------------------------------
// WHY A BYTE STREAM AND NOT A MESSAGE PER RECORD
// ---------------------------------------------------------------------------
// A route is far larger than one BLE write. The obvious design sends one
// record per write and lets the receiver append, which fails the moment a
// write is lost: the receiver has no idea anything is missing and silently
// assembles a route with a hole in it. Instead every chunk states where its
// payload belongs, so a gap is detectable and the transfer can be reported
// incomplete rather than quietly wrong.
//
// Chunks may therefore arrive in any order and may be retried. The receiver
// tracks which byte ranges have landed and only reports success when the blob
// is whole.
//
// ---------------------------------------------------------------------------
// WIRE FORMAT (little-endian throughout)
// ---------------------------------------------------------------------------
// Every chunk:
//   off  size  field
//   0    1     magic        0x52 ('R')
//   1    1     version      0x01
//   2    2     chunk_index  u16, 0 is the manifest, 1.. are payload
//   4    2     payload_len  u16, bytes of payload that follow
//   6    n     payload
//
// The manifest (chunk_index 0) payload, 16 bytes:
//   0    4     route_id        u32, changes whenever the phone plans a route
//   4    2     point_count     u16, polyline points
//   6    2     maneuver_count  u16
//   8    2     chunk_count     u16, total chunks including this manifest
//   10   2     reserved        u16, zero
//   12   4     total_length_m  u32, route length for a progress readout
//
// Payload chunks (chunk_index >= 1) carry consecutive slices of one blob:
//   [point_count   * ROUTE_POINT_SIZE    bytes]  polyline, then
//   [maneuver_count * ROUTE_MANEUVER_SIZE bytes] maneuvers
//
// A chunk's offset into that blob is implied by its index and the fixed
// payload size ROUTE_CHUNK_PAYLOAD, so no offset field is needed and a chunk
// costs six bytes of header. The final chunk may be short.
//
// Point record, 8 bytes, 4-aligned:
//   0    4     lat  int32, 1e7 degrees
//   4    4     lon  int32, 1e7 degrees
//
// Maneuver record, 40 bytes, 4-aligned:
//   0    1     icon_id                 TBT_ICON_* , same codes as TbtParse
//   1    1     exit_number             roundabout exit, 0 when not applicable
//   2    2     reserved                zero
//   4    4     distance_along_route_m  u32, from the route start
//   8    32    street_name             UTF-8, NUL-padded, not NUL-guaranteed
//
// Coordinates match RoadMap.h at 1e7 degrees: ~1cm, exact integer arithmetic,
// no float on the wire. The records are fixed size and 4-aligned for the same
// reason the .prd format pads its way header -- the ESP32 faults on an
// unaligned 32-bit load out of PSRAM, and the route blob lives in PSRAM.

// Coordinate scale, matching RoadMap.h and the .prd files -- 1e7 degrees is
// ~1cm and keeps every coordinate an exact integer on the wire.
#define ROUTE_COORD_SCALE 1e7

#define ROUTE_CHUNK_MAGIC 0x52
#define ROUTE_CHUNK_VERSION 0x01
#define ROUTE_CHUNK_HEADER_LEN 6
#define ROUTE_MANIFEST_LEN 16

#define ROUTE_POINT_SIZE 8
#define ROUTE_MANEUVER_SIZE 40
#define ROUTE_STREET_NAME_LEN 32

// Payload bytes per non-manifest chunk. Both sides must agree, because the
// receiver derives a chunk's position in the blob from its index alone.
//
// 180 fits inside a 185-byte ATT payload (a 189-byte MTU), which is what a
// phone negotiates in practice once it asks for more than the 23-byte default.
// It is also a multiple of 4, so a chunk boundary never splits a coordinate
// across two writes and the assembled blob stays aligned.
#define ROUTE_CHUNK_PAYLOAD 180

// Ceilings, so a malformed or hostile manifest cannot ask for an unbounded
// allocation. 4,000 points is roughly a 200km route at the 50m spacing Mapbox
// returns for cycling, and 256 maneuvers is far past any real ride.
#define ROUTE_MAX_POINTS 4000
#define ROUTE_MAX_MANEUVERS 256

typedef struct {
    uint32_t route_id;
    uint16_t point_count;
    uint16_t maneuver_count;
    uint16_t chunk_count;
    uint32_t total_length_m;
} RouteManifest_t;

typedef struct {
    int32_t lat; // 1e7 degrees
    int32_t lon;
} RoutePoint_t;

typedef struct {
    uint8_t icon_id;
    uint8_t exit_number;
    uint32_t distance_along_route_m;
    char street_name[ROUTE_STREET_NAME_LEN + 1]; // NUL added by the decoder
} RouteManeuver_t;

#ifdef __cplusplus
extern "C" {
#endif

// Reads a chunk's six-byte header. False if the magic, version or declared
// length disagree with `length`. On success `out_payload` points into `data`.
bool Route_ParseChunkHeader(const uint8_t *data,
                            size_t length,
                            uint16_t *out_chunk_index,
                            const uint8_t **out_payload,
                            uint16_t *out_payload_len);

// Decodes the manifest payload. False if it is the wrong size, declares more
// points or maneuvers than the ceilings above, or declares a chunk count that
// disagrees with the blob size those counts imply.
//
// That last check is what stops a truncated transfer being reported complete:
// the manifest has to be internally consistent before a single byte is stored.
bool Route_ParseManifest(const uint8_t *payload, uint16_t payload_len, RouteManifest_t *out);

// Total payload bytes the manifest implies, i.e. the assembled blob size.
size_t Route_BlobSize(const RouteManifest_t *manifest);

// Chunks needed to carry that blob, including the manifest itself.
uint16_t Route_ChunkCount(const RouteManifest_t *manifest);

// Reads record `index` out of an assembled blob. False past the end. `blob`
// must be the whole assembled payload and `manifest` the one that described
// it. Decoding rather than casting: the blob is a byte array and these
// unpack it field by field, which is also what keeps the host tests
// endian-independent.
bool Route_Point(const uint8_t *blob, const RouteManifest_t *manifest, uint16_t index,
                 RoutePoint_t *out);
bool Route_Maneuver(const uint8_t *blob, const RouteManifest_t *manifest, uint16_t index,
                    RouteManeuver_t *out);

#ifdef __cplusplus
}
#endif
