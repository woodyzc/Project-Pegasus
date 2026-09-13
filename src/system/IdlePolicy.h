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
    uint32_t sleep_after_ms;
    // Off until the rider turns it on. Waking from deep sleep depends on the
    // touch controller asserting its interrupt line, which has never been
    // tested on this board -- and if it does not, the device looks dead until
    // it is power cycled.
    bool sleep_enabled;
} IdlePolicy_t;

// Things that are happening regardless of whether anyone is touching the
// screen. Each blocks a different amount, and the differences are the point.
typedef struct {
    // Charging or bench-powered. Blocks sleep only: a device that sleeps
    // while plugged into the laptop that is flashing it looks broken, and
    // there is no battery to save.
    bool on_usb;
    // A ride is being written to the card. Blocks sleep only -- the screen
    // may go dark at a long lunch stop, but sleeping would end the ride.
    bool recording;
    // WiFi file transfer. Blocks everything: the password is on the screen,
    // and sleeping would drop a transfer in progress.
    bool transferring;
} IdleInhibit_t;

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
