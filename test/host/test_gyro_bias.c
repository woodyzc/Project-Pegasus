// Host tests for src/system/GyroBias.c -- zero-rate offset removal.
//
// The dangerous failure is not a bias left uncorrected; it is a bias
// estimator that learns REAL ROTATION and subtracts it. That reports a moving
// bike as still, which stops the odometer. So most of what is checked here is
// the estimator declining to adapt.

#include <math.h>
#include <stdio.h>

#include "../../src/system/GyroBias.h"

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

static float settle(GyroBias_t *b, float x, float y, float z, bool quiet, int n) {
    const float g[3] = {x, y, z};
    float out = 0.0f;
    for (int i = 0; i < n; i++) {
        out = GyroBias_Correct(b, g, quiet);
    }
    return out;
}

static void test_learns_a_resting_offset(void) {
    printf("- a resting offset is learned and removed: ");
    GyroBias_t b;
    GyroBias_Reset(&b);

    // The bench unit's 3.2 dps, spread over axes.
    const float out = settle(&b, 2.0f, 2.0f, 1.0f, true, 200);
    check_near(out, 0.0, 0.05, "a still part reads ~0 after correction");
    check_near(GyroBias_MagnitudeDps(&b), 3.0, 0.1, "and the estimate matches the offset");
    printf("done\n");
}

static void test_seeded_whole(void) {
    printf("- the first quiet sample is taken whole, not ramped: ");
    GyroBias_t b;
    GyroBias_Reset(&b);
    const float g[3] = {3.0f, 0.0f, 0.0f};
    // One sample is enough. Filtering up from zero would spend the first
    // minute reporting most of the offset as rotation.
    check_near(GyroBias_Correct(&b, g, true), 0.0, 0.001, "corrected immediately");
    printf("done\n");
}

static void test_refuses_to_learn_real_rotation(void) {
    printf("- real rotation is never learned as bias: ");
    GyroBias_t b;
    GyroBias_Reset(&b);

    // A bike being wheeled round: well past any resting offset. Even with the
    // accelerometer quiet, this must not be absorbed.
    const float out = settle(&b, 50.0f, 0.0f, 0.0f, true, 500);
    check_near(out, 50.0, 0.5, "50 dps still reads as 50 dps");
    check_near(GyroBias_MagnitudeDps(&b), 0.0, 0.001, "and nothing was learned");
    printf("done\n");
}

static void test_only_adapts_when_the_accelerometer_agrees(void) {
    printf("- it will not adapt while the part is being accelerated: ");
    GyroBias_t b;
    GyroBias_Reset(&b);

    const float out = settle(&b, 3.0f, 0.0f, 0.0f, false, 500);
    check_near(out, 3.0, 0.001, "no correction without the accelerometer's agreement");
    check_near(GyroBias_MagnitudeDps(&b), 0.0, 0.001, "nothing learned");
    printf("done\n");
}

static void test_real_rotation_survives_a_learned_bias(void) {
    printf("- a learned offset does not eat a real turn: ");
    GyroBias_t b;
    GyroBias_Reset(&b);
    settle(&b, 3.0f, 0.0f, 0.0f, true, 300); // learn the 3 dps offset

    // Now the bike turns at 40 dps on the same axis. The reading must be the
    // rotation, not the rotation minus nothing and not zero.
    const float g[3] = {43.0f, 0.0f, 0.0f};
    check_near(GyroBias_Correct(&b, g, true), 40.0, 0.2, "reads the real 40 dps");
    printf("done\n");
}

static void test_absurd_bias_is_refused(void) {
    printf("- an implausible estimate is not trusted: ");
    GyroBias_t b;
    GyroBias_Reset(&b);

    // Force a large estimate the only way the guard allows: a seed inside the
    // adapt window cannot reach it, so this checks the clamp directly by
    // hand-setting the state the way a faulty part would drift it.
    b.have = true;
    b.bias[0] = 100.0f;
    const float g[3] = {5.0f, 0.0f, 0.0f};
    check_near(GyroBias_Correct(&b, g, true), 5.0, 0.001,
               "the raw reading is used rather than a 100 dps subtraction");
    printf("done\n");
}

static void test_tracks_drift(void) {
    printf("- it follows a slow thermal drift: ");
    GyroBias_t b;
    GyroBias_Reset(&b);
    settle(&b, 2.0f, 0.0f, 0.0f, true, 300);
    check_near(GyroBias_MagnitudeDps(&b), 2.0, 0.05, "learned the first offset");

    // The part warms and its offset moves. The estimate should follow.
    settle(&b, 5.0f, 0.0f, 0.0f, true, 1000);
    check_near(GyroBias_MagnitudeDps(&b), 5.0, 0.1, "followed it");
    printf("done\n");
}

static void test_null_arguments(void) {
    printf("- null arguments: ");
    GyroBias_Reset(NULL);
    const float g[3] = {1.0f, 0.0f, 0.0f};
    // ⚠️ Movement, not zero. Zero reads as "not rotating", which is exactly
    // the input that lets a caller conclude the bike is parked and stop the
    // odometer. The safe answer to a question this module cannot answer is
    // always movement.
    check(GyroBias_Correct(NULL, g, true) >= GYRO_BIAS_ADAPT_MAX_DPS,
          "null tracker reads as moving, not as still");
    GyroBias_t b;
    GyroBias_Reset(&b);
    check(GyroBias_Correct(&b, NULL, true) >= GYRO_BIAS_ADAPT_MAX_DPS,
          "null sample reads as moving, not as still");
    check(GyroBias_MagnitudeDps(NULL) == 0.0f, "null magnitude is 0");
    printf("done\n");
}

int main(void) {
    printf("== test_gyro_bias ==\n");
    test_learns_a_resting_offset();
    test_seeded_whole();
    test_refuses_to_learn_real_rotation();
    test_only_adapts_when_the_accelerometer_agrees();
    test_real_rotation_survives_a_learned_bias();
    test_absurd_bias_is_refused();
    test_tracks_drift();
    test_null_arguments();

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
