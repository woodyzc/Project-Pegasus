#include "LvglFs.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <lvgl.h>

namespace {

lv_fs_drv_t s_drv;
bool s_ready = false;

// Accumulated across whatever LVGL asked for, not per call: LVGL reads an
// image in several chunks (header first, then the body), and a per-call figure
// would report the last small chunk rather than the cost of the tile.
volatile uint32_t s_read_us = 0;
volatile uint32_t s_read_bytes = 0;
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

    // Publish only on close, so a reader never sees a half-finished total.
    if (s_open_bytes > 0) {
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
