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

// Where Sensor/HeartRate readings come from. These are radio *init* modes,
// not a runtime switch: SoftANT_Start(false) takes the BLE controller
// exclusively, whereas coexist mode needs NimBLE initialised and scanning
// before ANT opens (see SoftANT.h and BLE_HR_Client.h). main.cpp reads this
// once at boot and sequences the radios accordingly, so a change only takes
// effect after a restart.
// Exactly one source is active at a time -- these are mutually exclusive uses
// of the same radio, not a preference order.
typedef enum {
    HR_SOURCE_BLE = 0, // NimBLE client only -- standard 0x180D peer (watch or strap)
    HR_SOURCE_ANT = 1, // software ANT+ only -- exclusive use of the radio
} HrSource_t;

// Loads persisted values from NVS and applies them (brightness reaches the
// panel here). Call once from setup(), after Display_Init().
void Settings_Init();

// Backlight, 0-100 percent. The setter applies immediately and persists.
uint8_t Settings_GetBrightness();
void Settings_SetBrightness(uint8_t percent);

// Speed/distance unit system.
SpeedUnit_t Settings_GetSpeedUnit();
void Settings_SetSpeedUnit(SpeedUnit_t unit);

// Heart-rate source. Persisted immediately; applied at the next boot.
HrSource_t Settings_GetHrSource();
void Settings_SetHrSource(HrSource_t source);
const char *Settings_HrSourceLabel(HrSource_t source);

// Navigation source (CLAUDE.md §5).
typedef enum {
    NAV_MODE_TBT = 0, // BLE turn-by-turn pushed from the phone
    NAV_MODE_GPX = 1, // offline breadcrumb from a .gpx on the SD card
} NavMode_t;

// ---- The one exclusivity rule ----
// TBT is a NimBLE GATT server, and HR_SOURCE_ANT hands the BLE controller to
// deps/esp32-ant as a raw ANT modem with no NimBLE host (SoftANT.h). So
// NAV_MODE_TBT and HR_SOURCE_ANT can never both be active:
//
//     never (NAV_MODE_TBT and HR_SOURCE_ANT)
//
// "TBT requires BLE" and "ANT+ requires GPX" are contrapositives of that one
// statement, so it is enforced in exactly one place rather than as two rules
// that could drift apart. Both setters below repair the conflict by moving the
// OTHER setting, and Settings_Init() repairs a stored pair that violates it.
// Callers that care which way it moved should re-read both after setting.
NavMode_t Settings_GetNavMode();
void Settings_SetNavMode(NavMode_t mode);
const char *Settings_NavModeLabel(NavMode_t mode);

// False for modes whose implementation doesn't exist yet, so the UI can say so
// instead of offering a choice that quietly does nothing.
bool Settings_NavModeIsImplemented(NavMode_t mode);

// ---- Radio bring-up safety net ----
// The HR source lives in NVS, so a mode that hangs during bring-up would
// survive a reflash and leave the device looping with no reachable UI to undo
// it. main.cpp raises a flag before touching the radios and clears it once
// setup() completes; if Settings_Init() finds the flag still raised, the
// previous boot died mid-bring-up and the source is forced back to BLE.
void Settings_NoteRadioBringUpStart();
void Settings_NoteRadioBringUpOk();

// True when this boot fell back to BLE because the previous one didn't finish.
bool Settings_DidHrSourceFallBack();

// Unit-aware conversion helpers, so pages never hardcode a unit.
float Settings_SpeedFromKmh(float kmh);
float Settings_DistanceFromKm(float km);
const char *Settings_SpeedUnitLabel();    // "km/h" or "mph"
const char *Settings_DistanceUnitLabel(); // "km" or "mi"
