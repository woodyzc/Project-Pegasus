#pragma once

#include <lvgl.h>

// Bring up the ST7789 SPI panel (Waveshare ESP32-S3-Touch-LCD-2.8) and
// register it as an LVGL display driver with a double, PSRAM-backed draw
// buffer. Same 240x320 geometry as the board before it, so nothing above this
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
