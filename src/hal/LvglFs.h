#pragma once

#include <stddef.h>
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
// The LARGEST single-file read since boot, not the most recent.
//
// High-water rather than last, because lv_img_set_src() only reads the 4-byte
// header -- LVGL defers the pixels to draw time. A "most recent" figure
// sampled when a page is built therefore reports 4 bytes, or nothing at all,
// and never the tile. The largest read is always the one that matters.
uint32_t LvglFs_LastReadUs();
uint32_t LvglFs_LastReadBytes();

// How many opens failed, and the path the last failure tried. "Not found" is
// useless without the path it looked for -- that was the whole of the first
// bring-up failure, and the path was wrong by one directory.
uint32_t LvglFs_OpenFailures();
const char *LvglFs_LastFailedPath();

// Walks the components of `path` and reports the deepest one that exists,
// followed by what is actually inside it.
//
// "Not found" plus a path says the firmware looked in the right place; it does
// not say whether the card has /MAP, or /MAP/15, or a differently-cased
// variant of either. Rather than another guess-and-reflash round trip, ask the
// card. Writes a single line suitable for a label.
void LvglFs_Probe(const char *path, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
