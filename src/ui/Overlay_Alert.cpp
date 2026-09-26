#include "Overlay_Alert.h"

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "CjkFont.h"
#include "../system/AlertFrame.h"
#include "../system/DataCenter.h"
#include "../system/PowerManager.h"

namespace {

// Page_Dashboard's palette. Repeated rather than shared because this draws
// over that layout and has to match it, and a header exporting the colours
// would make every page depend on the dashboard's private styling.
constexpr uint32_t COLOR_CARD = 0x18222C;
constexpr uint32_t COLOR_CAPTION = 0x93A4B8;
constexpr uint32_t COLOR_VALUE = 0xFFFFFF;

// One colour per kind, and they are ranked by how much the rider is being
// asked to care. Red is the one worth stopping for.
constexpr uint32_t COLOR_CALL = 0xFF6B6B; // COLOR_DANGER
constexpr uint32_t COLOR_SMS = 0xFFD166;  // COLOR_WARN
constexpr uint32_t COLOR_CHAT = 0x2FC6B7; // COLOR_CELL_SPEED

// ---- Geometry: the metric band, and not one pixel above it ----
// Page_Dashboard puts its navigation region at y 0..184 and its metric cells
// at 184..304, with the heart-rate zone strip below. These are the cells.
//
// Hard-coded rather than derived, because the dashboard computes its layout
// inside onViewLoad() and exports nothing. If that layout moves, this moves
// with it -- the comment in Page_Dashboard's layout block says so.
constexpr lv_coord_t BANNER_Y = 184;
constexpr lv_coord_t BANNER_H = 120;
constexpr lv_coord_t ACCENT_W = 6;
constexpr lv_coord_t PAD = 12;

// ---- How long it stays ----
// A call is ringing while this is up, so the banner should last about as long
// as the ring does -- it is answering "should I stop?", and the answer expires
// when the phone stops. A message is not urgent by construction, so it gets
// long enough to read a name and no longer.
constexpr uint32_t SHOW_CALL_MS = 20000;
constexpr uint32_t SHOW_MESSAGE_MS = 6000;

// Cheap: a mutex and a ~56-byte copy. Four times a second is well under any
// perceptible delay on something the rider is not waiting for.
constexpr uint32_t POLL_MS = 250;

lv_obj_t *s_root = nullptr;
lv_timer_t *s_poll = nullptr;
lv_timer_t *s_expiry = nullptr;

// The last alert actually put on screen. DataCenter_Pull keeps handing back
// the most recent publish forever, so this is the only thing separating a new
// alert from one already shown and dismissed.
uint32_t s_shown_seq = 0;

// True if any byte is outside ASCII, which here means the name needs the one
// font that can draw it. Checked on bytes rather than decoded codepoints
// because that is the whole question -- Montserrat has 0x20-0x7F and nothing
// else, so the first byte over 0x7F settles it.
bool NeedsCjkFace(const char *name) {
    for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; p++) {
        if (*p > 0x7F) {
            return true;
        }
    }
    return false;
}

// Trims `text` until it fits `max_w` x `max_h` in `font`, ending it with an
// ellipsis if anything was removed. `text` must have room for three more
// bytes than the longest name.
//
// The alternative was to reason about it: so many pixels per character, so
// many characters per line, two lines. That works until a name mixes a wide
// hanzi with narrow Latin, or the column changes width, and the failure is a
// name whose bottom half is sliced off by the edge of the banner -- which
// reads as a broken screen rather than as a long name. Asking LVGL to measure
// the real string in the real font is exact, and costs microseconds on a path
// that runs when the phone rings.
void FitToBox(char *text, const lv_font_t *font, lv_coord_t max_w, lv_coord_t max_h) {
    lv_point_t size;
    lv_txt_get_size(&size, text, font, 0, 0, max_w, LV_TEXT_FLAG_NONE);
    if (size.y <= max_h) {
        return;
    }

    char original[ALERT_NAME_MAX + 1];
    strncpy(original, text, sizeof(original) - 1);
    original[sizeof(original) - 1] = '\0';

    size_t len = strlen(original);
    while (len > 0) {
        // Back off one whole UTF-8 character: step over continuation bytes so
        // a multi-byte character is never cut in half, which would leave a
        // stray box on the end of the name.
        do {
            len--;
        } while (len > 0 && ((unsigned char)original[len] & 0xC0) == 0x80);

        // Three dots rather than U+2026: Montserrat is one of the two faces
        // this runs against and does not have the single glyph.
        memcpy(text, original, len);
        memcpy(text + len, "...", 4); // copies the terminator too

        lv_txt_get_size(&size, text, font, 0, 0, max_w, LV_TEXT_FLAG_NONE);
        if (size.y <= max_h) {
            return;
        }
    }
    text[0] = '\0';
}

uint32_t AccentFor(uint8_t kind) {
    switch (kind) {
        case ALERT_KIND_CALL: return COLOR_CALL;
        case ALERT_KIND_SMS:  return COLOR_SMS;
        default:              return COLOR_CHAT;
    }
}

void Dismiss() {
    if (s_expiry != nullptr) {
        lv_timer_del(s_expiry);
        s_expiry = nullptr;
    }
    if (s_root != nullptr) {
        // Async, because one caller is a click handler on this very object and
        // LVGL touches the event target after the handler returns.
        lv_obj_del_async(s_root);
        s_root = nullptr;
    }
}

void OnExpiry(lv_timer_t *timer) {
    (void)timer;
    Dismiss();
}

void OnClicked(lv_event_t *e) {
    (void)e;
    Dismiss();
}

void Show(const Alert_Info_t &alert) {
    // Newest wins outright. Two banners will not fit in this band, and a queue
    // would show the rider something stale -- by the time the second one came
    // up its moment would have passed.
    Dismiss();

    const uint32_t accent = AccentFor(alert.kind);

    s_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_pos(s_root, 0, BANNER_Y);
    lv_obj_set_size(s_root, LV_HOR_RES, BANNER_H);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(COLOR_CARD), 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    // Anywhere on the banner dismisses it. The rider is wearing gloves and
    // looking at the road; a close button would be a target to aim at.
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_root, OnClicked, LV_EVENT_CLICKED, nullptr);

    // A hairline along the top, so the banner reads as sitting on top of the
    // cells rather than as the cells having changed colour.
    // ---- Every child has to be transparent to touch ----
    // lv_obj_create() sets LV_OBJ_FLAG_CLICKABLE, and LVGL does not bubble a
    // click to the parent unless asked. So without this the tap-to-dismiss
    // above works only on the few pixels of bare background -- press the text
    // or the accent bar, which is most of the banner, and nothing happens.
    // Labels already come with the flag cleared; these three do not.
    lv_obj_t *edge = lv_obj_create(s_root);
    lv_obj_remove_style_all(edge);
    lv_obj_clear_flag(edge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(edge, 0, 0);
    lv_obj_set_size(edge, LV_HOR_RES, 1);
    lv_obj_set_style_bg_opa(edge, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(edge, lv_color_hex(accent), 0);

    lv_obj_t *bar = lv_obj_create(s_root);
    lv_obj_remove_style_all(bar);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, ACCENT_W, BANNER_H);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(accent), 0);

    lv_obj_t *column = lv_obj_create(s_root);
    lv_obj_remove_style_all(column);
    lv_obj_set_pos(column, ACCENT_W + PAD, 0);
    lv_obj_set_size(column, LV_HOR_RES - ACCENT_W - (2 * PAD), BANNER_H);
    lv_obj_clear_flag(column, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(column, 4, 0);

    lv_obj_t *kind = lv_label_create(column);
    lv_label_set_text(kind, Alert_KindText((AlertKind_t)alert.kind));
    lv_obj_set_style_text_font(kind, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(kind, lv_color_hex(accent), 0);
    lv_obj_set_style_text_letter_space(kind, 2, 0);

    // ---- Which face can draw this name ----
    // Montserrat for a Latin name, at two sizes -- a short one gets the
    // larger. Anything with a byte over 0x7F has to use the CJK face or it
    // draws nothing at all: that font is the only one in the image with a
    // Chinese glyph in it, which is also why it is not simply used for
    // everything. It costs 750KB, and Montserrat renders Latin better at
    // these sizes.
    //
    // An unnamed caller is the normal case for a number not in the address
    // book, and the banner still has to say a call is happening.
    char name[ALERT_NAME_MAX + 4]; // room for the ellipsis FitToBox may add
    if (alert.name[0] != '\0') {
        strncpy(name, alert.name, ALERT_NAME_MAX);
        name[ALERT_NAME_MAX] = '\0';
    } else {
        strcpy(name, "--");
    }

    const lv_font_t *face;
    if (NeedsCjkFace(name)) {
        face = &pegasus_font_cjk_20;
    } else {
        face = (strlen(name) > 14) ? &lv_font_montserrat_18 : &lv_font_montserrat_24;
    }

    // What is left of the banner once the kind label, the count line and the
    // gaps between them have taken their share. Measured against the real
    // font below rather than assumed to be some number of lines, because the
    // three faces have three different line heights.
    constexpr lv_coord_t NAME_MAX_H = BANNER_H - 56;
    const lv_coord_t name_max_w = LV_HOR_RES - ACCENT_W - (2 * PAD);
    FitToBox(name, face, name_max_w, NAME_MAX_H);

    lv_obj_t *who = lv_label_create(column);
    lv_label_set_text(who, name);
    lv_obj_set_style_text_font(who, face, 0);
    lv_obj_set_style_text_color(who, lv_color_hex(COLOR_VALUE), 0);
    // Wraps to a second line rather than being cut, because the distinguishing
    // part of a group-chat name is often at the end of it.
    lv_label_set_long_mode(who, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(who, LV_PCT(100));

    if (alert.count > 1) {
        lv_obj_t *count = lv_label_create(column);
        char text[24];
        snprintf(text, sizeof(text), "%u messages", (unsigned)alert.count);
        lv_label_set_text(count, text);
        lv_obj_set_style_text_font(count, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(count, lv_color_hex(COLOR_CAPTION), 0);
    }

    const uint32_t hold_ms =
        (alert.kind == ALERT_KIND_CALL) ? SHOW_CALL_MS : SHOW_MESSAGE_MS;
    s_expiry = lv_timer_create(OnExpiry, hold_ms, nullptr);
    lv_timer_set_repeat_count(s_expiry, 1);

    // ---- Only a call is worth the backlight ----
    // Waking the screen costs battery and, on a night ride, night vision. A
    // call is the one alert where the rider's decision is time-limited: the
    // phone stops ringing. A message will still be there at the next stop, so
    // its banner is drawn but the screen is left as the rider left it -- and
    // the expiry above then clears it long before they look again, which is
    // right, because a name with no time attached is not worth showing later.
    if (alert.kind == ALERT_KIND_CALL) {
        PowerManager_NoteActivity();
    }
    // Deliberately NOT extending the idle timer for messages: a phone that
    // chirps every few minutes would otherwise hold the screen on for a whole
    // ride and flatten the battery, without anyone having looked at it.
}

void Poll(lv_timer_t *timer) {
    (void)timer;

    Alert_Info_t alert;
    if (!DataCenter_Pull(TOPIC_PHONE_ALERT, &alert, sizeof(alert))) {
        return; // nothing has ever been published
    }
    if (alert.seq == s_shown_seq) {
        return;
    }
    // Belt and braces against a corrupt or zeroed buffer: seq 0 is the value
    // that means "no alert", and showing one would put an empty banner up.
    if (alert.seq == 0) {
        return;
    }
    s_shown_seq = alert.seq;

    // The name is bounded by the parser, but this copy came off the bus rather
    // than out of the parser, so terminate it here too. It is about to be
    // handed to lv_label_set_text, which reads until it finds a NUL.
    alert.name[ALERT_NAME_MAX] = '\0';

    Show(alert);
}

} // namespace

void Overlay_Alert_Init() {
    if (s_poll != nullptr) {
        return;
    }
    s_poll = lv_timer_create(Poll, POLL_MS, nullptr);
}
