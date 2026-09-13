#include "Overlay_FileTransfer.h"

#include <lvgl.h>

#include "../system/FileServer.h"

namespace {

constexpr uint32_t COLOR_BG = 0x0D1720;
constexpr uint32_t COLOR_CAPTION = 0x93A4B8;
constexpr uint32_t COLOR_VALUE = 0xFFFFFF;
constexpr uint32_t COLOR_ACCENT = 0x61DAFB;
constexpr uint32_t COLOR_OK = 0x7CE38B;
constexpr uint32_t COLOR_DANGER = 0xFF6B6B;

lv_obj_t *s_root = nullptr;
lv_obj_t *s_status = nullptr;
lv_timer_t *s_timer = nullptr;

// Whether the server actually took the radio. It decides what the button at
// the bottom does, and that is not cosmetic: if nothing was taken there is
// nothing to give back, and rebooting anyway would punish the rider for a
// refusal that was not their fault -- a missing card, a ride in progress, or
// a firmware built without the feature.
bool s_took_radio = false;

lv_obj_t *MakeCaption(lv_obj_t *parent, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_CAPTION), 0);
    return label;
}

lv_obj_t *MakeValue(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    return label;
}

void Dismiss() {
    if (s_timer != nullptr) {
        lv_timer_del(s_timer);
        s_timer = nullptr;
    }
    if (s_root != nullptr) {
        // Async: Dismiss() is reached from a button inside this object, and
        // LVGL uses the event target after the handler returns. Deleting an
        // ancestor synchronously there frees the ground it is standing on.
        lv_obj_del_async(s_root);
        s_root = nullptr;
    }
    s_status = nullptr;
}

void OnStopClicked(lv_event_t *e) {
    (void)e;
    if (!s_took_radio) {
        // Never started, so the Bluetooth stack is untouched and the card
        // still has its usual readers. Just go back.
        Dismiss();
        return;
    }
    // Does not return. The timer and the overlay go with the reboot, so there
    // is nothing to tear down first.
    FileServer_StopAndRestart();
}

void RefreshStatus(lv_timer_t *timer) {
    (void)timer;
    if (s_status == nullptr) {
        return;
    }

    const int clients = FileServer_ClientCount();
    lv_label_set_text_fmt(s_status, "%d device%s joined, %u requests\n%s", clients,
                          (clients == 1) ? "" : "s", (unsigned)FileServer_RequestCount(),
                          FileServer_StatusText());
    lv_obj_set_style_text_color(s_status, lv_color_hex(clients > 0 ? COLOR_OK : COLOR_CAPTION), 0);
}

lv_obj_t *MakeOverlay() {
    // The top layer sits above every page and takes input first, which is the
    // whole reason this is not a page.
    lv_obj_t *root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 12, 0);
    lv_obj_set_style_pad_row(root, 6, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    return root;
}

} // namespace

void Overlay_FileTransfer_Show() {
    if (s_root != nullptr) {
        return;
    }

    s_took_radio = FileServer_Start();
    const bool started = s_took_radio;
    s_root = MakeOverlay();

    lv_obj_t *title = MakeValue(s_root, LV_SYMBOL_WIFI "  FILE TRANSFER", &lv_font_montserrat_14,
                                started ? COLOR_ACCENT : COLOR_DANGER);
    lv_obj_set_style_text_letter_space(title, 1, 0);

    if (!started) {
        MakeValue(s_root, FileServer_StatusText(), &lv_font_montserrat_12, COLOR_DANGER);
        MakeValue(s_root, "Nothing was changed. Bluetooth and the card are as they were.",
                  &lv_font_montserrat_10, COLOR_CAPTION);
    } else {
        MakeCaption(s_root, "JOIN THIS NETWORK");
        MakeValue(s_root, FileServer_Ssid(), &lv_font_montserrat_18, COLOR_VALUE);

        MakeCaption(s_root, "PASSWORD");
        // The largest text on the screen, because it is the one thing here
        // that has to be read off the panel and typed into a phone. It is
        // new every session, so there is nothing to memorise.
        MakeValue(s_root, FileServer_Password(), &lv_font_montserrat_24, COLOR_VALUE);

        MakeCaption(s_root, "THEN OPEN");
        MakeValue(s_root, FileServer_Url(), &lv_font_montserrat_18, COLOR_ACCENT);

        s_status = MakeValue(s_root, "", &lv_font_montserrat_10, COLOR_CAPTION);
        s_timer = lv_timer_create(RefreshStatus, 1000, nullptr);
        RefreshStatus(nullptr);
    }

    // Bluetooth is down and the card has one reader, so there is no way back
    // that does not go through a restart. Say that on the button rather than
    // let it read as a cancel.
    lv_obj_t *stop = lv_btn_create(s_root);
    lv_obj_set_width(stop, LV_PCT(100));
    lv_obj_set_height(stop, 40);
    lv_obj_set_style_bg_color(stop, lv_color_hex(0x2A1F26), 0);
    lv_obj_set_style_bg_color(stop, lv_color_hex(COLOR_DANGER), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(stop, 0, 0);
    lv_obj_add_event_cb(stop, OnStopClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *stop_label = lv_label_create(stop);
    lv_label_set_text(stop_label,
                      started ? (LV_SYMBOL_POWER "  Stop and restart")
                              : (LV_SYMBOL_CLOSE "  Back"));
    lv_obj_set_style_text_font(stop_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(stop_label, lv_color_hex(COLOR_DANGER), 0);
    lv_obj_center(stop_label);
}
