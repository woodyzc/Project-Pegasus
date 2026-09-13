#include "IdlePolicy.h"

#include <stddef.h>

// Below this the dim stage is unreadable indoors, never mind in daylight, so
// it stops scaling and sits here.
#define DIM_FLOOR_PERCENT 5

// A fifth of the rider's setting. Enough to see the screen is alive and read a
// large number off it, far short of what the backlight costs at full.
#define DIM_DIVISOR 5

PowerStage_t IdlePolicy_Stage(const IdlePolicy_t *policy, uint32_t idle_ms,
                              const IdleInhibit_t *inhibit) {
    if (policy == NULL) {
        return POWER_STAGE_ACTIVE;
    }

    const bool on_usb = (inhibit != NULL) && inhibit->on_usb;
    const bool recording = (inhibit != NULL) && inhibit->recording;
    const bool transferring = (inhibit != NULL) && inhibit->transferring;

    // Nothing dims while the file server is up: the password the rider is
    // typing into their phone is on that screen.
    if (transferring) {
        return POWER_STAGE_ACTIVE;
    }

    if (idle_ms < policy->dim_after_ms) {
        return POWER_STAGE_ACTIVE;
    }
    if (idle_ms < policy->blank_after_ms) {
        return POWER_STAGE_DIM;
    }

    // Past the blank threshold. Whether it goes further is a separate
    // question, and three different things can answer no. Checked in this
    // order so that a device on USB reports BLANK rather than pretending the
    // sleep threshold was never reached.
    if (!policy->sleep_enabled || on_usb || recording) {
        return POWER_STAGE_BLANK;
    }
    if (idle_ms < policy->sleep_after_ms) {
        return POWER_STAGE_BLANK;
    }
    return POWER_STAGE_SLEEP;
}

uint8_t IdlePolicy_DimPercent(uint8_t active_percent) {
    const uint8_t scaled = (uint8_t)(active_percent / DIM_DIVISOR);
    if (scaled < DIM_FLOOR_PERCENT) {
        // ...but never brighter than what the rider asked for. Someone riding
        // at 3% must not have the screen brighten when it dims.
        return (active_percent < DIM_FLOOR_PERCENT) ? active_percent : DIM_FLOOR_PERCENT;
    }
    return scaled;
}

uint8_t IdlePolicy_Brightness(PowerStage_t stage, uint8_t active_percent) {
    switch (stage) {
        case POWER_STAGE_DIM:
            return IdlePolicy_DimPercent(active_percent);
        case POWER_STAGE_BLANK:
        case POWER_STAGE_SLEEP:
            return 0;
        case POWER_STAGE_ACTIVE:
        default:
            return active_percent;
    }
}
