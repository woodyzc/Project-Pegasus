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
// Deep sleep is off by default, and that is not timidity
// ---------------------------------------------------------------------------
// Waking depends on the FT6336G pulling its interrupt line low when it is
// touched, and on that line still meaning something after the ESP32 has let
// every non-RTC pin go. Neither has ever been tested on this board, and there
// is no BAT button here to fall back on -- the target board has one, this one
// does not (CLAUDE.md section 2).
//
// The failure mode is mild, which is why shipping it behind a switch is
// reasonable rather than reckless: a wake source that does not work leaves a
// device that looks switched off, and a power cycle brings it back. It is not
// a brick. But it is a nasty surprise on a ride, so the rider opts in.
//
// Dimming and blanking have no such risk and are always on.
// ---------------------------------------------------------------------------

// The thresholds, exposed so the settings page can count down to the last one
// rather than hardcode a number that would then drift.
#define POWER_DIM_AFTER_MS 60000u
#define POWER_BLANK_AFTER_MS 180000u
#define POWER_SLEEP_AFTER_MS 300000u

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
