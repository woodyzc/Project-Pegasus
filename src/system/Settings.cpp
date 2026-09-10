#include "Settings.h"

#include <Preferences.h>

#include "../hal/Display.h"

namespace {

constexpr char NVS_NAMESPACE[] = "pegasus";
constexpr char KEY_BRIGHTNESS[] = "bright";
constexpr char KEY_SPEED_UNIT[] = "unit";
constexpr char KEY_HR_SOURCE[] = "hrsrc";
constexpr char KEY_RADIO_PENDING[] = "radiopend";
constexpr char KEY_NAV_MODE[] = "navmode";

constexpr uint8_t DEFAULT_BRIGHTNESS = 100;
constexpr float KM_TO_MILES = 0.621371f;

// Defaults to BLE-only deliberately, not to the CLAUDE.md §3 "ANT+ primary"
// arrangement. ANT+ has never been exercised on this hardware, and a failure
// in SoftANT_Start() at boot would strand the user on a dead screen with no
// way to reach this setting and change it back. Opting in to ANT+ is a
// deliberate act until it is proven on the bench.
constexpr HrSource_t DEFAULT_HR_SOURCE = HR_SOURCE_BLE;

// TBT is the only navigation mode that exists today; GPX is a declared
// destination, not a working feature.
constexpr NavMode_t DEFAULT_NAV_MODE = NAV_MODE_TBT;

Preferences s_prefs;
bool s_ready = false;

uint8_t s_brightness = DEFAULT_BRIGHTNESS;
SpeedUnit_t s_speed_unit = SPEED_UNIT_KMH;
HrSource_t s_hr_source = DEFAULT_HR_SOURCE;
NavMode_t s_nav_mode = DEFAULT_NAV_MODE;
bool s_hr_fell_back = false;

} // namespace

void Settings_Init() {
    // Read-write namespace; begin() returns false if NVS is unavailable, in
    // which case the defaults above still apply and setters simply don't
    // persist rather than crashing the UI.
    s_ready = s_prefs.begin(NVS_NAMESPACE, false);

    if (s_ready) {
        s_brightness = s_prefs.getUChar(KEY_BRIGHTNESS, DEFAULT_BRIGHTNESS);
        s_speed_unit = (SpeedUnit_t)s_prefs.getUChar(KEY_SPEED_UNIT, SPEED_UNIT_KMH);
        s_hr_source = (HrSource_t)s_prefs.getUChar(KEY_HR_SOURCE, DEFAULT_HR_SOURCE);
        s_nav_mode = (NavMode_t)s_prefs.getUChar(KEY_NAV_MODE, DEFAULT_NAV_MODE);
    }

    if (s_brightness > 100) {
        s_brightness = DEFAULT_BRIGHTNESS;
    }
    if (s_speed_unit != SPEED_UNIT_KMH && s_speed_unit != SPEED_UNIT_MPH) {
        s_speed_unit = SPEED_UNIT_KMH;
    }
    // Also catches a stored 2 from the earlier three-way version of this
    // setting, which now maps back to the default rather than a dead value.
    if (s_hr_source != HR_SOURCE_BLE && s_hr_source != HR_SOURCE_ANT) {
        s_hr_source = DEFAULT_HR_SOURCE;
    }
    if (s_nav_mode != NAV_MODE_TBT && s_nav_mode != NAV_MODE_GPX) {
        s_nav_mode = DEFAULT_NAV_MODE;
    }

    // Repair a stored pair that breaks the exclusivity rule -- possible if the
    // two keys were written by different firmware versions. The heart-rate
    // source wins here: silently changing which sensor a rider's data comes
    // from is a worse surprise than losing turn prompts.
    if (s_nav_mode == NAV_MODE_TBT && s_hr_source == HR_SOURCE_ANT) {
        s_nav_mode = NAV_MODE_GPX;
        if (s_ready) {
            s_prefs.putUChar(KEY_NAV_MODE, (uint8_t)s_nav_mode);
        }
    }

    // A still-raised flag means the previous boot entered radio bring-up and
    // never came out. Fall back to BLE and persist it, so the device comes up
    // usable instead of repeating whatever hung -- otherwise the bad choice
    // would outlive even a reflash, since it lives in NVS rather than in the
    // firmware image.
    if (s_ready && s_prefs.getUChar(KEY_RADIO_PENDING, 0) != 0) {
        s_hr_fell_back = (s_hr_source != HR_SOURCE_BLE);
        s_hr_source = HR_SOURCE_BLE;
        s_prefs.putUChar(KEY_HR_SOURCE, (uint8_t)s_hr_source);
        s_prefs.putUChar(KEY_RADIO_PENDING, 0);
    }

    Display_SetBrightness(s_brightness);
}

void Settings_NoteRadioBringUpStart() {
    if (s_ready) {
        s_prefs.putUChar(KEY_RADIO_PENDING, 1);
    }
}

void Settings_NoteRadioBringUpOk() {
    if (s_ready) {
        s_prefs.putUChar(KEY_RADIO_PENDING, 0);
    }
}

bool Settings_DidHrSourceFallBack() {
    return s_hr_fell_back;
}

uint8_t Settings_GetBrightness() {
    return s_brightness;
}

void Settings_SetBrightness(uint8_t percent) {
    if (percent > 100) {
        percent = 100;
    }
    if (percent == s_brightness) {
        return; // no NVS write for a no-op: flash wear, and this runs off a slider
    }

    s_brightness = percent;
    Display_SetBrightness(s_brightness);
    if (s_ready) {
        s_prefs.putUChar(KEY_BRIGHTNESS, s_brightness);
    }
}

SpeedUnit_t Settings_GetSpeedUnit() {
    return s_speed_unit;
}

void Settings_SetSpeedUnit(SpeedUnit_t unit) {
    if (unit == s_speed_unit) {
        return;
    }

    s_speed_unit = unit;
    if (s_ready) {
        s_prefs.putUChar(KEY_SPEED_UNIT, (uint8_t)s_speed_unit);
    }
}

HrSource_t Settings_GetHrSource() {
    return s_hr_source;
}

void Settings_SetHrSource(HrSource_t source) {
    if (source == s_hr_source) {
        return;
    }

    s_hr_source = source;
    if (s_ready) {
        s_prefs.putUChar(KEY_HR_SOURCE, (uint8_t)s_hr_source);
    }

    // Exclusivity: ANT+ leaves no NimBLE host for the TBT GATT server.
    if (s_hr_source == HR_SOURCE_ANT && s_nav_mode == NAV_MODE_TBT) {
        s_nav_mode = NAV_MODE_GPX;
        if (s_ready) {
            s_prefs.putUChar(KEY_NAV_MODE, (uint8_t)s_nav_mode);
        }
    }
}

NavMode_t Settings_GetNavMode() {
    return s_nav_mode;
}

void Settings_SetNavMode(NavMode_t mode) {
    if (mode == s_nav_mode) {
        return;
    }

    s_nav_mode = mode;
    if (s_ready) {
        s_prefs.putUChar(KEY_NAV_MODE, (uint8_t)s_nav_mode);
    }

    // The same rule from the other side: TBT needs the BLE stack up.
    if (s_nav_mode == NAV_MODE_TBT && s_hr_source == HR_SOURCE_ANT) {
        s_hr_source = HR_SOURCE_BLE;
        if (s_ready) {
            s_prefs.putUChar(KEY_HR_SOURCE, (uint8_t)s_hr_source);
        }
    }
}

const char *Settings_NavModeLabel(NavMode_t mode) {
    switch (mode) {
        case NAV_MODE_GPX:
            return "GPX";
        case NAV_MODE_TBT:
        default:
            return "TBT";
    }
}

bool Settings_NavModeIsImplemented(NavMode_t mode) {
    // No SD card driver, no GPX parser and no breadcrumb renderer exist yet.
    // The board does have the slot (SDIO on IO38/40 + IO39/41/47/48), so this
    // is a missing feature rather than a missing capability.
    return mode != NAV_MODE_GPX;
}

const char *Settings_HrSourceLabel(HrSource_t source) {
    switch (source) {
        case HR_SOURCE_ANT:
            return "ANT+";
        case HR_SOURCE_BLE:
        default:
            return "BLE";
    }
}

float Settings_SpeedFromKmh(float kmh) {
    return (s_speed_unit == SPEED_UNIT_MPH) ? kmh * KM_TO_MILES : kmh;
}

float Settings_DistanceFromKm(float km) {
    return (s_speed_unit == SPEED_UNIT_MPH) ? km * KM_TO_MILES : km;
}

const char *Settings_SpeedUnitLabel() {
    return (s_speed_unit == SPEED_UNIT_MPH) ? "mph" : "km/h";
}

const char *Settings_DistanceUnitLabel() {
    return (s_speed_unit == SPEED_UNIT_MPH) ? "mi" : "km";
}
