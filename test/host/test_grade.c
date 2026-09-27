// Host tests for src/system/Grade.c -- road grade from barometric altitude and
// horizontal distance.
//
// Grade is a ratio of two small numbers and nearly every way of getting it
// wrong still produces a plausible-looking figure, so the cases here are
// chosen for the mistakes that do not announce themselves:
//
//   * dividing by a run of nearly zero when the rider stops, which yields a
//     huge grade at exactly the moment someone is most likely to be looking
//     at the panel;
//   * taking the longest baseline in the window instead of the shortest
//     qualifying one, which reads correctly on a uniform hill and badly
//     everywhere else, and gets worse the faster the rider goes;
//   * ramping a filter up from zero so the first seconds of a climb read flat;
//   * holding the last grade for ever once the rider stops.

#include <math.h>
#include <stdio.h>

#include "../../src/system/Grade.h"

static int checks;
static int failures;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("\n    FAIL: %s", what);
    }
}

static void check_near(double got, double want, double tol, const char *what) {
    checks++;
    if (!(fabs(got - want) <= tol)) {
        failures++;
        printf("\n    FAIL: %s (got %.3f, want %.3f +/- %.3f)", what, got, want, tol);
    }
}

// Rides a constant grade at a constant speed, 1Hz, and returns the tracker.
// `pct` is rise over run in percent, `speed` in m/s, `n` samples.
static void ride(Grade_t *g, double pct, double speed, int n) {
    double dist = 0.0, alt = 0.0;
    uint32_t t = 10000;
    for (int i = 0; i < n; i++) {
        Grade_Feed(g, dist, (float)alt, t);
        dist += speed;
        alt += speed * (pct / 100.0);
        t += 1000;
    }
}

static void test_nothing_until_enough_run(void) {
    printf("- no grade until there is run enough to divide by: ");
    Grade_t g;
    Grade_Reset(&g);

    check(!Grade_Have(&g), "nothing before the first sample");

    // 5 m/s for five seconds is 25 m -- just short of the threshold.
    ride(&g, 5.0, 5.0, 6);
    check(!Grade_Have(&g), "25m of run is still not a grade");

    // The seventh sample crosses 30 m.
    ride(&g, 5.0, 5.0, 7);
    check(Grade_Have(&g), "30m of run is");
    printf("done\n");
}

static void test_steady_grades(void) {
    printf("- a steady climb, descent and flat each read back: ");
    Grade_t g;

    Grade_Reset(&g);
    ride(&g, 5.0, 5.0, 40);
    check_near(Grade_Pct(&g), 5.0, 0.05, "5% climb reads 5%");

    Grade_Reset(&g);
    ride(&g, -8.0, 8.0, 40);
    check_near(Grade_Pct(&g), -8.0, 0.05, "-8% descent reads -8%");

    Grade_Reset(&g);
    ride(&g, 0.0, 6.0, 40);
    check_near(Grade_Pct(&g), 0.0, 0.01, "flat reads zero");

    // Sign is the whole point of the cell: a rider must not be told they are
    // descending a climb.
    Grade_Reset(&g);
    ride(&g, 12.0, 4.0, 40);
    check(Grade_Pct(&g) > 0.0f, "uphill is positive");
    printf("done\n");
}

static void test_first_estimate_is_taken_whole(void) {
    printf("- the figure does not ramp up from flat when it appears: ");
    Grade_t g;
    Grade_Reset(&g);

    // Exactly enough run, on a 10% grade, and nothing before it.
    Grade_Feed(&g, 0.0, 0.0f, 1000);
    Grade_Feed(&g, 30.0, 3.0f, 2000);
    check(Grade_Have(&g), "a grade is available");
    check_near(Grade_Pct(&g), 10.0, 0.01,
               "the first estimate is the real grade, not a fraction of it");
    printf("done\n");
}

static void test_baseline_is_the_shortest_qualifying_one(void) {
    printf("- it tracks the hill underneath, not the average of the window: ");
    Grade_t g;
    Grade_Reset(&g);

    // 10 m/s, so the 30s window spans 300m -- ten times the baseline the
    // estimator should actually use. 200m of flat, then 50m at 10%.
    double dist = 0.0, alt = 0.0;
    uint32_t t = 10000;
    for (int i = 0; i <= 20; i++) { // 0..200m, flat
        Grade_Feed(&g, dist, (float)alt, t);
        dist += 10.0;
        t += 1000;
    }
    for (int i = 0; i < 5; i++) { // 210..250m, 10%
        alt += 1.0;
        Grade_Feed(&g, dist, (float)alt, t);
        dist += 10.0;
        t += 1000;
    }

    // Using the oldest sample still in the window would give 5m over 250m, or
    // 2%. Using the shortest qualifying baseline gives a raw 10%, which the
    // output filter is partway through following.
    check(Grade_Pct(&g) > 5.0f,
          "the local 10% is being followed, not the 2% window average");
    check(Grade_Pct(&g) <= 10.05f, "and it does not overshoot the real grade");
    printf("done\n");
}

static void test_stopping(void) {
    printf("- a rider stopped on a hill keeps the hill, then loses it: ");
    Grade_t g;
    Grade_Reset(&g);
    ride(&g, 7.0, 5.0, 40);
    check_near(Grade_Pct(&g), 7.0, 0.05, "climbing at 7%");

    // Standing still: distance stops advancing, the clock does not. Someone
    // halted at a light on a 7% ramp is still on a 7% ramp.
    const double dist = 5.0 * 39;
    const float alt = (float)(dist * 0.07);
    uint32_t t = 10000 + 1000 * 40;

    for (int i = 0; i < 10; i++) {
        t += 1000;
        Grade_Feed(&g, dist, alt, t);
    }
    check(Grade_Have(&g), "ten seconds stopped still shows the grade");

    // Once every pre-stop sample has aged out there is no run left to divide
    // by, and a near-zero denominator must not be allowed to answer.
    for (int i = 0; i < 30; i++) {
        t += 1000;
        Grade_Feed(&g, dist, alt, t);
    }
    check(!Grade_Have(&g), "past the window it blanks rather than dividing by ~0");
    printf("done\n");
}

static void test_clamp(void) {
    printf("- a pressure artefact is bounded, not published: ");
    Grade_t g;

    Grade_Reset(&g);
    Grade_Feed(&g, 0.0, 0.0f, 1000);
    Grade_Feed(&g, 30.0, 20.0f, 2000); // 66% -- not a road
    check_near(Grade_Pct(&g), GRADE_MAX_PCT, 0.01, "clamped uphill");

    Grade_Reset(&g);
    Grade_Feed(&g, 0.0, 0.0f, 1000);
    Grade_Feed(&g, 30.0, -20.0f, 2000);
    check_near(Grade_Pct(&g), -GRADE_MAX_PCT, 0.01, "clamped downhill");
    printf("done\n");
}

static void test_sensor_noise_on_the_flat(void) {
    printf("- sensor noise on the flat does not invent a grade: ");
    Grade_t g;
    Grade_Reset(&g);

    // +/-0.1m of altitude jitter, which is generous for a BMP580 behind
    // BaroAltitude's filter, over a 30m baseline.
    double dist = 0.0;
    uint32_t t = 10000;
    for (int i = 0; i < 60; i++) {
        const float alt = (i % 2 == 0) ? 0.1f : -0.1f;
        Grade_Feed(&g, dist, alt, t);
        dist += 5.0;
        t += 1000;
    }
    check(fabs((double)Grade_Pct(&g)) < 1.0, "stays under 1% on flat ground");
    printf("done\n");
}

static void test_odometer_reset(void) {
    printf("- distance going backwards is a reset, not reverse travel: ");
    Grade_t g;
    Grade_Reset(&g);
    ride(&g, 6.0, 5.0, 40);
    check(Grade_Have(&g), "riding");

    Grade_Feed(&g, 0.0, 100.0f, 100000);
    check(!Grade_Have(&g), "the history is dropped rather than run backwards");

    // And it recovers on its own terms.
    double dist = 0.0, alt = 100.0;
    uint32_t t = 100000;
    for (int i = 0; i < 12; i++) {
        Grade_Feed(&g, dist, (float)alt, t);
        dist += 5.0;
        alt += 5.0 * 0.04;
        t += 1000;
    }
    check_near(Grade_Pct(&g), 4.0, 0.05, "and reacquires the new grade");
    printf("done\n");
}

static void test_tick_wrap(void) {
    printf("- the millisecond tick wrapping mid-climb: ");
    Grade_t g;
    Grade_Reset(&g);

    // Straddles 2^32. An age computed with signed arithmetic here reads as
    // enormous and evicts the whole window on every sample.
    double dist = 0.0, alt = 0.0;
    uint32_t t = 0xFFFFF000u;
    for (int i = 0; i < 40; i++) {
        Grade_Feed(&g, dist, (float)alt, t);
        dist += 5.0;
        alt += 0.25;
        t += 1000;
    }
    check(Grade_Have(&g), "a grade survives the wrap");
    check_near(Grade_Pct(&g), 5.0, 0.05, "and is still correct across it");
    printf("done\n");
}

static void test_ring_buffer_wraps(void) {
    printf("- more samples than the history holds: ");
    Grade_t g;
    Grade_Reset(&g);

    // 10 m/s at 10Hz is 1m per sample, so 200 samples overrun the 64-deep
    // ring without ever reaching the 30s time bound.
    double dist = 0.0, alt = 0.0;
    uint32_t t = 10000;
    for (int i = 0; i < 200; i++) {
        Grade_Feed(&g, dist, (float)alt, t);
        dist += 1.0;
        alt += 0.06; // 6%
        t += 100;
    }
    check_near(Grade_Pct(&g), 6.0, 0.05, "the ring wrapping does not corrupt the baseline");
    printf("done\n");
}

static void test_null_arguments(void) {
    printf("- null arguments: ");
    Grade_Reset(NULL);
    check(!Grade_Feed(NULL, 0.0, 0.0f, 0), "feeding null is false, not a crash");
    check(!Grade_Have(NULL), "null has no grade");
    check(Grade_Pct(NULL) == 0.0f, "null reads zero");
    printf("done\n");
}

int main(void) {
    printf("== test_grade ==\n");
    test_nothing_until_enough_run();
    test_steady_grades();
    test_first_estimate_is_taken_whole();
    test_baseline_is_the_shortest_qualifying_one();
    test_stopping();
    test_clamp();
    test_sensor_noise_on_the_flat();
    test_odometer_reset();
    test_tick_wrap();
    test_ring_buffer_wraps();
    test_null_arguments();

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
