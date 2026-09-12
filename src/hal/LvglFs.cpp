#include "LvglFs.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <string.h>
#include <lvgl.h>

namespace {

lv_fs_drv_t s_drv;
bool s_ready = false;

// Accumulated across whatever LVGL asked for, not per call: LVGL reads an
// image in several chunks (header first, then the body), and a per-call figure
// would report the last small chunk rather than the cost of the tile.
volatile uint32_t s_read_us = 0;
volatile uint32_t s_read_bytes = 0;
volatile uint32_t s_open_failures = 0;
char s_failed_path[64] = {0};
uint32_t s_open_us = 0;
uint32_t s_open_bytes = 0;

// One heap File per open handle. LVGL hands the pointer back on every call and
// never copies it, so it only has to outlive the open/close pair.
void *FsOpen(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode) {
    (void)drv;

    // Read-only on purpose. Nothing in this firmware writes through LVGL, and
    // a writable driver pointed at a rider's card is a way to lose a ride log
    // to a bug somewhere else entirely.
    if (mode != LV_FS_MODE_RD) {
        return nullptr;
    }

    File *file = new File(SD_MMC.open(path, FILE_READ));
    if (file == nullptr) {
        return nullptr;
    }
    if (!*file || file->isDirectory()) {
        delete file;
        s_open_failures++;
        strncpy(s_failed_path, path, sizeof(s_failed_path) - 1);
        s_failed_path[sizeof(s_failed_path) - 1] = '\0';
        return nullptr;
    }

    // A fresh open starts a fresh measurement: the interesting number is what
    // one whole file cost, from first byte to last.
    s_open_us = 0;
    s_open_bytes = 0;
    return file;
}

lv_fs_res_t FsClose(lv_fs_drv_t *drv, void *file_p) {
    (void)drv;
    File *file = (File *)file_p;
    file->close();
    delete file;

    // Publish on close, and only if this beat the previous best. LVGL opens
    // the same file twice for an image -- once for the 4-byte header at
    // set_src, once for the pixels at draw -- and a plain "most recent" would
    // leave whichever happened last on screen.
    if (s_open_bytes > s_read_bytes) {
        s_read_us = s_open_us;
        s_read_bytes = s_open_bytes;
    }
    return LV_FS_RES_OK;
}

lv_fs_res_t FsRead(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br) {
    (void)drv;
    File *file = (File *)file_p;

    const uint32_t started = micros();
    const int got = file->read((uint8_t *)buf, btr);
    s_open_us += micros() - started;

    if (got < 0) {
        *br = 0;
        return LV_FS_RES_UNKNOWN;
    }
    *br = (uint32_t)got;
    s_open_bytes += (uint32_t)got;
    return LV_FS_RES_OK;
}

lv_fs_res_t FsSeek(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence) {
    (void)drv;
    File *file = (File *)file_p;

    uint32_t target = pos;
    if (whence == LV_FS_SEEK_CUR) {
        target = file->position() + pos;
    } else if (whence == LV_FS_SEEK_END) {
        target = file->size() + pos;
    }
    return file->seek(target) ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

lv_fs_res_t FsTell(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p) {
    (void)drv;
    *pos_p = ((File *)file_p)->position();
    return LV_FS_RES_OK;
}

} // namespace

void LvglFs_Init() {
    if (s_ready) {
        return;
    }

    lv_fs_drv_init(&s_drv);
    s_drv.letter = LVGL_FS_LETTER;
    s_drv.open_cb = FsOpen;
    s_drv.close_cb = FsClose;
    s_drv.read_cb = FsRead;
    s_drv.seek_cb = FsSeek;
    s_drv.tell_cb = FsTell;
    // No write_cb or dir_* callbacks: this driver is read-only and LVGL only
    // needs the five above to load an image.
    lv_fs_drv_register(&s_drv);

    s_ready = true;
}

bool LvglFs_IsReady() {
    return s_ready;
}

uint32_t LvglFs_LastReadUs() {
    return s_read_us;
}

uint32_t LvglFs_LastReadBytes() {
    return s_read_bytes;
}

void LvglFs_Probe(const char *path, char *out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return;
    }
    out[0] = '\0';

    // Longest existing prefix, component by component. The first component
    // that is missing is the answer.
    char probe[96] = {0};
    char deepest[96] = "/";
    size_t len = strnlen(path, sizeof(probe) - 1);

    for (size_t i = 1; i <= len; i++) {
        if (path[i] != '/' && path[i] != '\0') {
            continue;
        }
        memcpy(probe, path, i);
        probe[i] = '\0';
        if (!SD_MMC.exists(probe)) {
            break;
        }
        strncpy(deepest, probe, sizeof(deepest) - 1);
        deepest[sizeof(deepest) - 1] = '\0';
    }

    // Then what is inside it, so a near-miss (MAP vs map, 15 vs 15/) is
    // visible rather than inferred.
    char kids[64] = {0};
    File dir = SD_MMC.open(deepest);
    if (dir && dir.isDirectory()) {
        for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
            const char *name = strrchr(e.name(), '/');
            name = (name != nullptr) ? name + 1 : e.name();
            if (strlen(kids) + strlen(name) + 2 >= sizeof(kids)) {
                strncat(kids, "...", sizeof(kids) - strlen(kids) - 1);
                break;
            }
            if (kids[0] != '\0') {
                strncat(kids, " ", sizeof(kids) - strlen(kids) - 1);
            }
            strncat(kids, name, sizeof(kids) - strlen(kids) - 1);
        }
    }
    if (dir) {
        dir.close();
    }

    snprintf(out, out_size, "have %s -> %s", deepest, kids[0] ? kids : "(empty)");
}

uint32_t LvglFs_OpenFailures() {
    return s_open_failures;
}

const char *LvglFs_LastFailedPath() {
    return s_failed_path;
}
