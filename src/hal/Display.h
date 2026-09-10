#pragma once

#include <lvgl.h>

// Bring up the ILI9341 SPI panel (Hosyond ES3C28P board) and register it as
// an LVGL display driver with a double, PSRAM-backed draw buffer.
// Must be called once from setup() before any LVGL widget/task code runs.
void Display_Init();
