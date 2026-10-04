#include "Battery.h"

#include <Arduino.h>

#include "../system/BatteryLevel.h"
#include "../system/DataCenter.h"

#ifndef BATTERY_ADC_PIN
#define BATTERY_ADC_PIN 8 // ADC1_CH7 on ESP32-S3; vendor pinout for this board
#endif

#ifndef BATTERY_DIVIDER_NUM
#define BATTERY_DIVIDER_NUM 3 // 3:1 resistor divider -> pack mV = pin mV * 3
#endif

#ifndef BATTERY_CAL_PERMILLE
// The vendor's trim on the nominal ratio: BAT_Driver.cpp divides by 0.990476,
// i.e. the real divider sits about 1% off its nominal value. Kept as parts per
// thousand so the whole conversion stays in integer arithmetic.
#define BATTERY_CAL_PERMILLE 990
#endif

namespace {

// Above any voltage a 1S LiPo reaches on its own, so the rail is being held up
// by USB. Matches the reference implementation's isOnBattery() threshold.
constexpr uint16_t USB_POWERED_MV = 4500;

// ⚠️ 64, not 8. The curve gives one percent per 7.5mV of pack, which is
// 2.5mV on the pin through the divider -- below the noise on an ESP32-S3
// ADC1 channel. A burst alone cannot fix that (it is over in microseconds and
// samples one slice of the noise, not its mean), but it is free and it helps;
// the filter across readings is what actually settles the gauge.
constexpr int SAMPLE_COUNT = 64;
constexpr uint32_t PUBLISH_PERIOD_MS = 5000;

bool s_ready = false;
BatterySmoother_t s_smoother;

void BatteryTask(void *pv) {
    (void)pv;

    for (;;) {
        Battery_t battery;
        // Smoothed before it becomes a percentage, not after: filtering the
        // percentage would quantise first and average the steps, which keeps
        // the jitter and adds lag.
        battery.millivolts = BatterySmoother_Push(&s_smoother, Battery_ReadMillivolts());
        battery.percent = Battery_PercentFromMillivolts(battery.millivolts);
        battery.on_usb = (battery.millivolts >= USB_POWERED_MV);

        DataCenter_Publish(TOPIC_BATTERY, &battery);

        vTaskDelay(pdMS_TO_TICKS(PUBLISH_PERIOD_MS));
    }
}

} // namespace

void Battery_Init() {
    // 12-bit at 11dB attenuation: the 3:1 divider puts a 4.2V pack at ~1.4V on the
    // pin, which needs the widest range to stay off the top of the scale.
    analogReadResolution(12);
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
    BatterySmoother_Reset(&s_smoother);
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

    // Divider ratio, then the vendor's calibration trim. Ordered so the
    // multiply happens first: a 4.2V pack puts ~1.4V on the pin, so the
    // intermediate is ~4200*1000 and nowhere near overflowing 32 bits, while
    // dividing first would throw away the fraction the trim exists to correct.
    const uint32_t pack_mv = (pin_mv * BATTERY_DIVIDER_NUM * 1000u) / BATTERY_CAL_PERMILLE;
    return (uint16_t)pack_mv;
}

uint8_t Battery_PercentFromMillivolts(uint16_t millivolts) {
    return BatteryLevel_PercentFromMillivolts(millivolts);
}

void Battery_StartMonitor() {
    // Core 0 per CLAUDE.md §4 ("Task 3: Power & IMU monitoring"). Small stack:
    // this task only reads an ADC and publishes a 4-byte struct.
    xTaskCreatePinnedToCore(BatteryTask, "battery", 2048, nullptr, 1, nullptr, 0);
}
