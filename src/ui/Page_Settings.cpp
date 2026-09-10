#include "Page_Settings.h"

#include <Arduino.h>
#include <WiFi.h> // WiFi.macAddress() -- reads the eFused MAC, no radio started

#include "../system/PageManager/PageManager.h"
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

lv_obj_t *s_brightness_value = nullptr;
lv_obj_t *s_unit_value = nullptr;
lv_obj_t *s_trip_status = nullptr;
lv_obj_t *s_uptime_value = nullptr;
lv_obj_t *s_heap_value = nullptr;
lv_timer_t *s_info_timer = nullptr;

// Index i corresponds to HrSource_t value i (BLE=0, ANT=1).
constexpr int HR_SOURCE_COUNT = 2;
lv_obj_t *s_hr_btns[HR_SOURCE_COUNT] = {nullptr, nullptr};
lv_obj_t *s_hr_note = nullptr;
HrSource_t s_hr_source_at_load = HR_SOURCE_BLE;

constexpr int NAV_MODE_COUNT = 2; // index i == NavMode_t value i (TBT=0, GPX=1)
lv_obj_t *s_nav_btns[NAV_MODE_COUNT] = {nullptr, nullptr};
lv_obj_t *s_nav_note = nullptr;
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

void OnResetTripClicked(lv_event_t *e) {
    (void)e;
    Page_Dashboard_ResetTrip();
    lv_label_set_text(s_trip_status, "Trip reset to 0.00");
    lv_obj_set_style_text_color(s_trip_status, lv_color_hex(COLOR_ACCENT), 0);
}

void RefreshHrSelection() {
    const HrSource_t current = Settings_GetHrSource();

    for (int i = 0; i < HR_SOURCE_COUNT; i++) {
        if (s_hr_btns[i] == nullptr) {
            continue;
        }
        const bool active = ((int)current == i);
        lv_obj_set_style_bg_color(s_hr_btns[i],
                                  lv_color_hex(active ? COLOR_ACCENT : 0x24313D), 0);
        lv_obj_t *label = lv_obj_get_child(s_hr_btns[i], 0);
        if (label != nullptr) {
            lv_obj_set_style_text_color(label, lv_color_hex(active ? 0x081015 : COLOR_VALUE), 0);
        }
    }

    // Radio bring-up happens once in setup(); switching modes means
    // re-sequencing the controller, so say plainly that a restart is needed
    // rather than letting the user think it took effect.
    if (s_hr_note != nullptr) {
        if (Settings_DidHrSourceFallBack() && current == s_hr_source_at_load) {
            // Don't silently swallow the recovery -- otherwise the user just
            // sees their choice mysteriously back on BLE.
            lv_label_set_text(s_hr_note,
                              "Previous boot stalled starting the radio, so this reverted to BLE.");
            lv_obj_set_style_text_color(s_hr_note, lv_color_hex(COLOR_DANGER), 0);
        } else if (current == s_hr_source_at_load) {
            lv_label_set_text(s_hr_note, "Active now.");
            lv_obj_set_style_text_color(s_hr_note, lv_color_hex(COLOR_CAPTION), 0);
        } else {
            lv_label_set_text_fmt(s_hr_note, "Saved. Restart to switch from %s to %s.",
                                  Settings_HrSourceLabel(s_hr_source_at_load),
                                  Settings_HrSourceLabel(current));
            lv_obj_set_style_text_color(s_hr_note, lv_color_hex(COLOR_ACCENT), 0);
        }
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

    if (!Settings_NavModeIsImplemented(current)) {
        // Say so rather than let the rider discover an empty ROUTE panel on
        // the road.
        lv_label_set_text(s_nav_note, "GPX breadcrumbs are not built yet: no turn prompts "
                                      "will be shown in this mode.");
        lv_obj_set_style_text_color(s_nav_note, lv_color_hex(COLOR_DANGER), 0);
    } else if (current != s_nav_mode_at_load) {
        lv_label_set_text(s_nav_note, "Saved. Restart to apply.");
        lv_obj_set_style_text_color(s_nav_note, lv_color_hex(COLOR_ACCENT), 0);
    } else {
        lv_label_set_text(s_nav_note, "Active now.");
        lv_obj_set_style_text_color(s_nav_note, lv_color_hex(COLOR_CAPTION), 0);
    }
}

// Both settings share one radio, so changing either can move the other (see
// the exclusivity rule in Settings.h). Report it plainly instead of letting a
// button the user didn't touch change under them.
void NoteCoercion(const char *changed_to) {
    lv_label_set_text_fmt(s_hr_note, "Heart rate switched to %s: turn-by-turn needs the BLE "
                                     "stack, which ANT+ takes over.", changed_to);
    lv_obj_set_style_text_color(s_hr_note, lv_color_hex(COLOR_DANGER), 0);
}

void OnNavModeClicked(lv_event_t *e) {
    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    const HrSource_t hr_before = Settings_GetHrSource();

    Settings_SetNavMode((NavMode_t)index);

    RefreshNavSelection();
    RefreshHrSelection();
    if (Settings_GetHrSource() != hr_before) {
        NoteCoercion(Settings_HrSourceLabel(Settings_GetHrSource()));
    }
}

void OnHrSourceClicked(lv_event_t *e) {
    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    const NavMode_t nav_before = Settings_GetNavMode();

    Settings_SetHrSource((HrSource_t)index);

    RefreshHrSelection();
    RefreshNavSelection();
    if (Settings_GetNavMode() != nav_before) {
        lv_label_set_text_fmt(s_nav_note, "Navigation switched to %s: ANT+ takes the radio "
                                          "turn-by-turn needs.",
                              Settings_NavModeLabel(Settings_GetNavMode()));
        lv_obj_set_style_text_color(s_nav_note, lv_color_hex(COLOR_DANGER), 0);
    }
}

void OnRestartClicked(lv_event_t *e) {
    (void)e;
    ESP.restart();
}

void InfoTimerCallback(lv_timer_t *timer) {
    (void)timer;

    const uint32_t seconds = millis() / 1000UL;
    lv_label_set_text_fmt(s_uptime_value, "%luh %02lum %02lus", (unsigned long)(seconds / 3600UL),
                          (unsigned long)((seconds / 60UL) % 60UL), (unsigned long)(seconds % 60UL));
    lv_label_set_text_fmt(s_heap_value, "%u KB free / PSRAM %u KB free",
                          (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
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
    lv_obj_t *back_btn = lv_btn_create(parent);
    lv_obj_set_size(back_btn, 40, 32);
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

    // ---- Heart-rate source ----
    // ANT+ and BLE are different radio configurations chosen at init, not a
    // live switch: SoftANT_Start(false) takes the BLE controller exclusively,
    // and coexist mode requires NimBLE scanning before ANT opens. So this
    // persists the choice and main.cpp acts on it at the next boot.
    s_hr_source_at_load = Settings_GetHrSource();

    lv_obj_t *hr_card = MakeCard(body, "HEART RATE SOURCE");

    lv_obj_t *hr_row = lv_obj_create(hr_card);
    lv_obj_set_size(hr_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hr_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hr_row, 0, 0);
    lv_obj_set_style_pad_all(hr_row, 0, 0);
    lv_obj_set_style_pad_column(hr_row, 6, 0);
    lv_obj_clear_flag(hr_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hr_row, LV_FLEX_FLOW_ROW);

    static const char *const HR_LABELS[HR_SOURCE_COUNT] = {"BLE", "ANT+"};
    for (int i = 0; i < HR_SOURCE_COUNT; i++) {
        lv_obj_t *btn = lv_btn_create(hr_row);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, 34);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, OnHrSourceClicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, HR_LABELS[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_center(label);

        s_hr_btns[i] = btn;
    }

    lv_obj_t *hr_hint = lv_label_create(hr_card);
    lv_label_set_text(hr_hint,
                      "BLE: watch or strap broadcasting 0x180D.\n"
                      "ANT+: strap on the ESP32's own radio.\n"
                      "One at a time -- they share the same radio.");
    lv_obj_set_style_text_font(hr_hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(hr_hint, lv_color_hex(COLOR_CAPTION), 0);
    lv_label_set_long_mode(hr_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hr_hint, LV_PCT(100));

    s_hr_note = lv_label_create(hr_card);
    lv_obj_set_style_text_font(s_hr_note, &lv_font_montserrat_10, 0);
    lv_label_set_long_mode(s_hr_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_hr_note, LV_PCT(100));

    lv_obj_t *restart_btn = lv_btn_create(hr_card);
    lv_obj_set_width(restart_btn, LV_PCT(100));
    lv_obj_set_style_bg_color(restart_btn, lv_color_hex(0x24313D), 0);
    lv_obj_set_style_bg_color(restart_btn, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(restart_btn, 0, 0);
    lv_obj_add_event_cb(restart_btn, OnRestartClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *restart_label = lv_label_create(restart_btn);
    lv_label_set_text(restart_label, LV_SYMBOL_POWER "  Restart now");
    lv_obj_set_style_text_font(restart_label, &lv_font_montserrat_12, 0);
    lv_obj_center(restart_label);

    RefreshHrSelection();

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

    lv_obj_t *nav_hint = lv_label_create(nav_card);
    lv_label_set_text(nav_hint,
                      "TBT: turn prompts pushed from the phone over BLE.\n"
                      "GPX: offline breadcrumb from a .gpx on the SD card.\n"
                      "TBT needs BLE, so it cannot run alongside ANT+.");
    lv_obj_set_style_text_font(nav_hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(nav_hint, lv_color_hex(COLOR_CAPTION), 0);
    lv_label_set_long_mode(nav_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(nav_hint, LV_PCT(100));

    s_nav_note = lv_label_create(nav_card);
    lv_obj_set_style_text_font(s_nav_note, &lv_font_montserrat_10, 0);
    lv_label_set_long_mode(s_nav_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_nav_note, LV_PCT(100));

    RefreshNavSelection();

    // ---- Reset trip ----
    lv_obj_t *trip_card = MakeCard(body, "TRIP");
    lv_obj_t *reset_btn = lv_btn_create(trip_card);
    lv_obj_set_width(reset_btn, LV_PCT(100));
    lv_obj_set_style_bg_color(reset_btn, lv_color_hex(0x2A1F26), 0);
    lv_obj_set_style_bg_color(reset_btn, lv_color_hex(COLOR_DANGER), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(reset_btn, 0, 0);
    lv_obj_add_event_cb(reset_btn, OnResetTripClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *reset_label = lv_label_create(reset_btn);
    lv_label_set_text(reset_label, LV_SYMBOL_REFRESH "  Reset trip distance");
    lv_obj_set_style_text_font(reset_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(reset_label, lv_color_hex(COLOR_DANGER), 0);
    lv_obj_center(reset_label);

    s_trip_status = lv_label_create(trip_card);
    lv_label_set_text(s_trip_status, "Resets the odometer on the dashboard");
    lv_obj_set_style_text_font(s_trip_status, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_trip_status, lv_color_hex(COLOR_CAPTION), 0);
    lv_label_set_long_mode(s_trip_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_trip_status, LV_PCT(100));

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
    MakeInfoRow(info_card, "Build", __DATE__ " " __TIME__);

    s_uptime_value = MakeInfoRow(info_card, "Uptime", "--");
    s_heap_value = MakeInfoRow(info_card, "Memory", "--");

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
    s_heap_value = nullptr;
    s_hr_note = nullptr;
    s_nav_note = nullptr;
    for (int i = 0; i < HR_SOURCE_COUNT; i++) {
        s_hr_btns[i] = nullptr;
    }
    for (int i = 0; i < NAV_MODE_COUNT; i++) {
        s_nav_btns[i] = nullptr;
    }
}
