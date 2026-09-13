#pragma once

#include <stdint.h>
#include <stddef.h>

#include "TrackBuffer.h"

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

// ---- Why the card did or did not mount ----
// "No SD card" is the same message for a missing card, a card the bus could
// not train, and a filesystem we cannot read. These tell them apart on the
// panel, since serial cannot (CLAUDE.md section 8).
const char *GpxTrack_MountStatus();

// 4, 1, or 0 if not mounted. A 1 means the 4-bit bus failed and the 1-bit
// fallback carried it, which is worth seeing: it says D1/D2/D3 are not wired
// or not pulled up the way the board's documentation claims.
int GpxTrack_BusWidth();

uint64_t GpxTrack_CardSizeMb();

// Streams `path` (e.g. "/route.gpx") through the GPX parser into the point
// store, replacing whatever was loaded before. Returns false if the card is
// not mounted, the file is missing, or it held no usable track points.
bool GpxTrack_Load(const char *path);

// Loads the first .gpx found in the card's root directory, so a rider can drop
// a file on a card without editing any configuration.
bool GpxTrack_LoadFirstAvailable();

// ---- Choosing between several routes ----
//
// Most riders keep more than one file on a card, and until now the head unit
// took whichever the filesystem happened to hand back first -- which is not
// even alphabetical, so "the first one" was unpredictable and unchangeable
// without a computer.

// Longest filename offered, including the leading '/' and the NUL. Names
// longer than this are skipped rather than truncated: a truncated path cannot
// be opened, so offering one would be a menu entry that silently fails.
#define GPX_NAME_MAX 64

// Most files listed. A card with more is not wrong, it is just not all
// reachable from the panel -- and a list longer than this is not something
// anyone scrolls on a bike anyway.
#define GPX_MAX_FILES 24

// Re-reads the card's root directory and returns how many .gpx files are
// there. Call before reading the list back; it is the only thing that touches
// the filesystem.
size_t GpxTrack_ScanFiles();

// How many the last scan found.
size_t GpxTrack_FileCount();

// Full path of one scanned file ("/ride.gpx"), or "" when out of range.
// Valid until the next scan.
const char *GpxTrack_FilePath(size_t index);

// Number of points held after thinning.
size_t GpxTrack_PointCount();

// Reads back one point in degrees.
bool GpxTrack_Point(size_t index, double *out_lat, double *out_lon);

// Centre of the loaded trail's bounding box, for framing the view.
bool GpxTrack_Center(double *out_lat, double *out_lon);

// The underlying store, so callers can use the tested Map_BuildPolyline()
// rather than re-walking the points themselves. Never NULL.
const TrackBuffer_t *GpxTrack_Buffer();

// Bounding box of the loaded trail, for framing the initial view.
bool GpxTrack_Bounds(double *out_min_lat, double *out_max_lat, double *out_min_lon,
                     double *out_max_lon);

// Name of the file currently loaded, or "" if none.
const char *GpxTrack_LoadedName();
