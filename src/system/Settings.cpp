#include "Settings.h"

#include <Preferences.h>
#include <esp_system.h>

#include "../hal/Display.h"

namespace {

constexpr char NVS_NAMESPACE[] = "pegasus";
constexpr char KEY_BRIGHTNESS[] = "bright";
constexpr char KEY_SPEED_UNIT[] = "unit";
// Retired with ANT+. Kept only so Settings_Init() can erase it, rather than
// leaving a dead key in the namespace for a future setting to trip over.
constexpr char KEY_HR_SOURCE_RETIRED[] = "hrsrc";
// Counts consecutive boots that entered radio bring-up and never came out,
// rather than merely flagging one. Two, not one, is what triggers the
// fallback -- see Settings_Init().
constexpr char KEY_RADIO_PENDING[] = "radiopend";
constexpr uint8_t RADIO_PENDING_LIMIT = 2;
constexpr char KEY_NAV_MODE[] = "navmode";
constexpr char KEY_HR_REST[] = "hrrest";
constexpr char KEY_HR_MAX[] = "hrmax";
constexpr char KEY_BOOT_COUNT[] = "boots";
constexpr char KEY_SLEEP[] = "sleepen";
constexpr char KEY_TRACKUP[] = "trackup";

// 60, not 100. The backlight is the largest single draw on this board while
// riding -- roughly half of it -- and it is the only one of the big three that
// costs nothing to turn down: the downclock never fires on a moving bike
// (PowerManager keeps the screen awake while the GPS reports movement) and the
// receiver cannot be throttled without costing fixes. On the estimates in
// CLAUDE.md this is the difference between about six hours and about eight.
//
// Only a default. NVS wins if the rider has ever moved the slider, which is
// the right way round: someone who chose 100 in bright sun chose it knowing
// what it looked like, and a firmware update must not quietly darken their
// screen. It therefore does nothing at all on a board that already has a
// stored value.
//
// Unverified in sunlight -- nobody has ridden with this. If 60 turns out to be
// unreadable outdoors the number is wrong, not the reasoning.
constexpr uint8_t DEFAULT_BRIGHTNESS = 60;
constexpr float KM_TO_MILES = 0.621371f;


// TBT is the only navigation mode that exists today; GPX is a declared
// destination, not a working feature.
constexpr NavMode_t DEFAULT_NAV_MODE = NAV_MODE_TBT;

// The rider's own measured pair, the same one HrZone.h's worked example uses.
// A default of nothing would be worse than a default that belongs to someone:
// the zone bar has to draw on first boot, and 51/174 at least produces the
// bands this device was checked against.
constexpr uint8_t DEFAULT_HR_REST = 51;
constexpr uint8_t DEFAULT_HR_MAX = 174;

// Outer bounds for each, wide enough for any rider and narrow enough to reject
// a byte that NVS never actually stored (a fresh key reads back as 0).
constexpr uint8_t HR_REST_MIN = 30;
constexpr uint8_t HR_REST_MAX = 120;
constexpr uint8_t HR_MAX_MIN = 90;
constexpr uint8_t HR_MAX_MAX = 220;

// Closest the two are allowed to get. One beat of reserve keeps the arithmetic
// defined but gives five zones to divide between it, so several would span no
// beats at all and the bar would lie about where the rider is. Twenty leaves
// every zone at least two beats wide.
constexpr uint8_t HR_MIN_RESERVE = 20;

Preferences s_prefs;
bool s_ready = false;

uint8_t s_brightness = DEFAULT_BRIGHTNESS;
SpeedUnit_t s_speed_unit = SPEED_UNIT_KMH;
NavMode_t s_nav_mode = DEFAULT_NAV_MODE;
// Opt-in: see Settings.h. An untested wake source that leaves the device
// looking dead is a poor default however mild the recovery.
bool s_sleep_enabled = false;
// Default on: the rider asked for it, and north-up costs a mental rotation at
// every junction.
bool s_track_up = true;
// Kept, and kept false: nothing forces the navigation mode any more. The
// accessor stays so the settings page compiles, and says so.
bool s_nav_fell_back = false;

// This boot came up after two unfinished radio bring-ups, so the GATT server
// and its advertisement are held off. Not persisted -- see where it is set.
bool s_radios_held_off = false;
uint8_t s_hr_rest = DEFAULT_HR_REST;
uint8_t s_hr_max = DEFAULT_HR_MAX;
const char *s_reset_text = "?";
bool s_reset_abnormal = false;
uint32_t s_boot_count = 0;

// Short enough for a settings row, specific enough to act on. The three that
// matter here are Panic (a crash in our code), the two watchdogs (something
// blocked or an ISR overran) and Brownout (the supply sagged, which on this
// board means USB current rather than firmware).
const char *ResetReasonText(esp_reset_reason_t reason, bool *abnormal) {
    *abnormal = false;
    switch (reason) {
        case ESP_RST_POWERON:  return "Power-on";
        case ESP_RST_EXT:      return "External pin";
        case ESP_RST_SW:       return "Restart";        // our own esp_restart()
        case ESP_RST_DEEPSLEEP:return "Deep sleep";
        case ESP_RST_SDIO:     return "SDIO";
        case ESP_RST_PANIC:    *abnormal = true; return "PANIC (crash)";
        case ESP_RST_INT_WDT:  *abnormal = true; return "Interrupt watchdog";
        case ESP_RST_TASK_WDT: *abnormal = true; return "Task watchdog";
        case ESP_RST_WDT:      *abnormal = true; return "Other watchdog";
        case ESP_RST_BROWNOUT: *abnormal = true; return "Brownout (power)";
        case ESP_RST_UNKNOWN:
        default:               return "Unknown";
    }
}

uint8_t ClampTo(uint8_t value, uint8_t low, uint8_t high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

} // namespace

void Settings_Init() {
    // Read-write namespace; begin() returns false if NVS is unavailable, in
    // which case the defaults above still apply and setters simply don't
    // persist rather than crashing the UI.
    s_ready = s_prefs.begin(NVS_NAMESPACE, false);

    if (s_ready) {
        s_brightness = s_prefs.getUChar(KEY_BRIGHTNESS, DEFAULT_BRIGHTNESS);
        s_speed_unit = (SpeedUnit_t)s_prefs.getUChar(KEY_SPEED_UNIT, SPEED_UNIT_KMH);
        s_nav_mode = (NavMode_t)s_prefs.getUChar(KEY_NAV_MODE, DEFAULT_NAV_MODE);
        s_sleep_enabled = s_prefs.getUChar(KEY_SLEEP, 0) != 0;
        s_track_up = s_prefs.getUChar(KEY_TRACKUP, 1) != 0;
        s_hr_rest = s_prefs.getUChar(KEY_HR_REST, DEFAULT_HR_REST);
        s_hr_max = s_prefs.getUChar(KEY_HR_MAX, DEFAULT_HR_MAX);
    }

    if (s_brightness > 100) {
        s_brightness = DEFAULT_BRIGHTNESS;
    }
    if (s_speed_unit != SPEED_UNIT_KMH && s_speed_unit != SPEED_UNIT_MPH) {
        s_speed_unit = SPEED_UNIT_KMH;
    }
    if (s_nav_mode != NAV_MODE_TBT && s_nav_mode != NAV_MODE_GPX) {
        s_nav_mode = DEFAULT_NAV_MODE;
    }

    // Range first, then the relationship between the two. Firmware older than
    // these keys leaves them absent and they read back as the defaults, but a
    // value written by a future build with different bounds would land here,
    // and so would a half-finished write. Falling back to the default pair
    // beats drawing a bar whose zones have no width.
    s_hr_rest = ClampTo(s_hr_rest, HR_REST_MIN, HR_REST_MAX);
    s_hr_max = ClampTo(s_hr_max, HR_MAX_MIN, HR_MAX_MAX);
    if (s_hr_max < s_hr_rest + HR_MIN_RESERVE) {
        s_hr_rest = DEFAULT_HR_REST;
        s_hr_max = DEFAULT_HR_MAX;
    }

    // The heart-rate source used to live beside this and constrain it. A board
    // that ran an older build still has that key in NVS, holding a value no
    // code reads any more; erase it rather than leave it to confuse whoever
    // dumps the namespace next.
    if (s_ready) {
        s_prefs.remove(KEY_HR_SOURCE_RETIRED);
    }

    // ---- Reset diagnostics ----
    // Read before anything else can restart us. A genuine power-on starts the
    // count again, so pulling the USB lead is how you clear it; the count is
    // what separates one bad boot from a loop.
    //
    // ⚠️ A deep-sleep wake does NOT count, and that exception is the whole
    // reason this is not a one-liner. The rule used to be "anything but a
    // power-on means the previous run ended without being asked to" -- which
    // was true right up until deep sleep started working, because until then
    // nothing ever ended a run deliberately. Now a rider who sleeps the device
    // every night would add a tick every night, and a real reboot loop would
    // be invisible against that background. Counting only the endings nobody
    // asked for is what makes the number mean anything.
    const esp_reset_reason_t reason = esp_reset_reason();
    s_reset_text = ResetReasonText(reason, &s_reset_abnormal);

    if (s_ready) {
        if (reason == ESP_RST_POWERON) {
            s_boot_count = 1;
        } else if (reason == ESP_RST_DEEPSLEEP) {
            // Carried, not raised: the previous run ended exactly as asked.
            s_boot_count = s_prefs.getUInt(KEY_BOOT_COUNT, 1);
        } else {
            s_boot_count = s_prefs.getUInt(KEY_BOOT_COUNT, 0) + 1;
        }
        s_prefs.putUInt(KEY_BOOT_COUNT, s_boot_count);
    } else {
        s_boot_count = 1;
    }

    // A raised count means a previous boot entered radio bring-up and never
    // came out. Fall back to GPX and persist it, so the device comes up usable
    // instead of repeating whatever hung -- otherwise the bad choice would
    // outlive even a reflash, since it lives in NVS rather than in the
    // firmware image.
    //
    // GPX is the safe end of this setting because it starts no radio at all:
    // the heart-rate client still comes up either way, but nothing advertises
    // and no GATT service is registered, which is where the bring-up hazards
    // in CLAUDE.md section 8 live.
    //
    // Two consecutive, not one, and the reason is specific. BLE_HR_Start()
    // scans synchronously for up to 15 seconds, so a rider who unplugs the
    // board during that window leaves the flag raised without anything having
    // hung. On one strike that unplug silently moves their navigation setting.
    // A real hang repeats on every boot and still trips this on the next one;
    // an impatient power cycle does not. It mattered less before ANT+ was
    // removed, because the fallback then moved the heart-rate source to BLE,
    // which was almost always already BLE and so invisible.
    if (s_ready && s_prefs.getUChar(KEY_RADIO_PENDING, 0) >= RADIO_PENDING_LIMIT) {
        // Holds the GATT server and its advertisement off for THIS boot, and
        // says nothing about the navigation mode.
        //
        // It used to force the mode to GPX, which worked only because GPX
        // happened to start no radio -- a coincidence, and one that cost the
        // whole phone-position feature the moment it was built, because a
        // rider in GPX mode then had no GATT server for the phone to write a
        // fix into and no advertisement for it to find. The two questions
        // "what navigation do I show" and "do I touch the radio" were never
        // the same question; they are now asked separately.
        //
        // One boot rather than persisted. The specific hangs section 8
        // records have since been fixed -- the registration order, the paused
        // advertisement, the parked supervisor -- so this is a net for an
        // unknown future hazard rather than a known present one, and a net
        // that permanently disables position is worse than one that retries.
        // A hang that really does repeat will trip this again two boots later.
        s_radios_held_off = true;
        s_prefs.putUChar(KEY_RADIO_PENDING, 0);
    }

    Display_SetBrightness(s_brightness);
}

void Settings_NoteRadioBringUpStart() {
    if (s_ready) {
        // Saturating, so a long run of bad boots cannot wrap the counter back
        // to zero and quietly disarm the net.
        const uint8_t seen = s_prefs.getUChar(KEY_RADIO_PENDING, 0);
        s_prefs.putUChar(KEY_RADIO_PENDING, (seen < 250) ? (uint8_t)(seen + 1) : seen);
    }
}

void Settings_NoteRadioBringUpOk() {
    if (s_ready) {
        s_prefs.putUChar(KEY_RADIO_PENDING, 0);
    }
}

bool Settings_RadiosHeldOff() {
    return s_radios_held_off;
}

bool Settings_DidNavModeFallBack() {
    return s_nav_fell_back;
}

const char *Settings_LastResetText() {
    return s_reset_text;
}

uint32_t Settings_BootCount() {
    return s_boot_count;
}

bool Settings_LastResetWasAbnormal() {
    return s_reset_abnormal;
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

uint8_t Settings_GetHrRestBpm() {
    return s_hr_rest;
}

uint8_t Settings_GetHrMaxBpm() {
    return s_hr_max;
}

void Settings_SetHrRestBpm(uint8_t bpm) {
    // Clamped against the maximum as well as the fixed bounds, so the pair can
    // never close up. The rider gets the nearest usable value rather than a
    // rejected edit with no feedback.
    uint8_t next = ClampTo(bpm, HR_REST_MIN, HR_REST_MAX);
    if (next + HR_MIN_RESERVE > s_hr_max) {
        next = (uint8_t)(s_hr_max - HR_MIN_RESERVE);
    }
    if (next == s_hr_rest) {
        return; // no NVS write for a no-op: this runs off a stepper
    }

    s_hr_rest = next;
    if (s_ready) {
        s_prefs.putUChar(KEY_HR_REST, s_hr_rest);
    }
}

void Settings_SetHrMaxBpm(uint8_t bpm) {
    uint8_t next = ClampTo(bpm, HR_MAX_MIN, HR_MAX_MAX);
    if (next < s_hr_rest + HR_MIN_RESERVE) {
        next = (uint8_t)(s_hr_rest + HR_MIN_RESERVE);
    }
    if (next == s_hr_max) {
        return;
    }

    s_hr_max = next;
    if (s_ready) {
        s_prefs.putUChar(KEY_HR_MAX, s_hr_max);
    }
}

bool Settings_GetMapTrackUp() {
    return s_track_up;
}

void Settings_SetMapTrackUp(bool track_up) {
    if (track_up == s_track_up) {
        return;
    }
    s_track_up = track_up;
    if (s_ready) {
        s_prefs.putUChar(KEY_TRACKUP, s_track_up ? 1 : 0);
    }
}

bool Settings_GetSleepEnabled() {
    return s_sleep_enabled;
}

void Settings_SetSleepEnabled(bool enabled) {
    if (enabled == s_sleep_enabled) {
        return;
    }
    s_sleep_enabled = enabled;
    if (s_ready) {
        s_prefs.putUChar(KEY_SLEEP, s_sleep_enabled ? 1 : 0);
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
    // Both modes now exist. GPX was the outstanding one and this function
    // used to return false for it, back when there was no card driver, no
    // parser and no renderer. All three landed -- SD_MMC in GpxTrack.cpp on
    // the board's SDIO pins, GpxParse.c, and MapView -- and the claim went
    // stale without anything forcing it to be revisited.
    //
    // Kept rather than deleted: it is the hook that stops a half-built mode
    // being offered as if it worked, which is a mistake worth being able to
    // make cheaply again.
    (void)mode;
    return true;
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
