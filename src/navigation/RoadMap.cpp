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

// ---- Uniform grid index ----
// 64x64 over the file's bounds. For a 10km extract that is ~160m a cell, so a
// 240m view touches four of them and the scan drops from every way in the file
// to the few dozen actually nearby.
//
// Compressed sparse row: s_cell_start[c]..s_cell_start[c+1] indexes into
// s_cell_ways. One allocation each instead of 4,096 little ones, and each
// cell's ways are contiguous.
constexpr int GRID_N = 64;
uint32_t *s_cell_start = nullptr;
uint32_t *s_cell_ways = nullptr;

// Marks ways already returned by the current query, so one spanning several
// visible cells is not drawn repeatedly. Internal RAM: it is touched randomly
// and is only ~1KB for 8,000 ways.
uint8_t *s_query_seen = nullptr;
uint32_t s_query_seen_bytes = 0;

int GridCol(int32_t lon) {
    const int64_t span = (int64_t)s_bounds[3] - s_bounds[1];
    if (span <= 0) return 0;
    int64_t c = ((int64_t)lon - s_bounds[1]) * GRID_N / span;
    if (c < 0) c = 0;
    if (c >= GRID_N) c = GRID_N - 1;
    return (int)c;
}

int GridRow(int32_t lat) {
    const int64_t span = (int64_t)s_bounds[2] - s_bounds[0];
    if (span <= 0) return 0;
    int64_t r = ((int64_t)lat - s_bounds[0]) * GRID_N / span;
    if (r < 0) r = 0;
    if (r >= GRID_N) r = GRID_N - 1;
    return (int)r;
}

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
    heap_caps_free(s_cell_start);
    heap_caps_free(s_cell_ways);
    heap_caps_free(s_query_seen);
    s_blob = nullptr;
    s_offsets = nullptr;
    s_way_bounds = nullptr;
    s_cell_start = nullptr;
    s_cell_ways = nullptr;
    s_query_seen = nullptr;
    s_query_seen_bytes = 0;
    s_bytes = 0;
    s_ways = 0;
    s_points = 0;
}

// Two passes: count how many ways fall in each cell, prefix-sum into starts,
// then fill. Avoids either growing 4,096 vectors or guessing a per-cell
// capacity, and leaves each cell's ways contiguous.
bool BuildGrid() {
    const uint32_t cells = GRID_N * GRID_N;
    s_cell_start = (uint32_t *)heap_caps_calloc(cells + 1, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (s_cell_start == nullptr) {
        return false;
    }

    for (uint32_t i = 0; i < s_ways; i++) {
        const int c0 = GridCol(s_way_bounds[i * 4 + 1]);
        const int c1 = GridCol(s_way_bounds[i * 4 + 3]);
        const int r0 = GridRow(s_way_bounds[i * 4 + 0]);
        const int r1 = GridRow(s_way_bounds[i * 4 + 2]);
        for (int r = r0; r <= r1; r++) {
            for (int c = c0; c <= c1; c++) {
                s_cell_start[r * GRID_N + c + 1]++;
            }
        }
    }
    for (uint32_t i = 0; i < cells; i++) {
        s_cell_start[i + 1] += s_cell_start[i];
    }

    const uint32_t entries = s_cell_start[cells];
    s_cell_ways = (uint32_t *)heap_caps_malloc(entries * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (s_cell_ways == nullptr) {
        return false;
    }

    // Fill using a moving cursor per cell, then the cursor array IS the next
    // cell's start, so no second copy of the offsets is needed.
    uint32_t *cursor = (uint32_t *)heap_caps_malloc(cells * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (cursor == nullptr) {
        return false;
    }
    memcpy(cursor, s_cell_start, cells * sizeof(uint32_t));

    for (uint32_t i = 0; i < s_ways; i++) {
        const int c0 = GridCol(s_way_bounds[i * 4 + 1]);
        const int c1 = GridCol(s_way_bounds[i * 4 + 3]);
        const int r0 = GridRow(s_way_bounds[i * 4 + 0]);
        const int r1 = GridRow(s_way_bounds[i * 4 + 2]);
        for (int r = r0; r <= r1; r++) {
            for (int c = c0; c <= c1; c++) {
                s_cell_ways[cursor[r * GRID_N + c]++] = i;
            }
        }
    }
    heap_caps_free(cursor);

    s_query_seen_bytes = (s_ways + 7) / 8;
    s_query_seen = (uint8_t *)heap_caps_calloc(s_query_seen_bytes, 1, MALLOC_CAP_INTERNAL);
    if (s_query_seen == nullptr) {
        return false;
    }
    return true;
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

    return BuildGrid();
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

size_t RoadMap_Query(int32_t min_lat, int32_t min_lon, int32_t max_lat, int32_t max_lon,
                     uint32_t *out, size_t max_out) {
    if (!RoadMap_IsLoaded() || out == nullptr || max_out == 0 || s_cell_ways == nullptr) {
        return 0;
    }

    memset(s_query_seen, 0, s_query_seen_bytes);

    const int c0 = GridCol(min_lon);
    const int c1 = GridCol(max_lon);
    const int r0 = GridRow(min_lat);
    const int r1 = GridRow(max_lat);

    size_t found = 0;
    for (int r = r0; r <= r1 && found < max_out; r++) {
        for (int c = c0; c <= c1 && found < max_out; c++) {
            const uint32_t cell = r * GRID_N + c;
            const uint32_t end = s_cell_start[cell + 1];
            for (uint32_t e = s_cell_start[cell]; e < end && found < max_out; e++) {
                const uint32_t w = s_cell_ways[e];

                // A way wider than one cell appears in several, and the view
                // may show more than one of them.
                if (s_query_seen[w >> 3] & (1u << (w & 7))) {
                    continue;
                }
                s_query_seen[w >> 3] |= (uint8_t)(1u << (w & 7));

                // The cell only says "near"; the box still has to be checked.
                if (s_way_bounds[w * 4 + 2] < min_lat || s_way_bounds[w * 4 + 0] > max_lat ||
                    s_way_bounds[w * 4 + 3] < min_lon || s_way_bounds[w * 4 + 1] > max_lon) {
                    continue;
                }
                out[found++] = w;
            }
        }
    }
    return found;
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
