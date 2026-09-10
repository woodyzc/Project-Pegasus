#include "Battery.h"

#include <Arduino.h>

#include "../system/DataCenter.h"

#ifndef BATTERY_ADC_PIN
#define BATTERY_ADC_PIN 9 // ADC1_CH8 on ESP32-S3; vendor pinout for this board
#endif

#ifndef BATTERY_DIVIDER_NUM
#define BATTERY_DIVIDER_NUM 2 // 2:1 resistor divider -> pack mV = pin mV * 2
#endif

namespace {

// LiPo discharge curve from deps/Waveshare-LCD-2.8's power_manager. Piecewise
// linear rather than a single 3.0-4.2V ramp, because a LiPo spends most of its
// charge between 3.7V and 4.2V and a linear map badly overstates what's left
// once it drops below nominal.
struct CurvePoint {
    uint16_t mv;
    uint8_t percent;
};

constexpr CurvePoint CURVE[] = {
    {3000, 0},   // cutoff
    {3400, 10},  // low-battery warning
    {3700, 50},  // nominal
    {4200, 100}, // fully charged
};
constexpr size_t CURVE_LEN = sizeof(CURVE) / sizeof(CURVE[0]);

// Above any voltage a 1S LiPo reaches on its own, so the rail is being held up
// by USB. Matches the reference implementation's isOnBattery() threshold.
constexpr uint16_t USB_POWERED_MV = 4500;

constexpr int SAMPLE_COUNT = 8;      // ADC1 is noisy; average a short burst
constexpr uint32_t PUBLISH_PERIOD_MS = 5000;

bool s_ready = false;

void BatteryTask(void *pv) {
    (void)pv;

    for (;;) {
        Battery_t battery;
        battery.millivolts = Battery_ReadMillivolts();
        battery.percent = Battery_PercentFromMillivolts(battery.millivolts);
        battery.on_usb = (battery.millivolts >= USB_POWERED_MV);

        DataCenter_Publish(TOPIC_BATTERY, &battery);

        vTaskDelay(pdMS_TO_TICKS(PUBLISH_PERIOD_MS));
    }
}

} // namespace

void Battery_Init() {
    // 12-bit at 11dB attenuation: the divider puts a 4.2V pack at ~2.1V on the
    // pin, which needs the widest range to stay off the top of the scale.
    analogReadResolution(12);
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
    s_ready = true;
}

uint16_t Battery_ReadMillivolts() {
    if (!s_ready) {
        return 0;
    }

    // analogReadMilliVolts() applies the chip's eFused ADC calibration, which
    // matters here: the raw counts on an uncalibrated S3 can be off by well
    // over 100mV, enough to move the reported percentage by double digits.
    uint32_t total = 0;
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        total += analogReadMilliVolts(BATTERY_ADC_PIN);
    }

    const uint32_t pin_mv = total / SAMPLE_COUNT;
    return (uint16_t)(pin_mv * BATTERY_DIVIDER_NUM);
}

uint8_t Battery_PercentFromMillivolts(uint16_t millivolts) {
    if (millivolts == 0) {
        return 0; // no reading yet
    }
    if (millivolts <= CURVE[0].mv) {
        return 0;
    }
    if (millivolts >= CURVE[CURVE_LEN - 1].mv) {
        return 100;
    }

    for (size_t i = 1; i < CURVE_LEN; i++) {
        if (millivolts <= CURVE[i].mv) {
            const CurvePoint &lo = CURVE[i - 1];
            const CurvePoint &hi = CURVE[i];
            const uint32_t span_mv = hi.mv - lo.mv;
            const uint32_t span_pct = hi.percent - lo.percent;
            return (uint8_t)(lo.percent + ((millivolts - lo.mv) * span_pct) / span_mv);
        }
    }
    return 100;
}

void Battery_StartMonitor() {
    // Core 0 per CLAUDE.md §4 ("Task 3: Power & IMU monitoring"). Small stack:
    // this task only reads an ADC and publishes a 4-byte struct.
    xTaskCreatePinnedToCore(BatteryTask, "battery", 2048, nullptr, 1, nullptr, 0);
}
