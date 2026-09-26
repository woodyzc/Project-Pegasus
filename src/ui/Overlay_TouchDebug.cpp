#include "Overlay_TouchDebug.h"

#if PEGASUS_TOUCH_DEBUG

#include <Wire.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "../hal/Touch.h"

namespace {

constexpr uint32_t COLOR_BG = 0x101820;
constexpr uint32_t COLOR_OK = 0x7CE38B;   // the palette's green
constexpr uint32_t COLOR_WARN = 0xFFD166; // the palette's amber

// The other I2C bus on this board (CLAUDE.md §2): the QMI8658 IMU at 0x6B and
// the PCF85063 RTC at 0x51 live here. Nothing drives either yet, which is
// exactly what makes it useful as a control -- if this bus enumerates its two
// known devices and the touch bus enumerates nothing, then the I2C code and
// the Wire library are fine and the fault is specific to the touch bus.
constexpr int SENSOR_I2C_SDA = 11;
constexpr int SENSOR_I2C_SCL = 10;

lv_obj_t *s_label = nullptr;
char s_touch_scan[40] = "?";
char s_sensor_scan[40] = "?";

// Probes every 7-bit address and writes the hits into `out` as hex.
//
// A bare address probe rather than a register read: it answers "is anything
// electrically there" without assuming a register map, which is the question
// when a controller has not answered at all. Runs once -- 112 addresses with
// a failing bus is ~100ms on the LVGL thread, which is a visible hitch and
// not something to repeat on a timer.
void ScanBus(TwoWire &bus, char *out, size_t out_len) {
    out[0] = '\0';
    size_t used = 0;
    int found = 0;

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        bus.beginTransmission(addr);
        if (bus.endTransmission(true) != 0) {
            continue;
        }
        found++;
        if (used + 4 < out_len) {
            used += (size_t)snprintf(out + used, out_len - used, "%s%02X",
                                     used > 0 ? " " : "", addr);
        }
    }

    if (found == 0) {
        snprintf(out, out_len, "none");
    }
}

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
    uint8_t addr = 0;
    uint16_t sig = 0;
    Touch_DebugIdentity(&addr, &sig);

    const char *part = (addr == 0x1A)   ? "CST328"
                       : (addr == 0x58) ? "CST3530"
                       : (addr == 0)    ? "NONE"
                                        : "?";

    lv_label_set_text_fmt(s_label,
                          "%s @%02X sig%04X  rd%lu pr%lu\n"
                          "xy %d,%d\n"
                          "bus1(1/3): %s   bus0(11/10): %s",
                          part, (int)addr, (int)sig, (unsigned long)reads,
                          (unsigned long)presses, (int)x, (int)y, s_touch_scan,
                          s_sensor_scan);
    lv_obj_set_style_text_color(s_label,
                                lv_color_hex((found && presses > 0) ? COLOR_OK : COLOR_WARN), 0);
}

} // namespace

void TouchDebug_Show() {
    if (s_label != nullptr) {
        return;
    }

    // Both buses, once, before the first draw. Touch_Init() has already run
    // and begun the touch bus; the sensor bus has no driver at all yet, so it
    // is begun here purely to be scanned.
    ScanBus(Wire1, s_touch_scan, sizeof(s_touch_scan));
    Wire.begin(SENSOR_I2C_SDA, SENSOR_I2C_SCL, 400000);
    ScanBus(Wire, s_sensor_scan, sizeof(s_sensor_scan));

    s_label = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_CENTER, 0);
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
