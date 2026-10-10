#pragma once

#include <lvgl.h>

// Bring up the ST7789 SPI panel (Waveshare ESP32-S3-Touch-LCD-2.8) and
// register it as an LVGL display driver with two 40-line internal-RAM draw
// buffers, flushed by DMA -- or one, flushed by the CPU, if the second cannot
// be had (Display_ModeText says which). Same 240x320 geometry as the board before it, so nothing above this
// layer -- lv_conf.h, the fonts, every page's layout -- changed with the port.
// Must be called once from setup() before any LVGL widget/task code runs.
void Display_Init();

// Backlight level, 0-100 percent. Backed by the same ledc channel
// Backlight_Init() sets up on TFT_BL, so this is safe to call any time after
// Display_Init(). Clamped internally; 0 turns the panel dark but leaves the
// SPI link up.
void Display_SetBrightness(uint8_t percent);

// Last value passed to Display_SetBrightness() (defaults to full brightness).
uint8_t Display_GetBrightness();

// How the panel is being fed -- "DMA, 2 x 40 lines" or the fallback it took and
// why. For the settings page: the fallbacks keep the board booting, so this is
// the only place it shows.
const char *Display_ModeText();

// Counts the frames LVGL has begun rendering. The one boundary between one
// frame's 40-line strips and the next: a draw callback runs once per strip,
// so anything that wants to total a FRAME compares this between calls.
uint32_t Display_FrameSeq();
