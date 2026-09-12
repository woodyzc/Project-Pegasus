#include "RoadMap.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace {

// The whole file, held in PSRAM. Roads are read once and drawn many times, so
// the alternative -- seeking the card on every frame -- would trade the one
// advantage this approach has over tiles.
uint8_t *s_blob = nullptr;
uint32_t s_bytes = 0;
uint32_t s_ways = 0;
size_t s_points = 0;
int32_t s_bounds[4] = {0, 0, 0, 0};

// Byte offset of each way's header, built once at load. Without it, reaching
// way N means walking N variable-length records, which turns drawing into an
// O(n^2) crawl over the file.
uint32_t *s_offsets = nullptr;

// Per-way bounding boxes, parallel to s_offsets: min_lat, min_lon, max_lat,
// max_lon. Four int32 a way, so a 10,000-way extract costs 160KB of PSRAM --
// cheap against skipping the projection for every way that is not on screen.
int32_t *s_way_bounds = nullptr;

// Header fields are read through memcpy regardless. They are only touched once
// per way at load time, so the cost is nothing, and it keeps the loader honest
// about a file that might not be laid out as promised.
uint32_t Rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

uint16_t Rd16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

void Release() {
    heap_caps_free(s_blob);
    heap_caps_free(s_offsets);
    heap_caps_free(s_way_bounds);
    s_blob = nullptr;
    s_offsets = nullptr;
    s_way_bounds = nullptr;
    s_bytes = 0;
    s_ways = 0;
    s_points = 0;
}

} // namespace

bool RoadMap_Load(const char *path) {
    Release();

    File file = SD_MMC.open(path, FILE_READ);
    if (!file || file.isDirectory()) {
        return false;
    }

    const uint32_t size = file.size();
    if (size < 24) {
        file.close();
        return false;
    }

    s_blob = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (s_blob == nullptr) {
        file.close();
        return false;
    }
    const uint32_t got = file.read(s_blob, size);
    file.close();
    if (got != size) {
        Release();
        return false;
    }
    s_bytes = size;

    if (memcmp(s_blob, ROADMAP_MAGIC, 4) != 0) {
        Release();
        return false;
    }
    s_ways = Rd32(s_blob + 4);
    for (int i = 0; i < 4; i++) {
        s_bounds[i] = (int32_t)Rd32(s_blob + 8 + i * 4);
    }

    s_offsets = (uint32_t *)heap_caps_malloc(s_ways * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_way_bounds = (int32_t *)heap_caps_malloc(s_ways * 4 * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    if (s_offsets == nullptr || s_way_bounds == nullptr) {
        Release();
        return false;
    }

    // Walk once to index, validating as we go. A truncated file must fail here
    // rather than run off the end of the blob on the first draw.
    uint32_t at = 24;
    for (uint32_t i = 0; i < s_ways; i++) {
        if (at + 4 > s_bytes) {
            Release();
            return false;
        }
        s_offsets[i] = at;
        const uint16_t count = Rd16(s_blob + at + 2);
        const uint32_t bytes = 4 + (uint32_t)count * 8;
        if (at + bytes > s_bytes) {
            Release();
            return false;
        }
        // Bounds while the way is already in cache, rather than a second pass.
        int32_t lo_la = INT32_MAX, lo_lo = INT32_MAX;
        int32_t hi_la = INT32_MIN, hi_lo = INT32_MIN;
        const uint8_t *pts = s_blob + at + 4;
        for (uint16_t k = 0; k < count; k++) {
            const int32_t la = (int32_t)Rd32(pts + k * 8);
            const int32_t lo = (int32_t)Rd32(pts + k * 8 + 4);
            if (la < lo_la) lo_la = la;
            if (lo < lo_lo) lo_lo = lo;
            if (la > hi_la) hi_la = la;
            if (lo > hi_lo) hi_lo = lo;
        }
        s_way_bounds[i * 4 + 0] = lo_la;
        s_way_bounds[i * 4 + 1] = lo_lo;
        s_way_bounds[i * 4 + 2] = hi_la;
        s_way_bounds[i * 4 + 3] = hi_lo;

        s_points += count;
        at += bytes;
    }
    return true;
}

bool RoadMap_IsLoaded() {
    return s_blob != nullptr && s_ways > 0;
}

size_t RoadMap_WayCount() {
    return s_ways;
}

size_t RoadMap_PointCount() {
    return s_points;
}

uint32_t RoadMap_Bytes() {
    return s_bytes;
}

bool RoadMap_Way(size_t index, RoadWay_t *out) {
    if (!RoadMap_IsLoaded() || index >= s_ways || out == nullptr) {
        return false;
    }
    const uint8_t *p = s_blob + s_offsets[index];
    out->klass = p[0];
    out->count = Rd16(p + 2);
    // 4-aligned by construction: 24-byte file header, 4-byte way headers, and
    // 8 bytes a point. See the format note in RoadMap.h -- this cast is only
    // safe because of that padding.
    out->points = (const int32_t *)(p + 4);
    out->min_lat = s_way_bounds[index * 4 + 0];
    out->min_lon = s_way_bounds[index * 4 + 1];
    out->max_lat = s_way_bounds[index * 4 + 2];
    out->max_lon = s_way_bounds[index * 4 + 3];
    return true;
}

bool RoadMap_Bounds(double *min_lat, double *min_lon, double *max_lat, double *max_lon) {
    if (!RoadMap_IsLoaded()) {
        return false;
    }
    if (min_lat) *min_lat = s_bounds[0] / ROADMAP_COORD_SCALE;
    if (min_lon) *min_lon = s_bounds[1] / ROADMAP_COORD_SCALE;
    if (max_lat) *max_lat = s_bounds[2] / ROADMAP_COORD_SCALE;
    if (max_lon) *max_lon = s_bounds[3] / ROADMAP_COORD_SCALE;
    return true;
}
