#pragma once

#include <stddef.h>

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

// What the driver has actually seen, for bring-up on a new board.
//
// "Touch does not work" has three quite different causes and they are
// indistinguishable from the outside: the controller never answered at all,
// it answers but reports no contacts, or it reports contacts at coordinates
// that land somewhere other than the finger. On a board whose only input is
// the thing under test, and with no usable serial console (CLAUDE.md §8),
// the only way to tell them apart is to put the counters on the panel.
//
// `reads` counts successful register reads of the touch-count register, so a
// climbing value means the I2C link is alive whatever the panel reports;
// `presses` counts reads that found at least one contact; `last_x`/`last_y`
// are the raw coordinates of the most recent one, before any mapping. Any
// pointer may be null.
void Touch_DebugCounters(uint32_t *reads, uint32_t *presses, uint16_t *last_x,
                         uint16_t *last_y);

// Which part answered, and what its info block said.
//
// `addr` is the 7-bit address that acknowledged -- 0x1A for a CST328 (V1
// board), 0x58 for a CST3530 (V2), or 0 if nothing answered at all. The two
// revisions are outwardly identical apart from a label, so this is the only
// way the firmware can say which one it is talking to. `signature` is the
// 0xCACA the CST328's info block carries; the CST3530 is undocumented and may
// not, so it is reported rather than required. Either pointer may be null.
void Touch_DebugIdentity(uint8_t *addr, uint16_t *signature);

// The first `len` bytes (max 8) of the touch block: `latest` is the most
// recent frame read, `latched` the most recent one that looked like it
// carried a contact. Either pointer may be null.
//
// Ground truth about a frame layout nobody has published. The kernel driver's
// byte offsets were derived for this part's siblings, and applying them here
// produced first phantom contacts and then none at all -- at which point
// reading the actual bytes is cheaper than a third guess.
void Touch_DebugFrame(uint8_t *latest, uint8_t *latched, size_t len);

// Result of the one-shot register-width probe run at init: the ACK code and
// first bytes for a 16-bit register address and for an 8-bit one. `e0` means
// the write was acknowledged; anything else means it was not, which is itself
// the answer.
void Touch_DebugProbe(char *out, size_t len);
