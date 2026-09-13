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
    const IdlePolicy_t policy = {60000, 180000, 300000, true};
    const IdlePolicy_t no_sleep = {60000, 180000, 300000, false};
    const IdleInhibit_t idle = {false, false, false};

    printf("- the stages arrive in order as time passes: ");
    check_stage(&policy, 0, &idle, POWER_STAGE_ACTIVE, "just touched");
    check_stage(&policy, 59999, &idle, POWER_STAGE_ACTIVE, "a moment before dimming");
    check_stage(&policy, 60000, &idle, POWER_STAGE_DIM, "exactly at the dim threshold");
    check_stage(&policy, 179999, &idle, POWER_STAGE_DIM, "a moment before blanking");
    check_stage(&policy, 180000, &idle, POWER_STAGE_BLANK, "exactly at the blank threshold");
    check_stage(&policy, 299999, &idle, POWER_STAGE_BLANK, "a moment before sleeping");
    check_stage(&policy, 300000, &idle, POWER_STAGE_SLEEP, "exactly at the sleep threshold");
    check_stage(&policy, 0xFFFFFFFFu, &idle, POWER_STAGE_SLEEP, "and stays asleep");
    printf("done\n");

    printf("- sleep off still dims and blanks, and stops there: ");
    check_stage(&no_sleep, 60000, &idle, POWER_STAGE_DIM, "dims");
    check_stage(&no_sleep, 180000, &idle, POWER_STAGE_BLANK, "blanks");
    check_stage(&no_sleep, 300000, &idle, POWER_STAGE_BLANK, "but never sleeps");
    check_stage(&no_sleep, 0xFFFFFFFFu, &idle, POWER_STAGE_BLANK, "however long it waits");
    printf("done\n");

    printf("- USB blocks sleeping and nothing else: ");
    {
        const IdleInhibit_t usb = {true, false, false};
        check_stage(&policy, 60000, &usb, POWER_STAGE_DIM, "still dims");
        check_stage(&policy, 180000, &usb, POWER_STAGE_BLANK, "still blanks");
        // Sleeping while plugged into the laptop that flashes it looks like a
        // dead board, and there is no battery to save.
        check_stage(&policy, 300000, &usb, POWER_STAGE_BLANK, "but does not sleep on USB");
    }
    printf("done\n");

    printf("- recording blocks sleeping and nothing else: ");
    {
        const IdleInhibit_t rec = {false, true, false};
        check_stage(&policy, 60000, &rec, POWER_STAGE_DIM, "the screen may still dim");
        check_stage(&policy, 180000, &rec, POWER_STAGE_BLANK, "and still go dark");
        // A long lunch stop must not end the ride.
        check_stage(&policy, 300000, &rec, POWER_STAGE_BLANK, "but the ride is not ended");
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

    printf("- inhibitors combine without one masking another: ");
    {
        const IdleInhibit_t both = {true, true, false};
        check_stage(&policy, 300000, &both, POWER_STAGE_BLANK, "USB and recording together");
        const IdleInhibit_t all = {true, true, true};
        check_stage(&policy, 300000, &all, POWER_STAGE_ACTIVE, "transfer still wins over both");
    }
    printf("done\n");

    printf("- a later stage never fires before an earlier one: ");
    {
        // Thresholds out of order, which is what a future settings page with
        // independent controls could produce.
        const IdlePolicy_t muddled = {180000, 60000, 30000, true};
        check_stage(&muddled, 0, &idle, POWER_STAGE_ACTIVE, "still active at zero");
        check_stage(&muddled, 59999, &idle, POWER_STAGE_ACTIVE, "no blank before the dim");
        check_stage(&muddled, 179999, &idle, POWER_STAGE_ACTIVE, "nor a sleep before it");
        check_stage(&muddled, 180000, &idle, POWER_STAGE_SLEEP, "and then it catches up at once");
    }
    printf("done\n");

    printf("- a null policy keeps the screen on rather than guessing: ");
    check(IdlePolicy_Stage(NULL, 0xFFFFFFFFu, &idle) == POWER_STAGE_ACTIVE, "null policy");
    check(IdlePolicy_Stage(&policy, 300000, NULL) == POWER_STAGE_SLEEP, "null inhibit is none");
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
