#pragma once

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Lets LVGL read files off the SD card.
//
// LVGL addresses files by a drive letter it looks up in its own driver table,
// so `lv_img_set_src(img, "S:/15/8721/12556.bin")` works once this is
// registered. Without it that call silently does nothing -- LVGL finds no
// driver for the letter and returns, leaving an empty image and no error.
//
// This exists for map tiles (see tools/tilegen.py and the offline-map scope):
// the firmware has no PNG decoder, so tiles are converted to LVGL's own raw
// format on a computer and read straight through here.
//
// Preconditions: GpxTrack_MountCard() has succeeded. Registering with no card
// mounted gives a driver whose every open fails, which is survivable but
// pointless.

#ifdef __cplusplus
extern "C" {
#endif

// LVGL's drive letter for the SD card. 'S' for SD; LVGL reserves nothing, it
// just has to be unique across registered drivers and match the path prefix.
#define LVGL_FS_LETTER 'S'

// Registers the driver. Call once, after the card is mounted. Safe to call
// again; the second call is ignored.
void LvglFs_Init();

// True once the driver is registered.
bool LvglFs_IsReady();

// How long the last successful read took, in microseconds, and how many bytes
// it moved. The reason these exist: a 128KB tile is the unit of work for the
// map, and whether this board reads one in 30ms or 300ms decides whether
// tile-backed maps are viable at all. Measuring it was the point of the
// bring-up spike -- see the "Will it keep up?" section of the scope.
uint32_t LvglFs_LastReadUs();
uint32_t LvglFs_LastReadBytes();

#ifdef __cplusplus
}
#endif
