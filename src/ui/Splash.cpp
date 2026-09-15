#include "Splash.h"

#include <Arduino.h>

namespace {

lv_obj_t *s_splash = nullptr;
uint32_t s_shown_ms = 0;

} // namespace

void Splash_Show() {
    if (s_splash != nullptr) {
        return;
    }

    // On the top layer, not the active screen. The page manager replaces the
    // screen's contents when it pushes the dashboard, and an image sitting
    // among them would be deleted at a moment nobody chose -- possibly before
    // it had been on screen at all.
    s_splash = lv_img_create(lv_layer_top());
    lv_img_set_src(s_splash, &pegasus_splash);
    lv_obj_align(s_splash, LV_ALIGN_TOP_LEFT, 0, 0);

    // Straight to the panel. The LVGL task has not started, so without this
    // nothing would be drawn until it does -- which is after every slow thing
    // in setup() and therefore after the splash was meant to be over.
    lv_refr_now(nullptr);
    s_shown_ms = millis();
}

void Splash_Dismiss() {
    if (s_splash == nullptr) {
        return;
    }

    // Unsigned, so a tick counter that wrapped between the two calls gives the
    // real elapsed time rather than an enormous one.
    const uint32_t shown_for = millis() - s_shown_ms;
    if (shown_for < SPLASH_MIN_MS) {
        // A blocking wait, and the only one in setup(): nothing else is
        // running yet that it could delay. The radios come up after
        // LvglTask_Start(), and the LVGL task is what this is waiting to hand
        // the screen to.
        delay(SPLASH_MIN_MS - shown_for);
    }

    lv_obj_del(s_splash);
    s_splash = nullptr;
}
