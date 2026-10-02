#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Long-press detection for the power key.
//
// Pure arithmetic and no hardware: hal/BoardPower.cpp samples the pin and
// test/host/test_long_press.c is where this is actually checked.
//
// ---------------------------------------------------------------------------
// Why a tested module for something this small
// ---------------------------------------------------------------------------
// Because of what it does when it is wrong. This fires the power-off path, so
// a spurious trigger switches the board off mid-ride, and a trigger that
// repeats while the key is held switches it off again the instant the rider
// turns it back on. Neither failure is subtle to live with and both are easy
// to write.
//
// Three rules, each with a test:
//   * fires ONCE per press, however long the key is held after that;
//   * a release before the threshold fires nothing at all;
//   * a key found already down at startup is ignored until it has been
//     released -- otherwise a rider holding the button a moment too long to
//     switch the board ON would immediately switch it off again.

// How long the key must be held. Long enough that a knock cannot do it,
// short enough that nobody wonders whether it is working.
#define LONG_PRESS_MS 1500u

typedef struct {
    bool down;         // the key is currently held
    bool fired;        // this press has already produced its event
    bool armed;        // false until the first release, see the header
    uint32_t down_since_ms;
} LongPress_t;

// `pressed_at_start` is the state of the key the moment this is set up. Pass
// the pin's real level: true leaves the detector disarmed until release.
void LongPress_Reset(LongPress_t *lp, bool pressed_at_start);

// Folds in one sample. Returns true exactly once, on the sample where the
// hold threshold is first crossed.
bool LongPress_Feed(LongPress_t *lp, bool pressed, uint32_t t_ms);

#ifdef __cplusplus
}
#endif
