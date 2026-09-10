#pragma once

#include <lvgl.h>

// Bring up the FT6336G capacitive touch controller (I2C) on the Hosyond
// ES3C28P board. Call once from setup(), after Display_Init().
void Touch_Init();

// LVGL indev read callback: reports the current press state + coordinates,
// already rotated to match the display's orientation. Registered as the
// `read_cb` of an INDEV_TYPE_POINTER input device.
void Touch_Read(lv_indev_drv_t *drv, lv_indev_data_t *data);
