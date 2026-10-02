#include "BoardPower.h"

#include <Arduino.h>
#include <driver/gpio.h>

void BoardPower_Init() {
    // ⚠️ Release the deep-sleep hold FIRST, or everything below is a no-op.
    //
    // EnterSleep() latches this pin through the RTC so the rail survives deep
    // sleep (PowerManager.cpp). That latch does not end at wake-up: the pad
    // stays frozen at its held level for the rest of the boot until something
    // calls gpio_hold_dis(), and a held pad ignores pinMode and digitalWrite
    // silently.
    //
    // Benign for the level we want -- it is held HIGH and we want HIGH -- but
    // it freezes the pin in that state, so the power-off path that drives it
    // LOW would do nothing at all on any boot that followed a sleep. The fault
    // would read as "the power key does not work, but only sometimes", which
    // is the hardest kind to find.
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)PWR_LATCH_PIN);

    pinMode(PWR_LATCH_PIN, OUTPUT);
    digitalWrite(PWR_LATCH_PIN, HIGH);

    // The vendor driver latches high only if the power key reads low at this
    // moment -- its way of telling "the user pressed the button" from "the
    // rail came up on its own", so that a board woken by USB can still be
    // switched off by pulling the cable.
    //
    // We latch unconditionally instead. The difference matters in exactly one
    // case: a board sitting on USB with no battery, where the vendor leaves
    // the latch open and we close it. That costs nothing -- the rail is
    // already up -- and it removes a boot-time race whose failure mode is a
    // board that dies silently if the key is released a few milliseconds too
    // early. A bike computer that will not stay on is a worse fault than one
    // that stays on when it could have slept.
    //
    // The consequence to remember: with the latch closed and no key handling
    // yet, there is no software path that switches this board off. Pulling the
    // battery is it. Driving PWR_LATCH_PIN low is what will do it once the
    // power key lands.
    (void)PWR_KEY_PIN;
}
