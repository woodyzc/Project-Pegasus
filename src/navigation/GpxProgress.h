#pragma once

#include <stddef.h>
#include <stdint.h>

// How far along the loaded GPX the rider has got, kept for the whole boot.
//
// The geometry is TrackProgress.h. This is where it lives, and it used to live
// inside each MapView -- which was the wrong owner twice over:
//
//   * The ROUTE page is destroyed every time the rider leaves it (PageManager
//     drops a popped page) and rebuilt on the next visit, and building a
//     MapView reset its progress. So every visit to the map put the whole
//     route back to magenta, and in TBT mode the route page is the ONLY map.
//   * A view only learns of a fix while its page is on screen. Ten minutes on
//     the dashboard were ten minutes the route page's progress never saw.
//
// So one tracker, fed every valid fix from boot, read by any view that draws
// the route. Reset only when a different track is loaded.
//
// ⚠️ LVGL task only, all of it. The track it reads can be replaced by the
// route picker, which runs on that task, so feeding from the GPS task would
// read a buffer mid-reload. The GPS callback only counts fixes; the work is
// done on the timer created by GpxProgress_Init().

#ifdef __cplusplus
extern "C" {
#endif

// Subscribes to GPS and creates the timer that feeds the tracker. Before
// LvglTask_Start(): it creates an LVGL timer, and LVGL here has no lock.
void GpxProgress_Init();

// Feeds the latest fix if one has arrived since the last call. The timer does
// this on its own; MapView calls it before drawing, so the split it draws is
// never a timer tick behind the fix it is drawing.
void GpxProgress_Service();

// The last track vertex passed, for the track currently loaded. False until
// the rider has been picked up on it.
bool GpxProgress_Mark(size_t *out_mark);

// What the last feed cost, for the settings page. Charged to the fix rather
// than to a frame, so it never shows in an FPS reading.
uint32_t GpxProgress_LastFeedUs();

#ifdef __cplusplus
}
#endif
