#include "Page_Settings.h"

#include <Arduino.h>
#include <WiFi.h>

#include "../navigation/RoadMap.h"
#include "RoadView.h"
#include "../system/TimeSource.h" // WiFi.macAddress() -- reads the eFused MAC, no radio started

#include "../navigation/BLE_TBT_Receiver.h"
#include "../navigation/GpxTrack.h"
#include "../navigation/RideLog.h"
#include "Overlay_FileTransfer.h"
#include "Overlay_RideSummary.h"
#include "../sensors/BLE_HR_Client.h"
#include "../system/HrZone.h"
#include "../system/PageManager/PageManager.h"
#include "../system/DataCenter.h"
#include "../system/PowerManager.h"
#include "../system/Settings.h"
#include "Page_Dashboard.h"

// Matches Page_Dashboard's palette so the two screens read as one product.

namespace {

constexpr uint32_t COLOR_BG = 0x101820;
constexpr uint32_t COLOR_CAPTION = 0x93A4B8;
constexpr uint32_t COLOR_VALUE = 0xFFFFFF;
constexpr uint32_t COLOR_ACCENT = 0x61DAFB;
constexpr uint32_t COLOR_PANEL = 0x18232E;
constexpr uint32_t COLOR_DANGER = 0xFF6B6B;
constexpr uint32_t COLOR_OK = 0x7CE38B;

// ---- The ride pair, which is one control in two halves ----
//
// Exactly one of "new ride" and "finish" is live at a time, because exactly
// one of them can mean anything: starting a ride that is already started
// silently threw the current one away and began another, and finishing one
// that was never started did nothing at all while looking like it had.
//
// Recessed rather than merely faded. A dimmed button on a card this colour
// still reads as a button, and a rider who has just pressed one and is
// watching for a change needs the dead half to look dead.
constexpr uint32_t COLOR_BTN_DEAD_BG = 0x141C24;
constexpr uint32_t COLOR_BTN_DEAD_INK = 0x53616F;
constexpr uint32_t COLOR_BTN_START_BG = 0x16281E;
// Lifted off COLOR_PANEL (0x18232E). The old 0x14242E was within a few counts
// of the card behind it, so the finish button read as an icon floating on the
// card rather than as a control at all.
constexpr uint32_t COLOR_BTN_FINISH_BG = 0x1E3340;

lv_obj_t *s_ride_start_btn = nullptr;
lv_obj_t *s_ride_start_icon = nullptr;
lv_obj_t *s_ride_start_label = nullptr;
lv_obj_t *s_ride_finish_btn = nullptr;
lv_obj_t *s_ride_finish_icon = nullptr;
lv_obj_t *s_ride_finish_label = nullptr;

lv_obj_t *s_brightness_value = nullptr;
lv_obj_t *s_unit_value = nullptr;
lv_obj_t *s_trip_status = nullptr;
lv_obj_t *s_uptime_value = nullptr;
lv_obj_t *s_ridelog_value = nullptr;
lv_obj_t *s_power_status = nullptr;

// ---- The raw battery reading, because the derived one cannot be checked ----
//
// Everything else on this board shows a percentage, and a percentage cannot
// tell you whether the thing it was derived from is sane. Two questions this
// answers and nothing else could:
//
//   * is the divider right? A 3:1 that is really 2:1 reads as a pack that is
//     simply flatter or fuller than it is, on a curve where 3.7V and 4.2V are
//     only half a volt apart.
//   * is `on usb` ever true? It is a threshold at 4500mV on this same reading,
//     and a 1S charger terminates at 4.2V -- so if the ADC senses the pack
//     rather than VBUS, that flag can never fire. The dashboard's status bar
//     agrees that it does not: plugged in, it shows a plain battery icon where
//     on_usb would give it LV_SYMBOL_CHARGE.
//
//     It no longer gates deep sleep -- that moved to ride state, exactly
//     because a flag that can never be true is not a gate -- so what is left
//     riding on it is cosmetic: the charge icon, and suppressing the red
//     low-battery colour while charging. Worth fixing, not worth hurrying.
lv_obj_t *s_power_battery = nullptr;
lv_obj_t *s_hrlink_value = nullptr;
lv_obj_t *s_tbtlink_value = nullptr;
lv_obj_t *s_heap_value = nullptr;
lv_timer_t *s_info_timer = nullptr;


lv_obj_t *s_hr_rest_value = nullptr;
lv_obj_t *s_hr_max_value = nullptr;
lv_obj_t *s_hr_zone_table = nullptr;

// One beat per press is too slow for a 50-beat correction on a touchscreen,
// and ten overshoots. Five matches how precisely either number is known.
constexpr int HR_BPM_STEP = 5;

constexpr int NAV_MODE_COUNT = 2; // index i == NavMode_t value i (TBT=0, GPX=1)
lv_obj_t *s_nav_btns[NAV_MODE_COUNT] = {nullptr, nullptr};
lv_obj_t *s_nav_note = nullptr;
// Shown only while the chosen mode differs from the one this boot actually
// started with. A restart button standing there permanently would read as
// something the rider is supposed to press.
lv_obj_t *s_nav_restart_btn = nullptr;
NavMode_t s_nav_mode_at_load = NAV_MODE_TBT;

// One card per settings group, so the page scrolls as a tidy stack.
lv_obj_t *MakeCard(lv_obj_t *parent, const char *caption) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, caption);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_CAPTION), 0);
    return card;
}

// A swipe left leaves, matching the swipe right that opened this page.
//
// The dashboard puts settings, page one and page two on one strip and moves
// along it with left and right; this is the far end of that strip, so left is
// the way back and there is nothing further to the right.
void OnSettingsGesture(lv_event_t *e) {
    if (lv_indev_get_gesture_dir(lv_indev_get_act()) != LV_DIR_LEFT) {
        return;
    }
    PageSettings *self = (PageSettings *)lv_event_get_user_data(e);
    if (self != nullptr && self->_Manager != nullptr) {
        self->_Manager->Pop();
    }
}

void OnBackClicked(lv_event_t *e) {
    PageSettings *self = (PageSettings *)lv_event_get_user_data(e);
    if (self != nullptr && self->_Manager != nullptr) {
        self->_Manager->Pop();
    }
}

void OnBrightnessChanged(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    const int32_t value = lv_slider_get_value(slider);

    // Applies to the panel immediately and persists to NVS (the setter skips
    // the write when the value hasn't actually changed, so dragging the
    // slider doesn't hammer flash).
    Settings_SetBrightness((uint8_t)value);
    lv_label_set_text_fmt(s_brightness_value, "%d%%", (int)value);
}

void OnUnitToggled(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    const bool mph = lv_obj_has_state(sw, LV_STATE_CHECKED);

    Settings_SetSpeedUnit(mph ? SPEED_UNIT_MPH : SPEED_UNIT_KMH);
    lv_label_set_text(s_unit_value, Settings_SpeedUnitLabel());
}

void SetRideButton(lv_obj_t *btn, lv_obj_t *icon, lv_obj_t *label, bool live, uint32_t live_bg,
                   uint32_t live_ink) {
    if (btn == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(btn, lv_color_hex(live ? live_bg : COLOR_BTN_DEAD_BG), 0);
    const lv_color_t ink = lv_color_hex(live ? live_ink : COLOR_BTN_DEAD_INK);
    if (icon != nullptr) {
        lv_obj_set_style_text_color(icon, ink, 0);
    }
    if (label != nullptr) {
        lv_obj_set_style_text_color(label, ink, 0);
    }
    // Both, deliberately. LV_STATE_DISABLED is what the styling hangs off, and
    // clearing CLICKABLE is what actually stops the press -- relying on the
    // state alone would leave a dead-looking button that still fires.
    if (live) {
        lv_obj_clear_state(btn, LV_STATE_DISABLED);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    }
}

// `armed` is passed rather than read, because the truth arrives late.
// RideLog_StartNewRide() posts a command through the writer queue and
// RideLog_IsArmed() does not change until that task picks it up -- so a
// handler that refreshed from the real state straight after a press would read
// the old value and leave the button live for up to a second, which is exactly
// long enough for a second press. The handlers pass what they just asked for;
// the one-second timer passes the truth and corrects any disagreement.
void RefreshRideButtons(bool armed) {
    SetRideButton(s_ride_start_btn, s_ride_start_icon, s_ride_start_label, !armed,
                  COLOR_BTN_START_BG, COLOR_OK);
    SetRideButton(s_ride_finish_btn, s_ride_finish_icon, s_ride_finish_label, armed,
                  COLOR_BTN_FINISH_BG, COLOR_ACCENT);
}

void OnRideSummaryClicked(lv_event_t *e) {
    (void)e;
    RideSummary_t summary;
    RideSummary_Capture(&summary);
    Overlay_RideSummary_Show(&summary, false);
}

void OnStartNewRideClicked(lv_event_t *e) {
    (void)e;
    // The button should already be dead, so this is the second lock on the
    // same door: a press that slipped through the window between asking and
    // the writer task agreeing would otherwise discard the ride in progress
    // and silently start another.
    if (RideLog_IsArmed()) {
        return;
    }
    // Captured before anything is reset, which is the whole reason the summary
    // is a snapshot rather than a live view: one press zeroes every figure in
    // it, and this is the last instant they exist.
    RideSummary_t ending;
    RideSummary_Capture(&ending);

    const bool log_split = Page_Dashboard_StartNewRide();
    // Optimistic, and corrected within the second if the queue refused it.
    RefreshRideButtons(log_split);

    // Honest about the half that can fail. The odometer and the averages are
    // memory and always reset; the log has to reach the writer task, and if it
    // did not then the ride on the card is still the old one.
    if (log_split) {
        lv_label_set_text(s_trip_status,
                          "New ride. Odometer and averages cleared; the next fix "
                          "starts a new file.");
        lv_obj_set_style_text_color(s_trip_status, lv_color_hex(COLOR_OK), 0);
    } else {
        lv_label_set_text(s_trip_status,
                          "Odometer and averages cleared, but the ride log did not "
                          "restart.");
        lv_obj_set_style_text_color(s_trip_status, lv_color_hex(COLOR_DANGER), 0);
    }

    // Last, so the report sits over whatever the status line just said.
    // Skipped for a ride with nothing in it -- pressing this twice in a row
    // should not hand the rider an empty page the second time.
    if (!RideSummary_IsEmpty(&ending)) {
        Overlay_RideSummary_Show(&ending, true);
    }
}


void OnFinishRideClicked(lv_event_t *e) {
    (void)e;
    if (!RideLog_IsArmed()) {
        return;
    }
    // Snapshot first, for the same reason the start handler does it: this is
    // the last instant the figures exist as a ride.
    RideSummary_t ending;
    RideSummary_Capture(&ending);

    const bool stopped = RideLog_FinishRide();
    RefreshRideButtons(!stopped);

    if (stopped) {
        lv_label_set_text(s_trip_status,
                          "Ride finished. Nothing is being recorded until you "
                          "start another.");
        lv_obj_set_style_text_color(s_trip_status, lv_color_hex(COLOR_OK), 0);
    } else {
        // The queue was full, which means the writer is badly behind. Saying
        // so matters more here than anywhere else: the rider believes they
        // have stopped recording and they have not.
        lv_label_set_text(s_trip_status,
                          "Could not finish the ride - the card writer is busy. "
                          "Still recording; try again.");
        lv_obj_set_style_text_color(s_trip_status, lv_color_hex(COLOR_DANGER), 0);
    }

    // Deliberately leaves the odometer and the averages alone. Finishing says
    // the ride is over, not that it never happened -- the rider should still
    // be able to read what it came to. "Start new ride" is what clears them.
    if (!RideSummary_IsEmpty(&ending)) {
        Overlay_RideSummary_Show(&ending, true);
    }
}

// Hidden unless a mode change is waiting on it. Every branch of
// RefreshNavSelection that is not "saved, not yet applied" hides it, which is
// why that one branch returns early rather than falling through.
void ShowNavRestart(bool on) {
    if (s_nav_restart_btn == nullptr) {
        return;
    }
    if (on) {
        lv_obj_clear_flag(s_nav_restart_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_nav_restart_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

void RefreshNavSelection() {
    const NavMode_t current = Settings_GetNavMode();

    for (int i = 0; i < NAV_MODE_COUNT; i++) {
        if (s_nav_btns[i] == nullptr) {
            continue;
        }
        const bool active = ((int)current == i);
        lv_obj_set_style_bg_color(s_nav_btns[i],
                                  lv_color_hex(active ? COLOR_ACCENT : 0x24313D), 0);
        lv_obj_t *label = lv_obj_get_child(s_nav_btns[i], 0);
        if (label != nullptr) {
            lv_obj_set_style_text_color(label, lv_color_hex(active ? 0x081015 : COLOR_VALUE), 0);
        }
    }

    if (s_nav_note == nullptr) {
        return;
    }

    // ---- What is actually true, gathered once ----
    //
    // ⚠️ GpxTrack_PointCount() is how many points are LOADED, not how many
    // files are on the card, and the two are wildly different things here.
    // main.cpp only calls GpxTrack_LoadFirstAvailable() when the boot mode was
    // already GPX -- so after switching TBT to GPX nothing has ever been
    // loaded, and a point count of zero means "we never looked", not "the card
    // is empty". This page read it as the latter and told riders with a card
    // full of routes that it could not find any. GpxTrack_ScanFiles() is the
    // question that was meant, and is what Page_Map has always asked.
    //
    // Scanning touches the filesystem, which is why it is done here rather
    // than left in the chain below: this function runs on a page load and on a
    // mode button press, both rare, and nothing puts it on a timer.
    const bool card_up = GpxTrack_CardMounted();
    const size_t points_loaded = GpxTrack_PointCount();
    const size_t files_on_card = (card_up && points_loaded == 0) ? GpxTrack_ScanFiles() : 0;

    // Decided on its own, before the note is written, and deliberately not
    // folded into the chain below. The chain picks which of seven things to
    // say; this asks one question -- would restarting change anything -- and
    // an early attempt to answer it inside the chain got it wrong, because the
    // branch for "GPX selected but no card" fires ahead of the one for "mode
    // changed" and would have hidden the button on a note that ends with the
    // word "restart".
    //
    // The last term is the case above: routes are sitting on the card and a
    // restart is the only thing that will load one. Deliberately NOT shown
    // when the card has no .gpx at all -- restarting cannot conjure a file,
    // and a button that changes nothing is worse than no button.
    const bool mode_change_pending = (current != s_nav_mode_at_load);
    const bool card_would_be_remounted = (current == NAV_MODE_GPX && !card_up);
    const bool route_waiting_to_load =
        (current == NAV_MODE_GPX && card_up && points_loaded == 0 && files_on_card > 0);
    ShowNavRestart(mode_change_pending || card_would_be_remounted || route_waiting_to_load);

    // First, because it explains a setting the rider did not choose. The
    // reporting for this used to live in the heart-rate card, which the ANT+
    // removal deleted; without it the fallback moves the setting silently and
    // looks like the device forgetting what it was told.
    if (Settings_DidNavModeFallBack() && current == s_nav_mode_at_load) {
        lv_label_set_text(s_nav_note, "Forced to GPX: the previous boot did not finish "
                                      "bringing up the radios.");
        lv_obj_set_style_text_color(s_nav_note, lv_color_hex(COLOR_DANGER), 0);
    } else if (!Settings_NavModeIsImplemented(current)) {
        // Say so rather than let the rider discover an empty ROUTE panel on
        // the road.
        lv_label_set_text(s_nav_note, "Not built yet: no navigation will be shown in this mode.");
        lv_obj_set_style_text_color(s_nav_note, lv_color_hex(COLOR_DANGER), 0);
    } else if (current == NAV_MODE_GPX && !card_up) {
        // The one failure a rider can actually fix, and it is silent
        // otherwise: GPX with no card shows an empty map and no explanation.
        lv_label_set_text(s_nav_note, "No SD card. Insert one with a .gpx in its root "
                                      "folder, then restart.");
        lv_obj_set_style_text_color(s_nav_note, lv_color_hex(COLOR_DANGER), 0);
    } else if (current == NAV_MODE_GPX && points_loaded == 0 && files_on_card > 0) {
        // Routes are there; this boot simply never looked, because it came up
        // in TBT. Not a fault, so not red -- it is a step left to take, which
        // is the same thing the restart button below is saying.
        lv_label_set_text_fmt(s_nav_note,
                              "%u route%s on the card, none loaded yet. Restart to load one.",
                              (unsigned)files_on_card, files_on_card == 1 ? "" : "s");
        lv_obj_set_style_text_color(s_nav_note, lv_color_hex(COLOR_ACCENT), 0);
    } else if (current == NAV_MODE_GPX && points_loaded == 0) {
        lv_label_set_text(s_nav_note, "Card mounted, but no .gpx found in its root folder.");
        lv_obj_set_style_text_color(s_nav_note, lv_color_hex(COLOR_DANGER), 0);
    } else if (current == NAV_MODE_GPX && current == s_nav_mode_at_load) {
        lv_label_set_text_fmt(s_nav_note, "Active: %s, %u points.", GpxTrack_LoadedName(),
                              (unsigned)points_loaded);
        lv_obj_set_style_text_color(s_nav_note, lv_color_hex(COLOR_CAPTION), 0);
    } else if (current != s_nav_mode_at_load) {
        lv_label_set_text(s_nav_note, "Saved. Restart to apply.");
        lv_obj_set_style_text_color(s_nav_note, lv_color_hex(COLOR_ACCENT), 0);
    } else {
        lv_label_set_text(s_nav_note, "Active now.");
        lv_obj_set_style_text_color(s_nav_note, lv_color_hex(COLOR_CAPTION), 0);
    }
}

// Nothing else moves when this changes. It used to coerce the heart-rate
// source, because turn-by-turn needed the NimBLE host that ANT+ took away.
void OnTrackUpToggled(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    // Applies on the map page's next redraw. Nothing to restart: the
    // orientation is read per frame, not at boot like the radio settings.
    Settings_SetMapTrackUp(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

void OnNavModeClicked(lv_event_t *e) {
    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    Settings_SetNavMode((NavMode_t)index);
    RefreshNavSelection();
}

// Re-reads both numbers from Settings rather than tracking them locally, so
// the display shows what was actually stored after clamping -- press "-" at
// the bottom of the range and the value visibly stops rather than drifting
// away from what the zones are computed from.
void RefreshHrZoneCard() {
    const uint8_t rest = Settings_GetHrRestBpm();
    const uint8_t max = Settings_GetHrMaxBpm();

    if (s_hr_rest_value != nullptr) {
        lv_label_set_text_fmt(s_hr_rest_value, "%d", (int)rest);
    }
    if (s_hr_max_value != nullptr) {
        lv_label_set_text_fmt(s_hr_max_value, "%d", (int)max);
    }

    // The resulting bands, spelled out. Two abstract numbers become five
    // concrete ranges the rider can check against their phone, which is the
    // only way to tell that the pair was entered correctly.
    if (s_hr_zone_table != nullptr) {
        char table[96] = {0};
        size_t used = 0;
        for (int zone = 0; zone < HR_ZONE_COUNT; zone++) {
            const int written =
                snprintf(table + used, sizeof(table) - used, "%sZ%d %d-%d", (zone > 0) ? "\n" : "",
                         zone + 1, (int)HrZone_LowerBpm(zone, rest, max),
                         (int)HrZone_UpperBpm(zone, rest, max));
            if (written <= 0 || (size_t)written >= sizeof(table) - used) {
                break;
            }
            used += (size_t)written;
        }
        lv_label_set_text(s_hr_zone_table, table);
    }
}

// Kept inside 0..255 before narrowing: the setters clamp, but a step that
// underflowed the cast would arrive there as a large number and clamp to the
// top of the range instead of the bottom -- the button would jump the value
// the wrong way at the end of its travel.
uint8_t SteppedBpm(uint8_t current, int step) {
    const int next = (int)current + step;
    if (next < 0) {
        return 0;
    }
    if (next > 255) {
        return 255;
    }
    return (uint8_t)next;
}

// user_data carries the signed step, so one handler serves both buttons.
void OnHrRestStep(lv_event_t *e) {
    const int step = (int)(intptr_t)lv_event_get_user_data(e);
    Settings_SetHrRestBpm(SteppedBpm(Settings_GetHrRestBpm(), step));
    RefreshHrZoneCard();
}

void OnHrMaxStep(lv_event_t *e) {
    const int step = (int)(intptr_t)lv_event_get_user_data(e);
    Settings_SetHrMaxBpm(SteppedBpm(Settings_GetHrMaxBpm(), step));
    RefreshHrZoneCard();
}

void OnRestartClicked(lv_event_t *e) {
    (void)e;

    // Drop the BLE link before resetting, and do it here rather than relying
    // on the shutdown handler BLE_HR_Start() registers. That handler runs only
    // if esp_restart() honours it, which was assumed and never verified -- and
    // the symptom it was meant to fix survived: a restart with the watch
    // connected left it holding the link, so it stopped advertising and the
    // scan afterwards found 59 devices and no heart-rate service at all.
    //
    // This path is a plain task context with the scheduler running, so the
    // disconnect has somewhere to complete. It blocks for up to about 1.5s,
    // which is invisible directly before a reset.
    BLE_HR_Shutdown();
    ESP.restart();
}

// The restart button, built twice: once in NAVIGATION where a mode change
// needs it, and once at the bottom of DEVICE as the general utility.
//
// It used to live in HEART RATE, which was where it had ended up rather than
// where it belonged -- a power button under the strap settings, several
// screens above the mode change that was the only thing actually requiring it.
lv_obj_t *MakeRestartButton(lv_obj_t *card, const char *text) {
    lv_obj_t *btn = lv_btn_create(card);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x24313D), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, OnRestartClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_center(label);
    return btn;
}

void RefreshPowerStatus() {
    // Raw first, and deliberately raw: millivolts as measured, the percentage
    // derived from them, and the USB flag that is a threshold on the same
    // number. Printed together so a wrong divider or an impossible threshold
    // is visible as a disagreement rather than having to be inferred.
    if (s_power_battery != nullptr) {
        Battery_t battery;
        if (DataCenter_Pull(TOPIC_BATTERY, &battery, sizeof(battery))) {
            lv_label_set_text_fmt(s_power_battery, "Battery: %u mV, %u%%, on USB: %s",
                                  (unsigned)battery.millivolts, (unsigned)battery.percent,
                                  battery.on_usb ? "yes" : "NO");
        } else {
            lv_label_set_text(s_power_battery, "Battery: nothing published yet");
        }
    }

    if (s_power_status == nullptr) {
        return;
    }
    // The clock line goes on BOTH branches, and that is the whole point of it
    // being a separate line. It first went only on the "nothing is blocking
    // sleep" branch, which was then the branch almost nobody ever saw --
    // InhibitText() returned something nearly always, so the diagnostic was
    // invisible on the one screen built to show it.
    //
    // The two are about different things, and that has not changed even though
    // the inhibitors have. Those stop DEEP SLEEP; dimming, blanking and the
    // downclock that rides along with blanking happen regardless. Reporting
    // the clock only when sleep was possible tied it to a condition it has
    // nothing to do with.
    const char *blocked = PowerManager_InhibitText();
    if (blocked[0] != '\0') {
        lv_label_set_text_fmt(s_power_status, "Screen: %s @ %uMHz (idled %ux).\nWill not sleep: %s.",
                              PowerManager_StageText(), (unsigned)PowerManager_CpuMhz(),
                              (unsigned)PowerManager_DownclockCount(), blocked);
    } else {
        const uint32_t idle_ms = PowerManager_IdleMs();
        // Asked for, not recomputed: there are two thresholds now and which
        // applies depends on whether a ride has been recorded and finished.
        const uint32_t after_ms = PowerManager_SleepAfterMs();
        const uint32_t left_s = (idle_ms >= after_ms) ? 0u : ((after_ms - idle_ms) / 1000u);
        lv_label_set_text_fmt(s_power_status, "Screen: %s @ %uMHz (idled %ux).\nSleeps in %u s.",
                              PowerManager_StageText(), (unsigned)PowerManager_CpuMhz(),
                              (unsigned)PowerManager_DownclockCount(), (unsigned)left_s);
    }
}

void OnSleepToggled(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    Settings_SetSleepEnabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
    RefreshPowerStatus();
}

void OnFileTransferClicked(lv_event_t *e) {
    (void)e;
    // Everything from here is the overlay's: it takes the radio, the card and
    // the screen, and the only way out is a restart.
    Overlay_FileTransfer_Show();
}

void InfoTimerCallback(lv_timer_t *timer) {
    (void)timer;

    const uint32_t seconds = millis() / 1000UL;
    lv_label_set_text_fmt(s_uptime_value, "%luh %02lum %02lus", (unsigned long)(seconds / 3600UL),
                          (unsigned long)((seconds / 60UL) % 60UL), (unsigned long)(seconds % 60UL));
    lv_label_set_text_fmt(s_heap_value, "%u KB free / PSRAM %u KB free",
                          (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024));

    // Recording starts on the first fix, which is usually after this page was
    // built, so a value set only at load would read "waiting for fix" for the
    // whole ride and suggest the log was broken when it was working.
    if (s_hrlink_value != nullptr) {
        lv_label_set_text_fmt(s_hrlink_value, "Link: %s", BLE_HR_StatusText());
    }

    // The phone can only ever report that it did not find the head unit, which
    // is the same message whether the board is silent or the phone is. This is
    // the other half of that conversation.
    if (s_tbtlink_value != nullptr) {
        if (Settings_GetNavMode() != NAV_MODE_TBT) {
            lv_label_set_text(s_tbtlink_value, "TBT: off (GPX mode)");
        } else if (BLE_TBT_IsConnected()) {
            lv_label_set_text(s_tbtlink_value, "TBT: phone connected");
        } else {
            lv_label_set_text_fmt(s_tbtlink_value, "TBT: %s, advertising %s%s",
                                  BLE_TBT_StartResultText(),
                                  BLE_TBT_IsAdvertising() ? "YES" : "NO",
                                  BLE_TBT_RestartCount() > 0 ? " (restarted)" : "");
        }
    }

    RefreshPowerStatus();

    // Reconciled every second rather than only on a press, because the ride
    // can end without one: the hour-without-movement timeout disarms in the
    // writer task, and nothing would otherwise bring "finish" back down and
    // "new ride" back up.
    RefreshRideButtons(RideLog_IsArmed());

    if (s_ridelog_value != nullptr) {
        if (RideLog_IsRecording()) {
            lv_label_set_text_fmt(s_ridelog_value, "Ride log: %s (%u pts)", RideLog_FileName(),
                                  (unsigned)RideLog_PointCount());
        } else if (!RideLog_IsArmed()) {
            // Disarmed is not "waiting for fix", and conflating them is how a
            // rider spends a ride hunting for a GPS fault that was never
            // there. Says which of the two ways it got here, because "I
            // pressed finish" and "it gave up while I was at lunch" call for
            // different reactions.
            if (RideLog_AutoEndCount() > 0) {
                lv_label_set_text_fmt(s_ridelog_value,
                                      "Ride log: off - ended after an hour still (%ux). "
                                      "Start a ride to record again.",
                                      (unsigned)RideLog_AutoEndCount());
            } else {
                lv_label_set_text(s_ridelog_value,
                                  "Ride log: off. Start a ride to record.");
            }
        } else {
            lv_label_set_text(s_ridelog_value, "Ride log: armed, waiting for fix");
        }
    }
}

lv_obj_t *MakeInfoRow(lv_obj_t *card, const char *label, const char *value) {
    lv_obj_t *row = lv_label_create(card);
    lv_label_set_text_fmt(row, "%s: %s", label, value);
    lv_obj_set_style_text_font(row, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(row, lv_color_hex(COLOR_VALUE), 0);
    lv_label_set_long_mode(row, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(row, LV_PCT(100));
    return row;
}

} // namespace

void PageSettings::onViewLoad() {
    lv_obj_t *parent = _root;
    lv_obj_set_style_bg_color(parent, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Header ----
    // Swipe left to leave. The handler has to be here AND the flag cleared,
    // because LVGL delivers a gesture by walking up from the object under the
    // finger for as long as each ancestor has LV_OBJ_FLAG_GESTURE_BUBBLE --
    // and every object created with a parent has it. Left alone the walk runs
    // past this page to the screen and the event is sent there, so a handler
    // attached here is never called. The dashboard learned this the same way.
    lv_obj_add_event_cb(parent, OnSettingsGesture, LV_EVENT_GESTURE, this);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *back_btn = lv_btn_create(parent);
    lv_obj_set_size(back_btn, 40, 32);
    lv_obj_set_ext_click_area(back_btn, 10);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, OnBackClicked, LV_EVENT_CLICKED, this);

    lv_obj_t *back_icon = lv_label_create(back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_icon, lv_color_hex(COLOR_VALUE), 0);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_CAPTION), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    // ---- Scrollable body ----
    // 240x320 cannot show four groups at once, so the body scrolls. Touch is
    // the only input, and LVGL handles the drag-to-scroll itself.
    lv_obj_t *body = lv_obj_create(parent);
    lv_obj_set_size(body, LV_PCT(100), 320 - 46);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 8, 0);
    lv_obj_set_style_pad_row(body, 10, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    // Vertical only, and it is not merely tidiness: LVGL suppresses a gesture
    // for as long as something is scrolling, and a scrollable defaults to
    // accepting drags in every direction. Left as LV_DIR_ALL, a sideways drag
    // would start a horizontal scroll that goes nowhere -- the content is no
    // wider than the page -- and swallow the swipe that leaves.
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    // ---- Why we last restarted ----
    // First card, and only when there is bad news. A board in a reboot loop
    // gives you a few seconds of UI per boot, so the one thing worth reading
    // has to be at the top of the scroll rather than at the bottom under the
    // chip details. It disappears once a boot ends cleanly, so it is never
    // clutter on a healthy device.
    if (Settings_LastResetWasAbnormal()) {
        lv_obj_t *reset_card = MakeCard(body, "LAST RESTART");

        lv_obj_t *reason = lv_label_create(reset_card);
        lv_label_set_text(reason, Settings_LastResetText());
        lv_obj_set_style_text_font(reason, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(reason, lv_color_hex(COLOR_DANGER), 0);

        lv_obj_t *count = lv_label_create(reset_card);
        lv_label_set_text_fmt(count, "Boot %u since last power-on",
                              (unsigned)Settings_BootCount());
        lv_obj_set_style_text_font(count, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(count, lv_color_hex(COLOR_CAPTION), 0);
        lv_label_set_long_mode(count, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(count, LV_PCT(100));

        lv_obj_t *hint = lv_label_create(reset_card);
        lv_label_set_text(hint,
                          "Panic is a crash in the firmware. A watchdog means "
                          "something blocked. Brownout is the power supply, "
                          "not the code. Unplug to reset the count.");
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_CAPTION), 0);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, LV_PCT(100));
    }

    // ---- The ride ----
    // First on the page, at the rider's asking. It is the only card
    // holding controls that are touched on every single ride -- start
    // and finish -- where everything below it is set once and left.
    lv_obj_t *trip_card = MakeCard(body, "RIDE");
    // ---- Start and finish, side by side and square ----
    //
    // 98px each. The card's inside is 204 wide -- 240 of panel, less the
    // body's 8 a side, less the card's own 10 a side -- so two squares and an
    // 8px gap is 98 + 8 + 98. The height follows from the width because they
    // are square, not because 98 was wanted vertically.
    //
    // They were stacked and full-width before, on the reasoning that two
    // buttons sharing a row are two a gloved thumb cannot tell apart. Square
    // and side by side answers that better than stacking did: 98px is a far
    // larger target than the 40-odd a full-width row gave, and left-vs-right
    // is a distinction a thumb makes without looking, where upper-vs-lower on
    // two identical bars is not.
    //
    // Colour still carries which is which -- green starts, blue finishes --
    // and neither is a warning colour. Finishing a ride is the ordinary end of
    // one, done as often as starting, and nothing it does is destructive: the
    // file is closed complete and the summary is shown.
    constexpr lv_coord_t RIDE_BTN = 98;

    lv_obj_t *ride_row = lv_obj_create(trip_card);
    lv_obj_remove_style_all(ride_row);
    lv_obj_set_width(ride_row, LV_PCT(100));
    lv_obj_set_height(ride_row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(ride_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ride_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ride_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *reset_btn = lv_btn_create(ride_row);
    s_ride_start_btn = reset_btn;
    lv_obj_set_size(reset_btn, RIDE_BTN, RIDE_BTN);
    // Green rather than the red it wore as "Reset trip distance". The gesture
    // is now something the rider does at the start of every ride, and a
    // warning colour on a routine action is a warning nobody reads.
    lv_obj_set_style_bg_color(reset_btn, lv_color_hex(0x16281E), 0);
    lv_obj_set_style_bg_color(reset_btn, lv_color_hex(COLOR_OK), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(reset_btn, 0, 0);
    lv_obj_set_style_pad_all(reset_btn, 0, 0);
    // Icon over word rather than beside it: a square is the one shape where
    // stacking them costs nothing, and it lets the glyph be large enough to
    // recognise before the word is read.
    lv_obj_set_flex_flow(reset_btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(reset_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(reset_btn, 6, 0);
    lv_obj_add_event_cb(reset_btn, OnStartNewRideClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *reset_icon = lv_label_create(reset_btn);
    s_ride_start_icon = reset_icon;
    lv_label_set_text(reset_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(reset_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(reset_icon, lv_color_hex(COLOR_OK), 0);

    lv_obj_t *reset_label = lv_label_create(reset_btn);
    s_ride_start_label = reset_label;
    lv_label_set_text(reset_label, "New ride");
    lv_obj_set_style_text_font(reset_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(reset_label, lv_color_hex(COLOR_OK), 0);

    lv_obj_t *finish_btn = lv_btn_create(ride_row);
    s_ride_finish_btn = finish_btn;
    lv_obj_set_size(finish_btn, RIDE_BTN, RIDE_BTN);
    lv_obj_set_style_bg_color(finish_btn, lv_color_hex(0x14242E), 0);
    lv_obj_set_style_bg_color(finish_btn, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(finish_btn, 0, 0);
    lv_obj_set_style_pad_all(finish_btn, 0, 0);
    lv_obj_set_flex_flow(finish_btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(finish_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(finish_btn, 6, 0);
    lv_obj_add_event_cb(finish_btn, OnFinishRideClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *finish_icon = lv_label_create(finish_btn);
    s_ride_finish_icon = finish_icon;
    lv_label_set_text(finish_icon, LV_SYMBOL_STOP);
    lv_obj_set_style_text_font(finish_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(finish_icon, lv_color_hex(COLOR_ACCENT), 0);

    lv_obj_t *finish_label = lv_label_create(finish_btn);
    s_ride_finish_label = finish_label;
    lv_label_set_text(finish_label, "Finish");
    lv_obj_set_style_text_font(finish_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(finish_label, lv_color_hex(COLOR_ACCENT), 0);

    // Before the first frame, so the pair is never drawn both-live for a tick.
    RefreshRideButtons(RideLog_IsArmed());

    lv_obj_t *summary_btn = lv_btn_create(trip_card);
    lv_obj_set_width(summary_btn, LV_PCT(100));
    lv_obj_set_style_bg_color(summary_btn, lv_color_hex(0x14242E), 0);
    lv_obj_set_style_bg_color(summary_btn, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(summary_btn, 0, 0);
    lv_obj_add_event_cb(summary_btn, OnRideSummaryClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *summary_label = lv_label_create(summary_btn);
    lv_label_set_text(summary_label, LV_SYMBOL_LIST "  Ride summary");
    lv_obj_set_style_text_font(summary_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(summary_label, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_center(summary_label);

    s_trip_status = lv_label_create(trip_card);
    lv_label_set_text(s_trip_status,
                      "New ride clears the odometer and the averages; the next fix "
                      "opens a file on the card.\n"
                      "Finish closes it and stops recording until you start another. "
                      "An hour without moving does the same.");
    lv_obj_set_style_text_font(s_trip_status, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_trip_status, lv_color_hex(COLOR_CAPTION), 0);
    lv_label_set_long_mode(s_trip_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_trip_status, LV_PCT(100));

    // ---- Heart rate ----
    // No source to choose any more: BLE is the only one. The card stays
    // because it is where the link status and the restart live, and it is
    // where someone looks when the reading is missing.
    lv_obj_t *hr_card = MakeCard(body, "HEART RATE");

    lv_obj_t *hr_hint = lv_label_create(hr_card);
    lv_label_set_text(hr_hint, "A watch or strap broadcasting 0x180D.");
    lv_obj_set_style_text_font(hr_hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(hr_hint, lv_color_hex(COLOR_CAPTION), 0);
    lv_label_set_long_mode(hr_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hr_hint, LV_PCT(100));

    // Live link state, in the card about the heart-rate source rather than
    // buried at the bottom of DEVICE -- this is where someone looks when the
    // reading is missing, and it was several screens of scrolling away.
    //
    // It earns its place because the dashboard's HEART RATE cell shows "--"
    // both when nothing is connected and when a connected peer has not sent a
    // measurement yet, and serial cannot tell them apart on this board.
    lv_obj_t *shutdown_row = lv_label_create(hr_card);
    lv_obj_set_style_text_font(shutdown_row, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(shutdown_row, lv_color_hex(COLOR_CAPTION), 0);
    lv_label_set_text_fmt(shutdown_row, "Last disconnect on restart: %s\nSupervisor: %s",
                          BLE_HR_LastShutdownText(), BLE_HR_LastParkText());
    lv_label_set_long_mode(shutdown_row, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(shutdown_row, LV_PCT(100));

    s_hrlink_value = lv_label_create(hr_card);
    lv_obj_set_style_text_font(s_hrlink_value, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_hrlink_value, lv_color_hex(COLOR_VALUE), 0);
    lv_label_set_text(s_hrlink_value, "Link: --");

    // Sits in the heart-rate card because that is where the radio status
    // already lives, and both links share the one NimBLE stack.
    s_tbtlink_value = lv_label_create(hr_card);
    lv_obj_set_style_text_font(s_tbtlink_value, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_tbtlink_value, lv_color_hex(COLOR_VALUE), 0);
    lv_label_set_long_mode(s_tbtlink_value, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_tbtlink_value, LV_PCT(100));
    lv_label_set_text(s_tbtlink_value, "TBT: --");

    // ---- Heart-rate zones ----
    // Unlike the source above, these take effect immediately: they are only
    // arithmetic applied to a reading, with no radio to reconfigure. The
    // dashboard is cached rather than rebuilt when this page closes, but it
    // does not need rebuilding -- the bar's proportions come from the fixed
    // reserve fractions, and the zone lookup re-reads these numbers for every
    // reading. The next beat lands in the new bands.
    lv_obj_t *zone_card = MakeCard(body, "HEART RATE ZONES");

    struct {
        const char *caption;
        lv_obj_t **value;
        lv_event_cb_t handler;
    } const rows[] = {
        {"Resting", &s_hr_rest_value, OnHrRestStep},
        {"Maximum", &s_hr_max_value, OnHrMaxStep},
    };

    for (const auto &row : rows) {
        lv_obj_t *line = lv_obj_create(zone_card);
        lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_set_style_pad_all(line, 0, 0);
        lv_obj_set_style_pad_column(line, 6, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t *caption = lv_label_create(line);
        lv_label_set_text(caption, row.caption);
        lv_obj_set_style_text_font(caption, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(caption, lv_color_hex(COLOR_CAPTION), 0);
        lv_obj_set_flex_grow(caption, 1);

        *row.value = lv_label_create(line);
        lv_obj_set_style_text_font(*row.value, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(*row.value, lv_color_hex(COLOR_VALUE), 0);
        lv_label_set_text(*row.value, "--"); // filled by RefreshHrZoneCard below

        // Wide enough to hit on a bouncing bike, which is the only input this
        // device has.
        static const char *const STEP_LABELS[2] = {LV_SYMBOL_MINUS, LV_SYMBOL_PLUS};
        const int steps[2] = {-HR_BPM_STEP, HR_BPM_STEP};
        for (int i = 0; i < 2; i++) {
            lv_obj_t *btn = lv_btn_create(line);
            lv_obj_set_size(btn, 40, 34);
            lv_obj_set_style_radius(btn, 8, 0);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x24313D), 0);
            lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_add_event_cb(btn, row.handler, LV_EVENT_CLICKED, (void *)(intptr_t)steps[i]);

            lv_obj_t *label = lv_label_create(btn);
            lv_label_set_text(label, STEP_LABELS[i]);
            lv_obj_center(label);
        }
    }

    lv_obj_t *zone_hint = lv_label_create(zone_card);
    lv_label_set_text(zone_hint, "Bands by heart-rate reserve, matching the phone:");
    lv_obj_set_style_text_font(zone_hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(zone_hint, lv_color_hex(COLOR_CAPTION), 0);
    lv_label_set_long_mode(zone_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(zone_hint, LV_PCT(100));

    s_hr_zone_table = lv_label_create(zone_card);
    lv_obj_set_style_text_font(s_hr_zone_table, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_hr_zone_table, lv_color_hex(COLOR_VALUE), 0);

    RefreshHrZoneCard();

    // ---- Brightness ----
    lv_obj_t *bright_card = MakeCard(body, "DISPLAY BRIGHTNESS");
    s_brightness_value = lv_label_create(bright_card);
    lv_label_set_text_fmt(s_brightness_value, "%d%%", (int)Settings_GetBrightness());
    lv_obj_set_style_text_font(s_brightness_value, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_brightness_value, lv_color_hex(COLOR_VALUE), 0);

    lv_obj_t *slider = lv_slider_create(bright_card);
    lv_obj_set_width(slider, LV_PCT(100));
    // Floor of 5%: a 0% backlight looks identical to a crashed board, and
    // recovering would mean navigating a screen you cannot see.
    lv_slider_set_range(slider, 5, 100);
    lv_slider_set_value(slider, Settings_GetBrightness(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COLOR_ACCENT), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, OnBrightnessChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    // ---- Units ----
    lv_obj_t *unit_card = MakeCard(body, "UNITS");
    s_unit_value = lv_label_create(unit_card);
    lv_label_set_text(s_unit_value, Settings_SpeedUnitLabel());
    lv_obj_set_style_text_font(s_unit_value, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_unit_value, lv_color_hex(COLOR_VALUE), 0);

    lv_obj_t *unit_hint = lv_label_create(unit_card);
    lv_label_set_text(unit_hint, "Off: km/h + km    On: mph + mi");
    lv_obj_set_style_text_font(unit_hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(unit_hint, lv_color_hex(COLOR_CAPTION), 0);

    lv_obj_t *unit_sw = lv_switch_create(unit_card);
    lv_obj_set_style_bg_color(unit_sw, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (Settings_GetSpeedUnit() == SPEED_UNIT_MPH) {
        lv_obj_add_state(unit_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(unit_sw, OnUnitToggled, LV_EVENT_VALUE_CHANGED, nullptr);

    // ---- Navigation ----
    // Coupled to the heart-rate source by the one exclusivity rule in
    // Settings.h; both selectors repair the other and say what moved.
    s_nav_mode_at_load = Settings_GetNavMode();

    lv_obj_t *nav_card = MakeCard(body, "NAVIGATION");

    lv_obj_t *nav_row = lv_obj_create(nav_card);
    lv_obj_set_size(nav_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(nav_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav_row, 0, 0);
    lv_obj_set_style_pad_all(nav_row, 0, 0);
    lv_obj_set_style_pad_column(nav_row, 6, 0);
    lv_obj_clear_flag(nav_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(nav_row, LV_FLEX_FLOW_ROW);

    static const char *const NAV_LABELS[NAV_MODE_COUNT] = {"TBT", "GPX"};
    for (int i = 0; i < NAV_MODE_COUNT; i++) {
        lv_obj_t *btn = lv_btn_create(nav_row);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, 34);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, OnNavModeClicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, NAV_LABELS[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_center(label);

        s_nav_btns[i] = btn;
    }

    lv_obj_t *orient_row = lv_obj_create(nav_card);
    lv_obj_set_size(orient_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(orient_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(orient_row, 0, 0);
    lv_obj_set_style_pad_all(orient_row, 0, 0);
    lv_obj_clear_flag(orient_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(orient_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(orient_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *orient_label = lv_label_create(orient_row);
    lv_label_set_text(orient_label, "Map turns with you");
    lv_obj_set_style_text_font(orient_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(orient_label, lv_color_hex(COLOR_VALUE), 0);

    lv_obj_t *orient_sw = lv_switch_create(orient_row);
    if (Settings_GetMapTrackUp()) {
        lv_obj_add_state(orient_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(orient_sw, OnTrackUpToggled, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *orient_hint = lv_label_create(nav_card);
    lv_label_set_text(orient_hint,
                      "Track-up: the road ahead is at the top, so the screen matches "
                      "what you see. Off is north-up.\n"
                      "Falls back to north-up below walking pace, where the heading is "
                      "the receiver's own noise.");
    lv_obj_set_style_text_font(orient_hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(orient_hint, lv_color_hex(COLOR_CAPTION), 0);
    lv_label_set_long_mode(orient_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(orient_hint, LV_PCT(100));

    lv_obj_t *nav_hint = lv_label_create(nav_card);
    lv_label_set_text(nav_hint,
                      "TBT: turn prompts pushed from the phone over BLE.\n"
                      "GPX: offline breadcrumb from a .gpx on the SD card.");
    lv_obj_set_style_text_font(nav_hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(nav_hint, lv_color_hex(COLOR_CAPTION), 0);
    lv_label_set_long_mode(nav_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(nav_hint, LV_PCT(100));

    s_nav_note = lv_label_create(nav_card);
    lv_obj_set_style_text_font(s_nav_note, &lv_font_montserrat_10, 0);
    lv_label_set_long_mode(s_nav_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_nav_note, LV_PCT(100));

    // Directly beneath the note that asks for it. Both radio modes are
    // configured once at init (CLAUDE.md section 3), so a mode change is saved
    // immediately and applied never, until this is pressed -- which made the
    // setting look broken when the only button that could finish it was in
    // another card several screens away.
    s_nav_restart_btn = MakeRestartButton(nav_card, LV_SYMBOL_POWER "  Restart to apply");

    RefreshNavSelection();

    // ---- Power ----
    // The two stages that always happen are described rather than offered:
    // dimming and blanking carry no risk, and nothing is gained by letting the
    // rider switch off the single biggest saving on the board. Only the last
    // step is a choice for a different reason: it works, but a sleeping board
    // cannot be flashed until something wakes it.
    lv_obj_t *power_card = MakeCard(body, "POWER");

    lv_obj_t *power_row = lv_obj_create(power_card);
    lv_obj_set_size(power_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(power_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(power_row, 0, 0);
    lv_obj_set_style_pad_all(power_row, 0, 0);
    lv_obj_clear_flag(power_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(power_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(power_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *sleep_label = lv_label_create(power_row);
    lv_label_set_text(sleep_label, "Deep sleep when idle");
    lv_obj_set_style_text_font(sleep_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sleep_label, lv_color_hex(COLOR_VALUE), 0);

    lv_obj_t *sleep_sw = lv_switch_create(power_row);
    if (Settings_GetSleepEnabled()) {
        lv_obj_add_state(sleep_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sleep_sw, OnSleepToggled, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *power_hint = lv_label_create(power_card);
    lv_label_set_text(power_hint,
                      "Screen dims after 1 min and goes dark after 3, always.\n"
                      "Deep sleep follows once a ride has finished, and only a touch "
                      "wakes it. Tested: the board slept overnight and a touch brought "
                      "it back.\n"
                      "Never sleeps mid-ride or during file transfer. Waits 30 min "
                      "instead of 5 if no ride has been recorded.");
    lv_obj_set_style_text_font(power_hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(power_hint, lv_color_hex(COLOR_CAPTION), 0);
    lv_label_set_long_mode(power_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(power_hint, LV_PCT(100));

    s_power_battery = lv_label_create(power_card);
    lv_obj_set_style_text_font(s_power_battery, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_power_battery, lv_color_hex(COLOR_CAPTION), 0);
    lv_label_set_long_mode(s_power_battery, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_power_battery, LV_PCT(100));

    s_power_status = lv_label_create(power_card);
    lv_obj_set_style_text_font(s_power_status, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_power_status, lv_color_hex(COLOR_ACCENT), 0);
    lv_label_set_long_mode(s_power_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_power_status, LV_PCT(100));
    RefreshPowerStatus();

    // ---- WiFi file transfer ----
    // In the ride-log neighbourhood because rides are what most people come
    // to fetch, even though it also carries routes and maps in the other
    // direction.
    lv_obj_t *wifi_card = MakeCard(body, "FILE TRANSFER");
    lv_obj_t *wifi_btn = lv_btn_create(wifi_card);
    lv_obj_set_width(wifi_btn, LV_PCT(100));
    lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0x14242E), 0);
    lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(wifi_btn, 0, 0);
    lv_obj_add_event_cb(wifi_btn, OnFileTransferClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *wifi_label = lv_label_create(wifi_btn);
    lv_label_set_text(wifi_label, LV_SYMBOL_WIFI "  Start WiFi file transfer");
    lv_obj_set_style_text_font(wifi_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wifi_label, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_center(wifi_label);

    lv_obj_t *wifi_hint = lv_label_create(wifi_card);
    lv_label_set_text(wifi_hint,
                      "Serves rides, routes and maps to a phone or laptop over WiFi.\n"
                      "Takes the radio from Bluetooth, so it ends with a restart.");
    lv_obj_set_style_text_font(wifi_hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(wifi_hint, lv_color_hex(COLOR_CAPTION), 0);
    lv_label_set_long_mode(wifi_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(wifi_hint, LV_PCT(100));

    // ---- Device info ----
    // Read-only diagnostics. Worth more than usual on this board: serial is
    // unusable over USB-Serial-JTAG here, so the panel is the only place these
    // numbers can be seen.
    lv_obj_t *info_card = MakeCard(body, "DEVICE");
    char buf[64];

    snprintf(buf, sizeof(buf), "%s rev%d, %d core(s) @ %luMHz", ESP.getChipModel(),
             (int)ESP.getChipRevision(), (int)ESP.getChipCores(),
             (unsigned long)ESP.getCpuFreqMHz());
    MakeInfoRow(info_card, "Chip", buf);

    snprintf(buf, sizeof(buf), "%u MB", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
    MakeInfoRow(info_card, "Flash", buf);

    snprintf(buf, sizeof(buf), "%u MB", (unsigned)(ESP.getPsramSize() / (1024 * 1024)));
    MakeInfoRow(info_card, "PSRAM", buf);

    MakeInfoRow(info_card, "MAC", WiFi.macAddress().c_str());

    // Also here, unconditionally, so a normal reason can be checked on a
    // healthy board without waiting for it to misbehave.
    snprintf(buf, sizeof(buf), "%s (boot %u)", Settings_LastResetText(),
             (unsigned)Settings_BootCount());
    MakeInfoRow(info_card, "Last reset", buf);
    MakeInfoRow(info_card, "Build", __DATE__ " " __TIME__);

    // What the road layer cost on its last draw.
    //
    // This used to live on the map page and was taken off it, rightly: a rider
    // does not need a segment count. It belongs here, where the other numbers
    // that only matter when something is wrong already are -- and "the board
    // is slow with this map" cannot be answered without it, because serial
    // cannot be opened on this board.
    if (RoadMap_IsLoaded()) {
        snprintf(buf, sizeof(buf), "%u/%u ways, %u seg, %ums",
                 (unsigned)RoadView_LastVisibleWays(), (unsigned)RoadMap_WayCount(),
                 (unsigned)RoadView_LastSegments(),
                 (unsigned)(RoadView_LastDrawOnlyUs() / 1000));
        MakeInfoRow(info_card, "Roads", buf);
    }

    // The clock, and where it came from. "Blank clock" has three causes that
    // look identical on the dashboard and need opposite fixes: the phone never
    // wrote, it wrote something the parser refused, or it wrote hours ago and
    // the reading has aged out. This row separates them, and it is the only
    // way to do so on a board whose serial port cannot be opened.
    {
        TimeReading_t reading;
        const unsigned ok = (unsigned)TimeSource_PhoneAccepted();
        const unsigned bad = (unsigned)TimeSource_PhoneRejected();
        if (TimeSource_Now(millis(), &reading)) {
            const char *src = reading.kind == TIME_SRC_GNSS ? "GNSS" : "phone";
            snprintf(buf, sizeof(buf), "%s, %+d min (%u ok, %u bad)", src,
                     (int)reading.offset_min, ok, bad);
        } else if (ok > 0 || bad > 0) {
            snprintf(buf, sizeof(buf), "stale (%u ok, %u bad)", ok, bad);
        } else {
            snprintf(buf, sizeof(buf), "never set (no writes)");
        }
        MakeInfoRow(info_card, "Clock", buf);
    }

    // Ride logging has no controls -- it records whenever a card is present --
    // so this line is the only way to tell whether it is working. Without it a
    // rider would find out at the end of the ride, which is too late.
    // The mount reason, not just its outcome: "no SD card" reads the same for
    // a missing card, a bus that would not train and a filesystem we cannot
    // read, and only one of those is fixed by pushing the card in harder.
    {
        char sd[80];
        if (GpxTrack_CardMounted()) {
            snprintf(sd, sizeof(sd), "%u MB, %d-bit bus",
                     (unsigned)GpxTrack_CardSizeMb(), GpxTrack_BusWidth());
        } else {
            snprintf(sd, sizeof(sd), "%s", GpxTrack_MountStatus());
        }
        MakeInfoRow(info_card, "SD card", sd);
    }

    if (!GpxTrack_CardMounted()) {
        MakeInfoRow(info_card, "Ride log", "no SD card");
    } else if (RideLog_IsRecording()) {
        snprintf(buf, sizeof(buf), "%s (%u pts)", RideLog_FileName(),
                 (unsigned)RideLog_PointCount());
        s_ridelog_value = MakeInfoRow(info_card, "Ride log", buf);
    } else {
        s_ridelog_value = MakeInfoRow(info_card, "Ride log", "waiting for fix");
    }

    s_uptime_value = MakeInfoRow(info_card, "Uptime", "--");
    s_heap_value = MakeInfoRow(info_card, "Memory", "--");

    // The general one, at the very bottom of the page. A restart is a
    // diagnostic on this board rather than a setting -- it is how a stuck
    // radio gets another go, and the reset reason above is how you find out
    // what happened -- so it belongs here with the other diagnostics rather
    // than beside the strap settings, where it used to be.
    MakeRestartButton(info_card, LV_SYMBOL_POWER "  Restart device");

    s_info_timer = lv_timer_create(InfoTimerCallback, 1000, nullptr);
    InfoTimerCallback(nullptr); // populate immediately rather than after 1s
}

void PageSettings::onViewUnload() {
    if (s_info_timer != nullptr) {
        lv_timer_del(s_info_timer);
        s_info_timer = nullptr;
    }

    s_brightness_value = nullptr;
    s_unit_value = nullptr;
    s_trip_status = nullptr;
    s_uptime_value = nullptr;
    s_ridelog_value = nullptr;
    s_power_status = nullptr;
    s_power_battery = nullptr;
    s_ride_start_btn = nullptr;
    s_ride_start_icon = nullptr;
    s_ride_start_label = nullptr;
    s_ride_finish_btn = nullptr;
    s_ride_finish_icon = nullptr;
    s_ride_finish_label = nullptr;
    s_hrlink_value = nullptr;
    s_tbtlink_value = nullptr;
    s_heap_value = nullptr;
    s_hr_rest_value = nullptr;
    s_hr_max_value = nullptr;
    s_hr_zone_table = nullptr;
    s_nav_note = nullptr;
    s_nav_restart_btn = nullptr;
    for (int i = 0; i < NAV_MODE_COUNT; i++) {
        s_nav_btns[i] = nullptr;
    }
}
