#pragma once

#include <stdint.h>

// User-facing device settings, persisted to NVS so they survive a reboot.
// Deliberately separate from DataCenter: DataCenter carries live sensor
// telemetry between cores, whereas these are sticky user preferences with a
// single owner (the UI on Core 1) and no publish/subscribe semantics.

typedef enum {
    SPEED_UNIT_KMH = 0,
    SPEED_UNIT_MPH = 1,
} SpeedUnit_t;

// Loads persisted values from NVS and applies them (brightness reaches the
// panel here). Call once from setup(), after Display_Init().
void Settings_Init();

// Backlight, 0-100 percent. The setter applies immediately and persists.
uint8_t Settings_GetBrightness();
void Settings_SetBrightness(uint8_t percent);

// Speed/distance unit system.
SpeedUnit_t Settings_GetSpeedUnit();
void Settings_SetSpeedUnit(SpeedUnit_t unit);

// Navigation source (CLAUDE.md §5).
typedef enum {
    NAV_MODE_TBT = 0, // BLE turn-by-turn pushed from the phone
    NAV_MODE_GPX = 1, // offline breadcrumb from a .gpx on the SD card
} NavMode_t;

// Navigation is now a free choice. It used to be constrained by the
// heart-rate source, because software ANT+ took the BLE controller as a raw
// modem and left no NimBLE host for the turn-by-turn GATT server. ANT+ is
// gone, heart rate is BLE, and the two no longer compete for the radio.
NavMode_t Settings_GetNavMode();
void Settings_SetNavMode(NavMode_t mode);
const char *Settings_NavModeLabel(NavMode_t mode);

// False for modes whose implementation doesn't exist yet, so the UI can say so
// instead of offering a choice that quietly does nothing.
bool Settings_NavModeIsImplemented(NavMode_t mode);

// ---- Radio bring-up safety net ----
// The nav mode lives in NVS, so a mode that hangs during bring-up would
// survive a reflash and leave the device looping with no reachable UI to undo
// it. main.cpp counts up before touching the radios and clears the count once
// setup() completes; after two consecutive boots that never finished, the mode
// holds the GATT server and its advertisement off for one boot -- which is
// where every hang in section 8 lived, and so cannot repeat it.
//
// Two rather than one, because the heart-rate scan blocks for up to 15
// seconds and a rider who unplugs during it would otherwise have their
// navigation setting moved by an impatient power cycle. A real hang repeats;
// an unplug does not.
//
// This used to guard the heart-rate source, which was the riskier setting
// while ANT+ existed. Turn-by-turn is now the only init-time radio choice
// left, so the net moved rather than went away. Do not remove it when adding
// radio modes (CLAUDE.md section 8).
void Settings_NoteRadioBringUpStart();
void Settings_NoteRadioBringUpOk();

// True when this boot is running with the GATT server and its advertisement
// held off, because the previous two did not finish bringing the radio up.
//
// ⚠️ This is NOT the navigation mode. It used to be -- the watchdog forced
// GPX, which worked only because GPX happened to start no radio. That
// coincidence cost the phone-position feature outright: a rider in GPX mode
// had no server for the phone to write a fix into and nothing advertising for
// it to find, on a head unit with no receiver of its own. "What navigation do
// I show" and "do I touch the radio" are separate questions now.
//
// Lasts one boot. The hangs section 8 records have since been fixed, so this
// guards an unknown future hazard rather than a known present one, and a net
// that permanently disables position is worse than one that retries.
bool Settings_RadiosHeldOff();

// Retained and always false: nothing forces the navigation mode any anymore.
// Settings_RadiosHeldOff() is what the watchdog raises now.
bool Settings_DidNavModeFallBack();

// ---- Deep sleep ----
// Off by default. Dimming and blanking always happen; this is only the last
// step, and PowerManager.h explains why it is opt-in: waking depends on the
// touch controller's interrupt line, which has never been tested on this
// board, and there is no BAT button here to fall back on.
bool Settings_GetSleepEnabled();
void Settings_SetSleepEnabled(bool enabled);

// ---- Map orientation ----
// Track-up turns the map so the direction of travel is at the top, which is
// how a rider matches the screen against the road in front of them. Default
// on: north-up asks for a mental rotation at every junction.
//
// It falls back to north-up on its own whenever there is no usable heading --
// before the first fix, and whenever the rider is slower than walking pace,
// where the receiver reports the direction of its own noise.
bool Settings_GetMapTrackUp();
void Settings_SetMapTrackUp(bool track_up);

// ---- Heart-rate zone personalisation ----
// The resting and maximum rate that HrZone.h needs to place a reading in a
// zone. Defaults are the rider's own measured pair, so the bands agree with
// the phone on a device that has never been configured.
//
// These are clamped rather than merely stored, because every zone boundary
// divides by (max - rest): a pair that inverted, or that closed to nothing,
// would make the whole table undefined. Each setter clamps its OWN value
// against the other and never moves the other one -- unlike the HR-source and
// nav-mode pair above, where one genuinely cannot be honoured without changing
// its partner. Here the rider asked for one number, so the other stays put.
uint8_t Settings_GetHrRestBpm();
uint8_t Settings_GetHrMaxBpm();
void Settings_SetHrRestBpm(uint8_t bpm);
void Settings_SetHrMaxBpm(uint8_t bpm);

// ---- Reset diagnostics ----
// Why the previous run ended, and how many times the device has come up since
// the last real power-on. Serial is unusable on this board (CLAUDE.md section
// 8), so the panel is the only place a reset reason can reach a human, and
// without one a reboot loop is indistinguishable from a hang.
//
// The count is what makes it useful: a single "Panic" after a manual reflash
// says nothing, whereas "Panic, boot 47" says the device is looping and names
// the cause. It resets on a genuine power-on, so unplugging clears it.
const char *Settings_LastResetText();
uint32_t Settings_BootCount();

// True when the previous run ended in a way the firmware should be ashamed of
// -- a panic, a watchdog or a brownout, as opposed to a power-on, a reflash or
// a deliberate restart. Lets the UI show the diagnostic prominently only when
// there is something to report.
bool Settings_LastResetWasAbnormal();

// Unit-aware conversion helpers, so pages never hardcode a unit.
float Settings_SpeedFromKmh(float kmh);
float Settings_DistanceFromKm(float km);
const char *Settings_SpeedUnitLabel();    // "km/h" or "mph"
const char *Settings_DistanceUnitLabel(); // "km" or "mi"
