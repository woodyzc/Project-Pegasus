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

/* Font sizes used by Page_Dashboard's layout. Every size referenced from C++
 * must be enabled here: LVGL compiles each lv_font_montserrat_*.c behind an
 * #if on these macros, so a missing one links as
 * "undefined reference to `lv_font_montserrat_NN'". */
#define LV_FONT_MONTSERRAT_10 1 /* captions (TRIP, INCLINE, HEART RATE, ...) */
#define LV_FONT_MONTSERRAT_12 1 /* route direction text */
#define LV_FONT_MONTSERRAT_14 1 /* title, zone badge */
#define LV_FONT_MONTSERRAT_18 1 /* clock / incline / bpm readouts */
/* 24 was the metric value size until the navigation region grew to 60%
 * and the four readouts moved up to 28. Nothing references it now, and
 * LVGL compiles a font whenever its macro is set. */
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_28 1 /* metric readouts, route arrow glyph */
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
