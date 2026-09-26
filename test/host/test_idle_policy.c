// Host tests for src/system/IdlePolicy.c -- when the screen dims, blanks and
// sleeps.
//
// Worth testing off-target because the alternative is waiting. Every threshold
// here is minutes long, the interesting cases are combinations of three
// inhibitors, and a mistake shows up either as a device that dies at lunchtime
// or as one that blanks while the rider is reading it.

#include <stdio.h>

#include "../../src/system/IdlePolicy.h"

static int checks;
static int failures;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("\n    FAIL: %s", what);
    }
}

static const char *StageName(PowerStage_t stage) {
    switch (stage) {
        case POWER_STAGE_ACTIVE: return "ACTIVE";
        case POWER_STAGE_DIM:    return "DIM";
        case POWER_STAGE_BLANK:  return "BLANK";
        case POWER_STAGE_SLEEP:  return "SLEEP";
        default:                 return "?";
    }
}

static void check_stage(const IdlePolicy_t *p, uint32_t idle_ms, const IdleInhibit_t *inh,
                       PowerStage_t want, const char *what) {
    const PowerStage_t got = IdlePolicy_Stage(p, idle_ms, inh);
    checks++;
    if (got != want) {
        failures++;
        printf("\n    FAIL: %s (got %s, want %s)", what, StageName(got), StageName(want));
    }
}

int main(void) {
    // The shipped numbers: one minute, three minutes, five.
    const IdlePolicy_t policy = {60000, 180000, 300000, 1800000, true};
    const IdlePolicy_t no_sleep = {60000, 180000, 300000, 1800000, false};
    const IdleInhibit_t idle = {false, false, false};
    // A ride was recorded and has ended: the state that takes the short sleep
    // threshold, and the one an ordinary evening ends in.
    const IdleInhibit_t done = {false, true, false};

    printf("- the stages arrive in order as time passes: ");
    // Walked with a finished ride, because that is the case the short sleep
    // threshold belongs to and this is the ordering test. The long threshold
    // gets its own case below.
    check_stage(&policy, 0, &done, POWER_STAGE_ACTIVE, "just touched");
    check_stage(&policy, 59999, &done, POWER_STAGE_ACTIVE, "a moment before dimming");
    check_stage(&policy, 60000, &done, POWER_STAGE_DIM, "exactly at the dim threshold");
    check_stage(&policy, 179999, &done, POWER_STAGE_DIM, "a moment before blanking");
    check_stage(&policy, 180000, &done, POWER_STAGE_BLANK, "exactly at the blank threshold");
    check_stage(&policy, 299999, &done, POWER_STAGE_BLANK, "a moment before sleeping");
    check_stage(&policy, 300000, &done, POWER_STAGE_SLEEP, "exactly at the sleep threshold");
    check_stage(&policy, 0xFFFFFFFFu, &done, POWER_STAGE_SLEEP, "and stays asleep");
    printf("done\n");

    printf("- sleep off still dims and blanks, and stops there: ");
    check_stage(&no_sleep, 60000, &idle, POWER_STAGE_DIM, "dims");
    check_stage(&no_sleep, 180000, &idle, POWER_STAGE_BLANK, "blanks");
    check_stage(&no_sleep, 300000, &idle, POWER_STAGE_BLANK, "but never sleeps");
    check_stage(&no_sleep, 0xFFFFFFFFu, &idle, POWER_STAGE_BLANK, "however long it waits");
    printf("done\n");

    printf("- an active ride blocks sleeping and nothing else: ");
    {
        const IdleInhibit_t riding = {true, false, false};
        check_stage(&policy, 60000, &riding, POWER_STAGE_DIM, "the screen may still dim");
        check_stage(&policy, 180000, &riding, POWER_STAGE_BLANK, "and still go dark");
        // A long lunch stop must not end the ride, and neither must a long
        // wait for a first fix in a multi-storey car park.
        check_stage(&policy, 300000, &riding, POWER_STAGE_BLANK, "but the ride is not ended");
        check_stage(&policy, 0xFFFFFFFFu, &riding, POWER_STAGE_BLANK, "however long it waits");
    }
    printf("done\n");

    printf("- a finished ride sleeps at the short threshold: ");
    {
        check_stage(&policy, 299999, &done, POWER_STAGE_BLANK, "not a moment early");
        check_stage(&policy, 300000, &done, POWER_STAGE_SLEEP, "and then it sleeps");
    }
    printf("done\n");

    printf("- a device that has recorded nothing waits far longer: ");
    {
        // The case the long threshold exists for: powered on, never told to
        // start, possibly sitting on a bar while its owner pumps a tyre.
        // Vanishing on that rider is worse than staying awake.
        check_stage(&policy, 300000, &idle, POWER_STAGE_BLANK, "does not take the short one");
        check_stage(&policy, 1799999, &idle, POWER_STAGE_BLANK, "nor anything before 30 min");
        check_stage(&policy, 1800000, &idle, POWER_STAGE_SLEEP, "and then it sleeps");
    }
    printf("done\n");

    printf("- a file transfer blocks everything, including dimming: ");
    {
        const IdleInhibit_t xfer = {false, false, true};
        // The password is on that screen and the rider is typing it into a
        // phone, which is not an activity the touch panel ever sees.
        check_stage(&policy, 60000, &xfer, POWER_STAGE_ACTIVE, "does not dim");
        check_stage(&policy, 300000, &xfer, POWER_STAGE_ACTIVE, "does not blank");
        check_stage(&policy, 0xFFFFFFFFu, &xfer, POWER_STAGE_ACTIVE, "and does not sleep");
    }
    printf("done\n");

    printf("- an active ride outranks a finished one, and transfer outranks both: ");
    {
        // Both true is not nonsense: finish a ride, start another, and the
        // second is active while the first is still the most recent thing to
        // have ended. Active has to win or the short threshold would apply
        // mid-ride.
        const IdleInhibit_t both = {true, true, false};
        check_stage(&policy, 300000, &both, POWER_STAGE_BLANK, "active beats finished");
        check_stage(&policy, 0xFFFFFFFFu, &both, POWER_STAGE_BLANK, "at any idle time");
        const IdleInhibit_t all = {true, true, true};
        check_stage(&policy, 300000, &all, POWER_STAGE_ACTIVE, "transfer still wins over both");
    }
    printf("done\n");

    printf("- a later stage never fires before an earlier one: ");
    {
        // Thresholds out of order, which is what a future settings page with
        // independent controls could produce.
        const IdlePolicy_t muddled = {180000, 60000, 30000, 30000, true};
        check_stage(&muddled, 0, &idle, POWER_STAGE_ACTIVE, "still active at zero");
        check_stage(&muddled, 59999, &idle, POWER_STAGE_ACTIVE, "no blank before the dim");
        check_stage(&muddled, 179999, &idle, POWER_STAGE_ACTIVE, "nor a sleep before it");
        check_stage(&muddled, 180000, &idle, POWER_STAGE_SLEEP, "and then it catches up at once");
    }
    printf("done\n");

    printf("- a null policy keeps the screen on rather than guessing: ");
    check(IdlePolicy_Stage(NULL, 0xFFFFFFFFu, &idle) == POWER_STAGE_ACTIVE, "null policy");
    // A null inhibit reads as every field false, which includes ride_finished
    // -- so it takes the LONG threshold, not the short one. Worth pinning:
    // "no information" must not be mistaken for "the rider said they were
    // done", and the two differ by twenty-five minutes.
    check(IdlePolicy_Stage(&policy, 300000, NULL) == POWER_STAGE_BLANK,
          "null inhibit takes the long threshold");
    check(IdlePolicy_Stage(&policy, 1800000, NULL) == POWER_STAGE_SLEEP,
          "and sleeps once it is reached");
    printf("done\n");

    printf("- dimming is a fraction of the rider's own setting: ");
    check(IdlePolicy_DimPercent(100) == 20, "full brightness dims to a fifth");
    check(IdlePolicy_DimPercent(50) == 10, "half dims to a fifth of half");
    // The bug this exists to prevent: a fixed dim level brightens a screen
    // that was already dimmer than it.
    check(IdlePolicy_DimPercent(20) == 5, "a low setting stops at the floor");
    check(IdlePolicy_DimPercent(10) == 5, "and stays there");
    check(IdlePolicy_DimPercent(3) == 3, "below the floor it never brightens");
    check(IdlePolicy_DimPercent(0) == 0, "off stays off");
    printf("done\n");

    printf("- each stage maps to a backlight level: ");
    check(IdlePolicy_Brightness(POWER_STAGE_ACTIVE, 70) == 70, "active keeps the setting");
    check(IdlePolicy_Brightness(POWER_STAGE_DIM, 70) == 14, "dim scales it");
    check(IdlePolicy_Brightness(POWER_STAGE_BLANK, 70) == 0, "blank is off");
    check(IdlePolicy_Brightness(POWER_STAGE_SLEEP, 70) == 0, "so is sleep");
    printf("done\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
