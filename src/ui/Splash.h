#pragma once

#include <lvgl.h>

// The boot screen.
//
// Shown as early as there is a panel to show it on, and left up while the rest
// of setup() runs -- mounting the card, loading a road extract, building the
// pages. That work takes seconds on a cold boot, and the alternative to a
// splash is not a faster boot, it is those same seconds spent looking at a
// black screen.
//
// So the two seconds are a FLOOR, not a ceiling. Splash_Dismiss() waits out
// whatever is left of them and then gets out of the way; if setup took longer
// than two seconds it waits for nothing and the splash simply ends when the
// dashboard is ready to replace it. Cutting it off at exactly two seconds
// would mean showing a blank panel for the remainder, which is worse than
// either.
//
// Drawn with lv_refr_now() rather than left to the LVGL task, because that
// task does not exist yet: this runs on the setup thread, before
// LvglTask_Start(), which is the whole point -- nothing else can draw for
// seconds and this can draw immediately.

// The floor, in milliseconds.
#define SPLASH_MIN_MS 2000

// Puts the image on screen and renders it there and then. Call after
// Display_Init() and lv_init(), before anything else builds a page.
void Splash_Show();

// Waits out the rest of SPLASH_MIN_MS and removes it. Call immediately before
// LvglTask_Start(), so the first frame that task draws is the dashboard.
void Splash_Dismiss();

// The converted image, from tools/gensplash.py. 240x320 RGB565, which is the
// panel's own format, so showing it costs a memcpy and no decoding.
extern "C" const lv_img_dsc_t pegasus_splash;
