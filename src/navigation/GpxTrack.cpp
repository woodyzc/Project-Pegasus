#include "GpxTrack.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "GpxParse.h"
#include "TrackBuffer.h"

// SDIO pins for this board (see the header). Overridable per board from
// platformio.ini, since a different carrier will wire the card elsewhere.
#ifndef SD_CLK_PIN
#define SD_CLK_PIN 38
#endif
#ifndef SD_CMD_PIN
#define SD_CMD_PIN 40
#endif
#ifndef SD_D0_PIN
#define SD_D0_PIN 39
#endif
#ifndef SD_D1_PIN
#define SD_D1_PIN 41
#endif
#ifndef SD_D2_PIN
#define SD_D2_PIN 47
#endif
#ifndef SD_D3_PIN
#define SD_D3_PIN 48
#endif

namespace {

bool s_mounted = false;
TrackBuffer_t s_track;
int32_t *s_lat_store = nullptr;
int32_t *s_lon_store = nullptr;
char s_loaded_name[64] = {0};

bool EnsureStorage() {
    if (s_lat_store != nullptr && s_lon_store != nullptr) {
        return true;
    }

    // PSRAM deliberately: 160KB of internal SRAM would be a large bite out of
    // the ~320KB the rest of the firmware shares, and this data is read
    // linearly by the renderer, which is what PSRAM is good at.
    const size_t bytes = GPX_MAX_POINTS * sizeof(int32_t);
    s_lat_store = (int32_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    s_lon_store = (int32_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);

    if (s_lat_store == nullptr || s_lon_store == nullptr) {
        heap_caps_free(s_lat_store);
        heap_caps_free(s_lon_store);
        s_lat_store = nullptr;
        s_lon_store = nullptr;
        return false;
    }

    TrackBuffer_Init(&s_track, s_lat_store, s_lon_store, GPX_MAX_POINTS);
    return true;
}

} // namespace

bool GpxTrack_MountCard() {
    if (s_mounted) {
        return true;
    }

    // setPins() before begin(): SD_MMC defaults to the ESP32-S3's standard
    // slot pins, which are not the ones this board uses.
    if (!SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN)) {
        return false;
    }

    // mode1bit = false: all four data lines are wired, so use the 4-bit bus.
    // format_if_empty stays false -- silently formatting a rider's card would
    // be an unforgivable way to handle a filesystem this code cannot read.
    if (!SD_MMC.begin("/sdcard", false, false)) {
        return false;
    }

    s_mounted = (SD_MMC.cardType() != CARD_NONE);
    return s_mounted;
}

bool GpxTrack_CardMounted() {
    return s_mounted;
}

bool GpxTrack_Load(const char *path) {
    if (!s_mounted || path == nullptr || !EnsureStorage()) {
        return false;
    }

    File file = SD_MMC.open(path, FILE_READ);
    if (!file) {
        return false;
    }

    TrackBuffer_Init(&s_track, s_lat_store, s_lon_store, GPX_MAX_POINTS);

    GpxParser_t parser;
    Gpx_Init(&parser);

    // Read in blocks rather than byte at a time: every File::read() call
    // crosses into the SD driver, and a multi-megabyte ride would spend
    // essentially all of its time in that overhead.
    uint8_t chunk[512];
    int read_bytes;
    while ((read_bytes = file.read(chunk, sizeof(chunk))) > 0) {
        for (int i = 0; i < read_bytes; i++) {
            double lat;
            double lon;
            if (Gpx_Feed(&parser, (char)chunk[i], &lat, &lon)) {
                TrackBuffer_Add(&s_track, lat, lon);
            }
        }
    }
    file.close();

    if (s_track.count == 0) {
        s_loaded_name[0] = '\0';
        return false;
    }

    strncpy(s_loaded_name, path, sizeof(s_loaded_name) - 1);
    s_loaded_name[sizeof(s_loaded_name) - 1] = '\0';
    return true;
}

bool GpxTrack_LoadFirstAvailable() {
    if (!s_mounted) {
        return false;
    }

    File root = SD_MMC.open("/");
    if (!root || !root.isDirectory()) {
        return false;
    }

    char found[64] = {0};
    for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
        if (entry.isDirectory()) {
            continue;
        }
        const char *name = entry.name();
        const size_t len = strlen(name);
        // Case-insensitive suffix test: cards written on a PC routinely carry
        // .GPX, and rejecting those would look like a broken card reader.
        if (len > 4 && strcasecmp(name + len - 4, ".gpx") == 0) {
            if (name[0] != '/') {
                snprintf(found, sizeof(found), "/%s", name);
            } else {
                strncpy(found, name, sizeof(found) - 1);
            }
            break;
        }
    }
    root.close();

    if (found[0] == '\0') {
        return false;
    }
    return GpxTrack_Load(found);
}

size_t GpxTrack_PointCount() {
    return s_track.count;
}

bool GpxTrack_Point(size_t index, double *out_lat, double *out_lon) {
    return TrackBuffer_Get(&s_track, index, out_lat, out_lon);
}

bool GpxTrack_Center(double *out_lat, double *out_lon) {
    return TrackBuffer_Center(&s_track, out_lat, out_lon);
}

const TrackBuffer_t *GpxTrack_Buffer() {
    return &s_track;
}

bool GpxTrack_Bounds(double *out_min_lat, double *out_max_lat, double *out_min_lon,
                     double *out_max_lon) {
    if (!s_track.has_bounds || out_min_lat == nullptr || out_max_lat == nullptr ||
        out_min_lon == nullptr || out_max_lon == nullptr) {
        return false;
    }
    *out_min_lat = (double)s_track.min_lat_e7 * 1e-7;
    *out_max_lat = (double)s_track.max_lat_e7 * 1e-7;
    *out_min_lon = (double)s_track.min_lon_e7 * 1e-7;
    *out_max_lon = (double)s_track.max_lon_e7 * 1e-7;
    return true;
}

const char *GpxTrack_LoadedName() {
    return s_loaded_name;
}
