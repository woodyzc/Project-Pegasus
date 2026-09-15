#include "BoardPower.h"

#include <Arduino.h>

void BoardPower_Init() {
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
