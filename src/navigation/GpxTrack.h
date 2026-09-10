#pragma once

#include <stdint.h>
#include <stddef.h>

// Loads a breadcrumb trail from a .gpx on the SD card (CLAUDE.md §5).
//
// ---------------------------------------------------------------------------
// The card is on SDIO, not SPI
// ---------------------------------------------------------------------------
// This board wires the microSD to the SDIO bus -- CLK 38, CMD 40, DATA
// 39/41/47/48 -- so it needs SD_MMC, NOT the SPI `SD` library that most
// ESP32 examples reach for. Those pins are free of the LCD (10-13, 45, 46),
// the touch panel (15-18) and the battery sense (9).
// ---------------------------------------------------------------------------
//
// NOTE: no SD card has ever been inserted into this project. The GPX parsing
// and the trail thinning are covered by test/host, but mounting, the 4-bit bus
// and the file reading below are unverified against real hardware.

// Point storage lives in PSRAM at 8 bytes per point. 20k points is 160KB --
// nothing against 8MB, and enough that a typical ride is thinned only lightly.
#ifndef GPX_MAX_POINTS
#define GPX_MAX_POINTS 20000
#endif

// Mounts the card. Safe to call when no card is present; returns false, and
// GPX navigation simply reports no trail.
bool GpxTrack_MountCard();

// True once the card is mounted.
bool GpxTrack_CardMounted();

// Streams `path` (e.g. "/route.gpx") through the GPX parser into the point
// store, replacing whatever was loaded before. Returns false if the card is
// not mounted, the file is missing, or it held no usable track points.
bool GpxTrack_Load(const char *path);

// Loads the first .gpx found in the card's root directory, so a rider can drop
// a file on a card without editing any configuration.
bool GpxTrack_LoadFirstAvailable();

// Number of points held after thinning.
size_t GpxTrack_PointCount();

// Reads back one point in degrees.
bool GpxTrack_Point(size_t index, double *out_lat, double *out_lon);

// Centre of the loaded trail's bounding box, for framing the view.
bool GpxTrack_Center(double *out_lat, double *out_lon);

// Name of the file currently loaded, or "" if none.
const char *GpxTrack_LoadedName();
