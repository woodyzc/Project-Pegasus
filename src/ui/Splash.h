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
// So the five seconds are a FLOOR, not a ceiling. Splash_Dismiss() waits out
// whatever is left of them and then gets out of the way; if setup took longer
// it waits for nothing and the splash simply ends when the dashboard is ready
// to replace it. Cutting it off at exactly five seconds would mean showing a
// blank panel for the remainder, which is worse than either.
//
// A touch ends it early. Five seconds is a long time to look at a picture you
// have already seen, and the panel is the only control this board has.
//
// Drawn with lv_refr_now() rather than left to the LVGL task, because that
// task does not exist yet: this runs on the setup thread, before
// LvglTask_Start(), which is the whole point -- nothing else can draw for
// seconds and this can draw immediately.

// The floor, in milliseconds. A touch cuts it short.
#define SPLASH_MIN_MS 5000

// Puts the image on screen and renders it there and then. Call after
// Display_Init() and lv_init(), before anything else builds a page.
void Splash_Show();

// Waits out the rest of SPLASH_MIN_MS, or until the panel is touched, and
// removes it. Call immediately before LvglTask_Start(), so the first frame
// that task draws is the dashboard.
void Splash_Dismiss();

// The converted image, from tools/gensplash.py. 240x320 LV_IMG_CF_INDEXED_8BIT
// -- a 256-entry palette followed by one byte per pixel, which LVGL looks up
// as it draws. Half the flash of true colour (76KB against 150KB) and still no
// decoder and no decode buffer, which is the property that matters: this is
// drawn before the LVGL task exists, at the moment the board has least to
// spare, and a decoder that failed would leave a blank screen with nothing to
// report it.
//
// It is shifted 16px down the frame. The title is drawn hard against the top
// of the artwork, and hard against the top of the artwork is hard against the
// bezel once it is on a panel -- which reads as a crop rather than a margin.
// The cover crop's own slack pays for 9 of those pixels and the flat sky at
// the top is repeated for the rest. The drawn "SYSTEM INITIALIZING" strip
// falls off the bottom as a result, deliberately: it was already being clipped
// and half a line of text reads as a fault, where none reads as a choice.
//
// The original it was made from is assets/pegasus-splash.jpeg, and the command
// is in assets/README.md. Keeping the source art matters here: SplashImage.c
// is a wall of hex bytes, which can be regenerated but cannot sensibly be
// edited.
extern "C" const lv_img_dsc_t pegasus_splash;
