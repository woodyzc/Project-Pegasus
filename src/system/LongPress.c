#include "LongPress.h"

#include <stddef.h>

void LongPress_Reset(LongPress_t *lp, bool pressed_at_start) {
    if (lp == NULL) {
        return;
    }
    lp->down = pressed_at_start;
    lp->fired = false;
    // Disarmed while the key is already down. The board is switched ON by
    // this same key, so at the moment firmware starts watching it, the rider's
    // thumb is very often still on it.
    lp->armed = !pressed_at_start;
    lp->down_since_ms = 0;
}

bool LongPress_Feed(LongPress_t *lp, bool pressed, uint32_t t_ms) {
    if (lp == NULL) {
        return false;
    }

    if (!pressed) {
        lp->down = false;
        lp->fired = false;
        // A release is what arms it, so the press that powered the board on
        // can never be the press that powers it off.
        lp->armed = true;
        return false;
    }

    if (!lp->down) {
        lp->down = true;
        lp->fired = false;
        lp->down_since_ms = t_ms;
        return false;
    }

    if (!lp->armed || lp->fired) {
        return false;
    }

    // Unsigned, so the 49-day tick wrap yields the real hold time rather than
    // an enormous one that would fire on the sample after the wrap.
    if ((uint32_t)(t_ms - lp->down_since_ms) >= LONG_PRESS_MS) {
        lp->fired = true;
        return true;
    }
    return false;
}
