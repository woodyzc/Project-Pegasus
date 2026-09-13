#include "GpxTrack.h"

#include <Arduino.h>
#include <Preferences.h>
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

// The last directory scan. Internal RAM rather than PSRAM: 1.5KB is nothing,
// and it is read on every frame the picker is open.
char s_files[GPX_MAX_FILES][GPX_NAME_MAX];
size_t s_file_count = 0;

// True for names the GPX loader will accept. Case-insensitive because cards
// written on a PC routinely carry .GPX, and refusing those would look like a
// broken card reader rather than a naming rule.
bool HasGpxSuffix(const char *name) {
    const size_t len = strlen(name);
    return len > 4 && strcasecmp(name + len - 4, ".gpx") == 0;
}
const char *s_mount_status = "not tried";
int s_bus_width = 0;

// Same trick as BLE_TBT_Receiver: the board has no usable serial (CLAUDE.md
// section 8), so the bring-up trace goes to NVS where esptool can read it back
// over the USB link that flashes it. Bounded, and only ever written at mount.
void NoteSdStep(const char *key, uint8_t value) {
    Preferences prefs;
    if (prefs.begin("pegasus", false)) {
        prefs.putUChar(key, value);
        prefs.end();
    }
}
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
        s_mount_status = "pin setup refused";
        NoteSdStep("sd_pins", 0);
        return false;
    }
    NoteSdStep("sd_pins", 1);

    // format_if_empty stays false throughout -- silently formatting a rider's
    // card would be an unforgivable way to handle a filesystem this code
    // cannot read.
    //
    // Try 4-bit first, then fall back to 1-bit.
    //
    // 4-bit needs D1/D2/D3 actually wired and pulled up, and this board is
    // already known not to match its own documentation (CLAUDE.md section 2 --
    // it is not even the board the spec describes). 1-bit needs only CLK, CMD
    // and D0, so it survives a card slot whose upper data lines are absent,
    // unpulled, or shared with something else. It is roughly four times
    // slower, which matters not at all here: a GPX is read once at boot and
    // the ride log writes a point per second.
    bool four_bit = SD_MMC.begin("/sdcard", false, false);
    NoteSdStep("sd_4bit", four_bit ? 1 : 0);

    if (!four_bit) {
        SD_MMC.end();
        if (!SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN)) {
            s_mount_status = "pin setup refused (1-bit retry)";
            return false;
        }
        const bool one_bit = SD_MMC.begin("/sdcard", true, false);
        NoteSdStep("sd_1bit", one_bit ? 1 : 0);
        if (!one_bit) {
            s_mount_status = "no card, or wrong format (needs FAT32)";
            return false;
        }
        s_bus_width = 1;
    } else {
        s_bus_width = 4;
    }

    const uint8_t type = SD_MMC.cardType();
    NoteSdStep("sd_type", type);
    if (type == CARD_NONE) {
        s_mount_status = "bus came up but no card present";
        SD_MMC.end();
        return false;
    }

    s_mounted = true;
    s_mount_status = "mounted";
    return true;
}

const char *GpxTrack_MountStatus() {
    return s_mount_status;
}

int GpxTrack_BusWidth() {
    return s_bus_width;
}

uint64_t GpxTrack_CardSizeMb() {
    return s_mounted ? (SD_MMC.cardSize() / (1024ULL * 1024ULL)) : 0;
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
        // A large .gpx is read from the route picker's event callback, on the
        // same task that draws. Chunked already; it just never yielded.
        delay(1);
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

size_t GpxTrack_ScanFiles() {
    s_file_count = 0;
    if (!s_mounted) {
        return 0;
    }

    File root = SD_MMC.open("/");
    if (!root || !root.isDirectory()) {
        return 0;
    }

    for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
        if (s_file_count >= GPX_MAX_FILES) {
            break;
        }
        if (entry.isDirectory()) {
            continue;
        }
        const char *name = entry.name();
        if (!HasGpxSuffix(name)) {
            continue;
        }

        // SD_MMC returns names with and without a leading slash depending on
        // the release, and GpxTrack_Load needs an absolute path.
        char path[GPX_NAME_MAX];
        const int written = (name[0] == '/') ? snprintf(path, sizeof(path), "%s", name)
                                             : snprintf(path, sizeof(path), "/%s", name);
        // Skipped rather than truncated. A truncated path cannot be opened, so
        // listing one would be a menu entry that fails when pressed.
        if (written <= 0 || (size_t)written >= sizeof(path)) {
            continue;
        }

        strncpy(s_files[s_file_count], path, GPX_NAME_MAX - 1);
        s_files[s_file_count][GPX_NAME_MAX - 1] = '\0';
        s_file_count++;
    }
    root.close();

    // Sorted, because the filesystem's own order is neither alphabetical nor
    // stable, and a list that reshuffles between visits is one nobody can
    // learn. Insertion sort: the list is at most 24 long and this runs once
    // when the picker opens.
    for (size_t i = 1; i < s_file_count; i++) {
        char key[GPX_NAME_MAX];
        strncpy(key, s_files[i], GPX_NAME_MAX);
        size_t j = i;
        while (j > 0 && strcasecmp(s_files[j - 1], key) > 0) {
            strncpy(s_files[j], s_files[j - 1], GPX_NAME_MAX);
            j--;
        }
        strncpy(s_files[j], key, GPX_NAME_MAX);
    }
    return s_file_count;
}

size_t GpxTrack_FileCount() {
    return s_file_count;
}

const char *GpxTrack_FilePath(size_t index) {
    if (index >= s_file_count) {
        return "";
    }
    return s_files[index];
}

bool GpxTrack_LoadFirstAvailable() {
    // The scan's order, not the filesystem's. "The first one" is now the first
    // alphabetically, which is at least something a rider can predict and
    // rename their way around.
    if (GpxTrack_ScanFiles() == 0) {
        return false;
    }
    return GpxTrack_Load(s_files[0]);
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
