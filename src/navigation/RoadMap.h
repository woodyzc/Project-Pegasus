#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Roads as geometry, not pictures.
//
// The alternative to raster tiles, and what every commercial bike computer
// actually does -- Garmin's .img, Wahoo's OSM data, the Coospo's thin black
// lines are all vector. Four reasons, each of which applies to this board:
//
//   storage   the same area measured 75x smaller than baked tiles
//   rotation  track-up is close to mandatory on a bike; rotating geometry is
//             free, rotating tiles is expensive and ugly
//   zoom      one dataset renders at every scale, instead of a separate tile
//             pyramid per level quadrupling the storage each time
//   routing   rerouting needs the road graph anyway, and once it exists the
//             drawing is nearly free
//
// LVGL 8 has no SVG renderer and never will -- that arrived in LVGL 9 -- but
// SVG was never the point. What is needed is the coordinates, and this panel
// already draws coordinates: MapView renders the GPX trail with the same
// projection these roads use.
//
// ---------------------------------------------------------------------------
// FILE FORMAT  (little-endian throughout, .prd on the card)
// ---------------------------------------------------------------------------
//   magic    4   "PRD1"
//   ways     u32
//   bounds   i32 x4   min_lat, min_lon, max_lat, max_lon, all 1e7 degrees
//   then per way:
//     class  u8    ROAD_CLASS_*
//     pad    u8    always 0
//     count  u16   points in this way
//     points i32 pairs, lat then lon, 1e7 degrees
//
// The pad byte is not spare space, it is alignment. A 4-byte way header keeps
// every point array 4-aligned, given the 24-byte file header. Without it the
// points sit at odd offsets and the draw loop has to memcpy every coordinate
// out -- the ESP32 faults on an unaligned 32-bit load from PSRAM, and doing it
// safely per point would cost more than the alignment saves.
//
// Coordinates as int32 at 1e7 degrees: ~1cm resolution worldwide, exact
// arithmetic, and no float in the file. 8 bytes a point is not the tightest
// possible -- delta-encoded int16 would roughly quarter it -- but this is a
// spike, and it is already far below what tiles cost.

#define ROADMAP_MAGIC "PRD1"
#define ROADMAP_COORD_SCALE 1e7

typedef enum {
    ROAD_CLASS_MINOR = 0,     // residential streets
    ROAD_CLASS_SECONDARY = 1, // through roads
    ROAD_CLASS_ARTERY = 2,    // trunk, the brightest thing on the map
    ROAD_CLASS_WATER = 3,     // rivers, drawn underneath everything
    ROAD_CLASS_COUNT = 4,
} RoadClass_t;

typedef struct {
    uint8_t klass;
    uint16_t count;
    const int32_t *points; // interleaved lat, lon -- 2 * count int32s
} RoadWay_t;

#ifdef __cplusplus
extern "C" {
#endif

// Loads a .prd from the card into PSRAM. False if absent or malformed.
// Preconditions: GpxTrack_MountCard() has succeeded.
bool RoadMap_Load(const char *path);

bool RoadMap_IsLoaded();
size_t RoadMap_WayCount();
size_t RoadMap_PointCount();
uint32_t RoadMap_Bytes();

// Way `index`, or false past the end. The points belong to the loaded blob and
// stay valid until the next RoadMap_Load().
bool RoadMap_Way(size_t index, RoadWay_t *out);

// Bounding box of everything loaded, in degrees.
bool RoadMap_Bounds(double *min_lat, double *min_lon, double *max_lat, double *max_lon);

#ifdef __cplusplus
}
#endif
