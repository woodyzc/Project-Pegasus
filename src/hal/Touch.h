#pragma once

#include <lvgl.h>

// Bring up the CST328 capacitive touch controller on the Waveshare
// ESP32-S3-Touch-LCD-2.8. Call once from setup(), after Display_Init().
//
// ---------------------------------------------------------------------------
// This is not the FT6336G with different pins
// ---------------------------------------------------------------------------
// The Hosyond board's FT6336G answered 8-bit register addresses on the global
// `Wire` bus and packed a 12-bit coordinate into two bytes. The CST328 uses
// 16-bit big-endian register addresses, packs two 12-bit coordinates into
// three bytes, and requires its status register be cleared after every read
// or it stops reporting. None of that survives a pin swap, so the driver is
// rewritten rather than reconfigured. Register map and sequences come from the
// vendor's own driver (WS_ESP32_Touch28/src/Touch_CST328.cpp).
//
// It also sits on its OWN I2C bus -- SDA 1 / SCL 3, driven through `Wire1`.
// The board's other bus (SDA 11 / SCL 10) carries the QMI8658 IMU and the
// PCF85063 RTC and is left to `Wire` for whoever brings those up.
// ---------------------------------------------------------------------------
void Touch_Init();

// True if Touch_Init() got the expected 0xCACA signature back from the
// controller. A false here and a panel that never responds are the same
// symptom, so it is worth surfacing rather than assuming.
bool Touch_ControllerFound();

// LVGL indev read callback: reports the current press state + coordinates,
// already rotated to match the display's orientation. Registered as the
// `read_cb` of an INDEV_TYPE_POINTER input device.
void Touch_Read(lv_indev_drv_t *drv, lv_indev_data_t *data);

// True while a finger is on the panel, asked directly rather than through
// LVGL. For the boot splash, which runs before the LVGL task exists and so has
// no input device to ask.
bool Touch_IsPressed();
