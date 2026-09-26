#pragma once

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Decides what the device should be doing after N milliseconds of nothing
// happening: full brightness, dimmed, dark, or asleep.
//
// Pure, so it can be host-tested. PowerManager.cpp owns the backlight, the
// clock and esp_deep_sleep_start(); this owns only the question "given how
// long it has been quiet and what is going on, which stage are we in".
// Getting that wrong is how a bike computer either dies at lunchtime or blanks
// while the rider is looking at it, and neither can be reproduced on a bench
// by waiting.

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POWER_STAGE_ACTIVE = 0, // the rider's chosen brightness
    POWER_STAGE_DIM,        // readable, but spending much less
    POWER_STAGE_BLANK,      // backlight off, everything still running
    POWER_STAGE_SLEEP,      // deep sleep; only a touch brings it back
} PowerStage_t;

typedef struct {
    uint32_t dim_after_ms;
    uint32_t blank_after_ms;
    // Two sleep thresholds, because the two situations are not alike. A rider
    // who has pressed "finish" has said they are done, so the device may go
    // soon. A device that has never recorded anything has been told nothing at
    // all -- it might be sitting on a bar waiting for its owner to finish
    // faffing with a shoe -- so it waits much longer before disappearing.
    uint32_t sleep_after_ms;   // a ride was recorded and has ended
    uint32_t sleep_idle_ms;    // no ride has happened this boot
    // Off until the rider turns it on. Waking from deep sleep depends on the
    // touch controller asserting its interrupt line, which has never been
    // tested on this board -- and if it does not, the device looks dead until
    // it is power cycled.
    bool sleep_enabled;
} IdlePolicy_t;

// Things that are happening regardless of whether anyone is touching the
// screen. Each blocks a different amount, and the differences are the point.
typedef struct {
    // A ride is under way: recording, or armed and waiting for its first fix.
    // Blocks sleep outright -- the screen may go dark at a long lunch stop,
    // but sleeping would end the ride, and a rider who has pressed "start" and
    // is waiting on satellites is about to need the device.
    bool ride_active;
    // A ride was recorded this boot and is over. Not an inhibitor at all: it
    // selects the shorter of the two sleep thresholds, because the rider has
    // said in as many words that they are finished.
    bool ride_finished;
    // WiFi file transfer. Blocks everything: the password is on the screen,
    // and sleeping would drop a transfer in progress.
    bool transferring;
} IdleInhibit_t;

// ---- Why "on USB" is not in that list any more ----
//
// It was, and it was the main thing standing between a plugged-in board and
// deep sleep. It came from Battery_t.on_usb, which is a threshold at 4500mV on
// a reading of the PACK voltage -- and a 1S charger terminates at 4.2V, so on
// this hardware that flag can never be true. A gate that never closes is worse
// than no gate: it reads as protection while providing none.
//
// The replacement asks a question the firmware can actually answer. Whether to
// sleep depends on whether the rider is using the device, not on whether a
// cable is in it, and ride state is known exactly.

// The stage for this much idle time. `inhibit` may be NULL, meaning nothing
// is inhibiting anything.
//
// Thresholds that are out of order are honoured in the order dim, blank,
// sleep rather than rejected: a later stage never triggers before an earlier
// one, whatever the numbers say.
PowerStage_t IdlePolicy_Stage(const IdlePolicy_t *policy, uint32_t idle_ms,
                              const IdleInhibit_t *inhibit);

// The backlight percentage for the dimmed stage, given what the rider chose.
// A fraction of their setting rather than a fixed level, so someone riding at
// 20% does not get *brighter* when the screen dims -- with a floor, because a
// dim screen that cannot be read at all may as well be off.
uint8_t IdlePolicy_DimPercent(uint8_t active_percent);

// The backlight percentage for a stage. Folds the two rules above together so
// PowerManager never decides a level itself.
uint8_t IdlePolicy_Brightness(PowerStage_t stage, uint8_t active_percent);

#ifdef __cplusplus
}
#endif
