// Host tests for src/system/Stillness.c -- is the bike moving at all.
//
// The asymmetry is the subject. Stillness must be sustained before it is
// believed and movement must be believed at once, because the two mistakes
// cost different things: a late "moving" throws away real metres from the
// odometer, a late "still" costs a second of drift nobody was watching.
//
// The orientation-free test matters too. A head unit clamps to the bars at
// whatever angle its owner likes, so anything that keyed on a particular axis
// would need calibration and would break the first time it was remounted.

#include <math.h>
#include <stdio.h>

#include "../../src/system/Stillness.h"

static int checks;
static int failures;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("\n    FAIL: %s", what);
    }
}

// Feeds `ms` worth of samples at 50ms intervals, returning the final verdict.
static bool feed_for(Stillness_t *s, float acc, float gyro, uint32_t *t, uint32_t ms) {
    bool out = Stillness_IsStill(s);
    for (uint32_t i = 0; i < ms; i += 50) {
        out = Stillness_Feed(s, acc, gyro, *t);
        *t += 50;
    }
    return out;
}

static void test_starts_moving(void) {
    printf("- nothing is still until it has proved it: ");
    Stillness_t s;
    Stillness_Reset(&s);
    check(!Stillness_IsStill(&s), "a fresh detector is not still");

    uint32_t t = 10000;
    // At rest, but not yet for long enough.
    check(!feed_for(&s, 1.0f, 0.0f, &t, STILL_HOLD_MS - 200), "quiet but not yet long enough");
    printf("done\n");
}

static void test_becomes_still(void) {
    printf("- sustained quiet is believed: ");
    Stillness_t s;
    Stillness_Reset(&s);
    uint32_t t = 10000;
    check(feed_for(&s, 1.0f, 0.0f, &t, STILL_HOLD_MS + 500), "still after the hold");
    check(Stillness_IsStill(&s), "and stays still");
    printf("done\n");
}

static void test_movement_is_immediate(void) {
    printf("- movement is believed on the very first sample: ");
    Stillness_t s;
    Stillness_Reset(&s);
    uint32_t t = 10000;
    feed_for(&s, 1.0f, 0.0f, &t, STILL_HOLD_MS + 500);
    check(Stillness_IsStill(&s), "still to begin with");

    // One sample of rotation. No hold, no averaging: the rider has set off.
    check(!Stillness_Feed(&s, 1.0f, 40.0f, t), "one rotating sample ends stillness");
    check(!Stillness_IsStill(&s), "and it stays ended");
    printf("done\n");
}

static void test_either_axis_of_evidence(void) {
    printf("- acceleration and rotation each end it alone: ");
    Stillness_t s;
    uint32_t t = 10000;

    Stillness_Reset(&s);
    feed_for(&s, 1.0f, 0.0f, &t, STILL_HOLD_MS + 500);
    check(!Stillness_Feed(&s, 1.6f, 0.0f, t), "acceleration alone");

    Stillness_Reset(&s);
    t = 10000;
    feed_for(&s, 1.0f, 0.0f, &t, STILL_HOLD_MS + 500);
    check(!Stillness_Feed(&s, 1.0f, 60.0f, t), "rotation alone");
    printf("done\n");
}

static void test_orientation_free(void) {
    printf("- any mounting angle reads as 1g: ");
    // The magnitude is what is tested, so a head unit on its back, on its
    // edge, or at 40 degrees on the bars is identical to this detector.
    Stillness_t s;
    uint32_t t = 10000;
    Stillness_Reset(&s);
    check(feed_for(&s, 1.0f, 0.2f, &t, STILL_HOLD_MS + 500), "1g, whatever direction it points");

    // Free fall is NOT at rest, even though no axis dominates.
    Stillness_Reset(&s);
    t = 10000;
    check(!feed_for(&s, 0.0f, 0.0f, &t, STILL_HOLD_MS + 500), "free fall is not rest");
    printf("done\n");
}

static void test_restarts_the_hold(void) {
    printf("- a bump restarts the clock rather than shortening it: ");
    Stillness_t s;
    Stillness_Reset(&s);
    uint32_t t = 10000;

    feed_for(&s, 1.0f, 0.0f, &t, STILL_HOLD_MS - 200);
    Stillness_Feed(&s, 1.0f, 50.0f, t); // knocked
    t += 50;
    // Nearly the whole hold again is still not enough.
    check(!feed_for(&s, 1.0f, 0.0f, &t, STILL_HOLD_MS - 200),
          "the hold starts again from the bump");
    check(feed_for(&s, 1.0f, 0.0f, &t, 400), "and completes shortly after");
    printf("done\n");
}

static void test_tolerances(void) {
    printf("- the quiet band itself: ");
    Stillness_t s;
    uint32_t t = 10000;

    // Just inside, both signs.
    Stillness_Reset(&s);
    check(feed_for(&s, 1.0f + (STILL_ACCEL_TOL_G * 0.5f), STILL_GYRO_TOL_DPS * 0.5f, &t,
                   STILL_HOLD_MS + 500),
          "inside the band is at rest");

    // Just outside.
    Stillness_Reset(&s);
    t = 10000;
    check(!feed_for(&s, 1.0f + (STILL_ACCEL_TOL_G * 2.0f), 0.0f, &t, STILL_HOLD_MS + 500),
          "outside the band is not");
    printf("done\n");
}

static void test_nan_is_movement(void) {
    printf("- a sensor producing nonsense must not freeze the odometer: ");
    Stillness_t s;
    Stillness_Reset(&s);
    uint32_t t = 10000;
    feed_for(&s, 1.0f, 0.0f, &t, STILL_HOLD_MS + 500);
    check(Stillness_IsStill(&s), "still first");

    check(!Stillness_Feed(&s, NAN, 0.0f, t), "NaN acceleration reads as moving");
    Stillness_Reset(&s);
    t = 10000;
    feed_for(&s, 1.0f, 0.0f, &t, STILL_HOLD_MS + 500);
    check(!Stillness_Feed(&s, 1.0f, NAN, t), "NaN rotation reads as moving");
    printf("done\n");
}

static void test_tick_wrap(void) {
    printf("- the millisecond tick wrapping while parked: ");
    Stillness_t s;
    Stillness_Reset(&s);
    // Straddles 2^32. Signed arithmetic here would read an enormous elapsed
    // time and declare stillness on the second sample.
    uint32_t t = 0xFFFFFF00u;
    check(!Stillness_Feed(&s, 1.0f, 0.0f, t), "first sample is never still");
    t += 100;
    check(!Stillness_Feed(&s, 1.0f, 0.0f, t), "nor the second, 100ms later");
    check(feed_for(&s, 1.0f, 0.0f, &t, STILL_HOLD_MS + 500), "still once the hold really elapses");
    printf("done\n");
}

static void test_null_arguments(void) {
    printf("- null arguments: ");
    Stillness_Reset(NULL);
    check(!Stillness_Feed(NULL, 1.0f, 0.0f, 0), "feeding null is false, not a crash");
    check(!Stillness_IsStill(NULL), "null is not still");
    printf("done\n");
}

int main(void) {
    printf("== test_stillness ==\n");
    test_starts_moving();
    test_becomes_still();
    test_movement_is_immediate();
    test_either_axis_of_evidence();
    test_orientation_free();
    test_restarts_the_hold();
    test_tolerances();
    test_nan_is_movement();
    test_tick_wrap();
    test_null_arguments();

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
