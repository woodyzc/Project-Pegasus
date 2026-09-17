#pragma once

#include <stdint.h>

#include "IdlePolicy.h"

// Power management (CLAUDE.md section 4, Core 0 Task 3): dim, blank, sleep.
//
// Until this existed the board drew full power with the backlight lit until
// the battery was flat, which is the difference between a bike computer and a
// gadget. The spec asks for ~200uA in deep sleep and months of standby.
//
// The decision is IdlePolicy's and is host-tested. This file is the parts that
// need hardware: what counts as activity, the backlight, the clean shutdown,
// and the wake source.
//
// ---------------------------------------------------------------------------
// Deep sleep works, and waking from it works
// ---------------------------------------------------------------------------
// Verified 2026-09-17: the device slept overnight, a touch woke it, and the
// settings page read "Last reset: Deep sleep". That single line proves the
// whole chain -- the FT6336G pulls its interrupt line low when touched, that
// line still means something after the ESP32 has let every non-RTC pin go
// (which is what the gpio_hold_en on TOUCH_RST_PIN in EnterSleep is for, and
// it had only ever been reasoned about), and setup() brings everything back:
// the card remounted, the PSRAM buffers reallocated, the pages rebuilt.
//
// It stays behind a switch anyway, and that is now a choice about development
// rather than about risk: a sleeping board's USB-Serial-JTAG is powered down
// with the rest of the digital domain, so the port disappears and it cannot be
// flashed or have its coredump read until something wakes it. On a bench where
// the board is reflashed twenty times a day that is a nuisance; on a bike it
// is the entire point.
//
// There is no BAT button here to fall back on -- the target board has one,
// this one does not (CLAUDE.md section 2) -- so touch is the only way back.
// That is now a tested statement rather than a hopeful one.
//
// Dimming and blanking carry no such consideration and are always on.
// ---------------------------------------------------------------------------

// The thresholds, exposed so the settings page can count down to the last one
// rather than hardcode a number that would then drift.
#define POWER_DIM_AFTER_MS 60000u
#define POWER_BLANK_AFTER_MS 180000u
#define POWER_SLEEP_AFTER_MS 300000u
// Thirty minutes, against five after a finished ride. A device that has
// recorded nothing has been told nothing, and may be waiting on a rider who is
// still pumping a tyre; disappearing on them is worse than staying awake.
#define POWER_SLEEP_IDLE_MS 1800000u

// Subscribes to GPS and battery, takes the current brightness as the rider's
// setting, and starts the LVGL timer that drives everything else.
//
// Call after Settings_Init(), Display_Init() and DataCenter_Init(), and
// BEFORE LvglTask_Start(). The ordering is not incidental: this runs on an
// LVGL timer rather than from loop() because it creates and deletes a widget
// and changes the backlight, and LVGL here has no lock -- every lv_* call in
// this firmware belongs to the LVGL task. Creating the timer before that task
// starts is what keeps even the creation off a second thread.
void PowerManager_Init();

// Something happened. Called from the touch driver on every press, and from
// anywhere else that should hold the screen on.
void PowerManager_NoteActivity();

// True while the backlight is off, so the touch driver can spend the next
// press on waking up rather than on whatever button is under the finger.
bool PowerManager_ScreenIsOff();

// For the settings page.
PowerStage_t PowerManager_Stage();
uint32_t PowerManager_IdleMs();
const char *PowerManager_StageText();

// The CPU clock right now, in MHz: 240 normally, 80 while the screen is dark.
//
// Exposed because it is otherwise invisible -- a downclock that silently
// failed and one that silently never reverted look identical from outside, and
// the second is the one that would make the device feel broken. Serial cannot
// report it (CLAUDE.md section 8), so the settings page does.
uint32_t PowerManager_CpuMhz();

// How many times the clock has been dropped to 80MHz since boot.
//
// The live reading above cannot confirm the downclock works, because it only
// happens while the screen is dark. This can: if the count has gone up by the
// time the rider wakes the device and opens this page, the switch fired -- and
// the page being readable proves it reverted.
uint32_t PowerManager_DownclockCount();

// What is currently preventing sleep, as a sentence, or "" if nothing is.
const char *PowerManager_InhibitText();

// The sleep threshold that applies right now: the short one after a finished
// ride, the long one when nothing has been recorded. For the settings page's
// countdown, which must not re-derive a rule that lives in IdlePolicy.
uint32_t PowerManager_SleepAfterMs();
