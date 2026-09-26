#include "Overlay_TouchDebug.h"

#if PEGASUS_TOUCH_DEBUG

#include <lvgl.h>
#include <stdio.h>

#include "../hal/Touch.h"

namespace {

constexpr uint32_t COLOR_BG = 0x101820;
constexpr uint32_t COLOR_OK = 0x7CE38B;   // the palette's green
constexpr uint32_t COLOR_WARN = 0xFFD166; // the palette's amber

lv_obj_t *s_label = nullptr;

void Refresh(lv_timer_t *timer) {
    (void)timer;
    if (s_label == nullptr) {
        return;
    }

    uint32_t reads = 0;
    uint32_t presses = 0;
    uint16_t x = 0;
    uint16_t y = 0;
    Touch_DebugCounters(&reads, &presses, &x, &y);

    const bool found = Touch_ControllerFound();

    // `reads` is the one that answers the first question, and it is separate
    // from `found` on purpose: the signature check runs once at init, while
    // reads climb for as long as the bus keeps answering. A found controller
    // with a frozen read count is a bus that died after bring-up, which is a
    // different fault from one that never started.
    lv_label_set_text_fmt(s_label, "CST328 %s  rd%lu  pr%lu  @%d,%d",
                          found ? "ok" : "NOT FOUND", (unsigned long)reads,
                          (unsigned long)presses, (int)x, (int)y);
    lv_obj_set_style_text_color(s_label,
                                lv_color_hex((found && presses > 0) ? COLOR_OK : COLOR_WARN), 0);
}

} // namespace

void TouchDebug_Show() {
    if (s_label != nullptr) {
        return;
    }

    s_label = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_bg_color(s_label, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_label, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(s_label, 3, 0);
    // Bottom, over the heart-rate zone bar: the one strip on the dashboard
    // that carries no number, so this costs the least while it is here.
    lv_obj_align(s_label, LV_ALIGN_BOTTOM_MID, 0, -2);
    // Not clickable -- it sits on the top layer over the whole UI, and a
    // transparent box that answers touches is how the road layer once
    // swallowed every tap meant for the map beneath it.
    lv_obj_clear_flag(s_label, LV_OBJ_FLAG_CLICKABLE);

    Refresh(nullptr);
    lv_timer_create(Refresh, 250, nullptr);
}

#else

void TouchDebug_Show() {}

#endif // PEGASUS_TOUCH_DEBUG
