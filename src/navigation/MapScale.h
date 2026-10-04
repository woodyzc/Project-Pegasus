#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Picking the number on a map's scale bar.
//
// Pure arithmetic and no LVGL: ui/MapView.cpp draws the bar and
// test/host/test_map_scale.c is where the choosing is checked.
//
// ---------------------------------------------------------------------------
// Why the bar is not simply a fixed width
// ---------------------------------------------------------------------------
// A bar of constant pixel length would be labelled with whatever distance it
// happened to span -- "137 m", "41 m" -- and a number nobody can hold in their
// head is a number that does not help. So the DISTANCE is chosen from a round
// sequence and the bar's length follows from it, which is the opposite of the
// obvious arrangement and the reason this is worth its own file.
//
// 1, 2, 5 and their decades, because those are the steps that stay legible at
// a glance and divide sensibly: half of 1km is 500m, half of 500m is 200ish.
//
// ⚠️ Metric only, deliberately, even though the panel has an imperial setting
// elsewhere. A scale bar is read as a comparison against the map, not as a
// measurement -- the rider is judging "is that junction near" and not
// converting -- and mixing 1/2/5 with miles produces 0.3 mi and 800 ft, which
// is worse at every zoom. Revisit only if the owner asks.

// Chooses the largest round distance whose bar fits within `max_px`.
//
// Returns false, and touches nothing, for a non-positive scale or width.
// `out_metres` is the number to print; `out_px` is how long to draw the bar.
bool MapScale_Choose(double metres_per_pixel, int max_px, uint32_t *out_metres, int *out_px);

// Formats a chosen distance for the panel: metres below 1000, kilometres
// above, with no trailing ".0". `out_size` should be at least 8.
bool MapScale_Format(uint32_t metres, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
