#pragma once

#include <lvgl.h>
#include <stdint.h>

#include "../system/DataCenter.h"

// Turn-by-turn maneuver arrows, as LVGL image assets.
//
// LVGL's built-in symbol font has no maneuver glyphs -- LV_SYMBOL_LEFT and
// friends are chevrons -- so a left turn used to render as a bare "<" and
// every left-ish maneuver (turn, slight, sharp) collapsed onto the same
// character. The turn type had to be carried by the street line instead.
//
// These are real arrows: a shaft that bends and ends in a filled head, one per
// TBT_ICON_* value, so the three left variants are finally distinguishable at
// a glance.
//
// The bitmaps are generated, not hand-written -- see tools/genicons.py, which
// draws them at 4x with Pillow and downsamples for antialiasing. Edit the
// script and regenerate; do not touch TbtIcons.c.
//
// Format is LV_IMG_CF_ALPHA_8BIT: coverage only, no colour, so
// lv_obj_set_style_img_recolor() chooses the colour at runtime and one asset
// serves both the live accent colour and the greyed-out "no route" state.
// Alpha images draw through LVGL's normal mask path and do NOT require
// LV_COLOR_SCREEN_TRANSP -- that is the flag whose absence made lv_canvas
// panic the board (lv_conf.h), and it does not apply here.
//
// Cost: 112x112 bytes each, 11 icons, ~135KB of flash. Worth it here: the
// navigation tile is 240x184 and the arrow is the one thing on it that has to
// be read without looking directly at the screen.

#ifdef __cplusplus
extern "C" {
#endif

#define TBT_ICON_PX 112

// Never returns null. TBT_ICON_NONE and any unknown id fall back to the
// straight-ahead arrow, which the caller is expected to grey out rather than
// hide -- an empty navigation panel looks like a crash.
const lv_img_dsc_t *TbtIcon(uint8_t icon_id);

#ifdef __cplusplus
}
#endif
