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

    const bool ride_active = (inhibit != NULL) && inhibit->ride_active;
    const bool ride_finished = (inhibit != NULL) && inhibit->ride_finished;
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

    // Past the blank threshold. Whether it goes further is a separate question,
    // and it is answered in two parts: may we sleep at all, and if so, after
    // how long. Both report BLANK rather than pretending the threshold was
    // never reached, so the panel can say what stage it is really in.
    if (!policy->sleep_enabled || ride_active) {
        return POWER_STAGE_BLANK;
    }

    // A finished ride is a statement of intent, so it gets the short wait. A
    // device that has recorded nothing has made no such statement and gets the
    // long one -- it may be waiting on a rider who has not started yet, and
    // vanishing on them is worse than staying awake a while longer.
    const uint32_t threshold = ride_finished ? policy->sleep_after_ms : policy->sleep_idle_ms;
    if (idle_ms < threshold) {
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
