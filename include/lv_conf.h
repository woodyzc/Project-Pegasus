/**
 * @file lv_conf.h
 * Minimal working configuration for LVGL v8.3.x on ESP32-S3 (Arduino framework).
 * Trimmed from lv_conf_template.h to the settings Project Pegasus actually needs;
 * every LV_USE_* / LV_FONT_* not listed here falls back to LVGL's own default
 * (lv_conf_internal.h wraps every option in #ifndef, so omission == LVGL default).
 */

#if 1 /* Set to "1" to enable content, matches upstream template convention */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0   /* TFT_eSPI pushColors() expects native (non-swapped) RGB565; flip to 1 if colors look byte-swapped on real hardware */
#define LV_COLOR_SCREEN_TRANSP 0

/*=========================
   MEMORY SETTINGS
 *=========================*/
/* LVGL's own object/style heap: kept small and in internal RAM (DRAM) --
 * the big allocations are the display draw buffers, which Display_Init()
 * allocates separately, directly from PSRAM via heap_caps_malloc(). */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (64U * 1024U)
#define LV_MEM_ADR 0
#define LV_MEM_BUF_MAX_NUM 16

/*====================
   HAL SETTINGS
 *====================*/
#define LV_DISP_DEF_REFR_PERIOD 30      /* ms */
#define LV_INDEV_DEF_READ_PERIOD 30     /* ms */

#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_DPI_DEF 130

/*=======================
 * FEATURE CONFIGURATION
 *=======================*/
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_LOG 0

/* Widgets used by the Phase-1 dashboard (speed/odometer/clock per the
 * existing agents/lvgl-ui-layout-speed-odometer-clock branch) plus the
 * common set PageManager-style UIs rely on. Everything else defaults on
 * via LVGL's own template defaults unless disabled below. */
#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_BTN 1
#define LV_USE_LABEL 1
#define LV_USE_IMG 1
#define LV_USE_LINE 1
#define LV_USE_METER 1
#define LV_USE_TABLE 1
#define LV_USE_SLIDER 1 /* Page_Settings: brightness */
#define LV_USE_SWITCH 1 /* Page_Settings: units toggle */

/* Off deliberately, and it is a trap worth naming. The zone-bar triangle was
 * briefly an lv_canvas polygon and the board rebooted: a canvas in
 * LV_IMG_CF_TRUE_COLOR_ALPHA needs LV_COLOR_SCREEN_TRANSP (above, 0), and
 * without it the software renderer's alpha-blend paths are compiled out from
 * under the canvas. Turning that on to fix a 15px marker would change blending
 * for every widget on the display. The marker is stacked rectangles instead.
 * Leaving this at 0 makes the mistake a link error rather than a reboot. */
#define LV_USE_CANVAS 0

/* Font sizes used by Page_Dashboard's layout. Every size referenced from C++
 * must be enabled here: LVGL compiles each lv_font_montserrat_*.c behind an
 * #if on these macros, so a missing one links as
 * "undefined reference to `lv_font_montserrat_NN'". */
#define LV_FONT_MONTSERRAT_10 1 /* captions (TRIP, INCLINE, HEART RATE, ...) */
#define LV_FONT_MONTSERRAT_12 1 /* route direction text */
#define LV_FONT_MONTSERRAT_14 1 /* title, zone badge */
#define LV_FONT_MONTSERRAT_18 1 /* clock / incline / bpm readouts */
/* The turn-by-turn road name. Was 14, which left the navigation tile
 * looking empty; 24 is the largest that still fits a typical street name
 * across the panel ("Rockingham Rd" is 197px of 228). */
#define LV_FONT_MONTSERRAT_24 1
/* 28 was the metric value size until the units moved up onto the caption row
 * and freed the width for 34, then 40. Neither is referenced now. */
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_34 0
/* The four metric readouts, and the largest size a 60px cell can hold under a
 * caption. line_height is 44; the 11px caption sits at y=2 and the value 1px
 * off the bottom, which leaves the two boxes 2px apart. 42 (line_height 46)
 * makes them touch. See the width table in Page_Dashboard's MakeValue for the
 * other half of the fit -- the cell is only 120px wide. */
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_48 1 /* primary speed readout */
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* lv_label_set_text_fmt() goes through LVGL's own printf, not the C library's,
 * and that one drops float support unless this is set: "%.2f" came out on the
 * panel as the single character "f". It is used by the trip distance, the
 * speed, the incline and the map scale -- every decimal figure on the device.
 *
 * LVGL's built-in implementation rather than LV_SPRINTF_CUSTOM with newlib's
 * snprintf: whether newlib here was built with float formatting depends on the
 * nano-format setting in the ESP-IDF underneath, which is not ours to
 * guarantee, and this path has no such dependency. */
#define LV_SPRINTF_USE_FLOAT 1

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 1

#define LV_USE_FLEX 1
#define LV_USE_GRID 1

#endif /*LV_CONF_H*/

#endif /*end of "Content enable"*/
