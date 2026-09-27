// Host tests for src/system/BaroAltitude.c -- pressure to altitude, and the
// smoothing in front of the climb total.
//
// Two of these exist because the failure is silent rather than loud. An
// inverted exponent still draws a smooth, monotonic, entirely plausible
// curve; it just puts sea level at 44km. And a filter seeded from zero
// instead of from its first sample produces a minute of invented climbing
// every time the board is switched on, which reads as a short warm-up rather
// than as a bug.

#include <math.h>
#include <stdio.h>

#include "../../src/system/BaroAltitude.h"

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

static void test_sea_level(void) {
    printf("- standard sea-level pressure is zero metres: ");
    check_near(BaroAltitude_MetresFromPa(BARO_SEA_LEVEL_PA), 0.0, 0.01,
               "101325 Pa reads 0m");
    // The inverted-exponent bug lands here, at about 44330m.
    check(BaroAltitude_MetresFromPa(BARO_SEA_LEVEL_PA) < 1.0f,
          "sea level is not tens of kilometres up");
    printf("done\n");
}

static void test_known_altitudes(void) {
    printf("- pressures from the standard atmosphere table: ");
    // ISA reference points. Tolerances are a few metres because the formula
    // is an approximation of the table, not a reproduction of it.
    check_near(BaroAltitude_MetresFromPa(89874.6f), 1000.0, 5.0, "89875 Pa is ~1000m");
    check_near(BaroAltitude_MetresFromPa(79495.2f), 2000.0, 8.0, "79495 Pa is ~2000m");
    check_near(BaroAltitude_MetresFromPa(54019.9f), 5000.0, 25.0, "54020 Pa is ~5000m");
    printf("done\n");
}

static void test_direction(void) {
    printf("- less pressure means higher, and the scale is right: ");
    const float low = BaroAltitude_MetresFromPa(100000.0f);
    const float high = BaroAltitude_MetresFromPa(95000.0f);
    check(high > low, "lower pressure reads higher");

    // About 8.3m per hPa near sea level. A wrong constant anywhere in the
    // formula shows up here as a plausible-looking number with the wrong
    // gearing -- the kind that makes a climb total quietly twice what it
    // should be.
    const float per_hpa = BaroAltitude_MetresFromPa(100000.0f) - BaroAltitude_MetresFromPa(100100.0f);
    check_near(per_hpa, 8.3, 0.6, "~8.3m per hPa near sea level");
    printf("done\n");
}

static void test_plausibility(void) {
    printf("- a failed read is rejected rather than turned into an altitude: ");
    check(!BaroAltitude_PressurePlausible(0.0f), "zero is not a pressure");
    check(!BaroAltitude_PressurePlausible(-1.0f), "negative is not a pressure");
    check(!BaroAltitude_PressurePlausible(1e9f), "a decode fault is not weather");
    check(!BaroAltitude_PressurePlausible(NAN), "NaN is rejected");
    check(BaroAltitude_PressurePlausible(101325.0f), "sea level is plausible");
    check(BaroAltitude_PressurePlausible(54020.0f), "5000m is plausible");

    // The NaN case is the one that matters: unrejected it reaches the climb
    // accumulator, and from then on every total is NaN with nothing on the
    // panel to say when it started.
    check(BaroAltitude_MetresFromPa(NAN) == 0.0f, "NaN pressure yields 0, not NaN");
    check(BaroAltitude_MetresFromPa(0.0f) == 0.0f, "zero pressure yields 0");
    printf("done\n");
}

static void test_smoother_seeds_from_first_sample(void) {
    printf("- the filter starts at the first reading, not at sea level: ");
    BaroSmoother_t s;
    BaroSmoother_Reset(&s);

    // Switched on at 300m. A filter seeded at zero would report ~30m here and
    // spend the next minute climbing to 300 -- 300m of ascent the rider never
    // rode.
    check_near(BaroSmoother_Push(&s, 300.0f, 0.1f), 300.0, 0.001,
               "first sample is taken whole");

    float last = 300.0f;
    for (int i = 0; i < 50; i++) {
        last = BaroSmoother_Push(&s, 300.0f, 0.1f);
    }
    check_near(last, 300.0, 0.001, "a steady input stays put");
    printf("done\n");
}

static void test_smoother_converges_and_damps(void) {
    printf("- it follows a real change and damps a spike: ");
    BaroSmoother_t s;
    BaroSmoother_Reset(&s);
    BaroSmoother_Push(&s, 100.0f, 0.2f);

    // A single wild sample must not move the output far.
    const float after_spike = BaroSmoother_Push(&s, 900.0f, 0.2f);
    check(after_spike < 300.0f, "one spike moves the output by a fraction");

    // A sustained change must arrive eventually, or the filter is just a
    // constant and the climb total never sees a hill.
    BaroSmoother_Reset(&s);
    BaroSmoother_Push(&s, 100.0f, 0.2f);
    float v = 100.0f;
    for (int i = 0; i < 100; i++) {
        v = BaroSmoother_Push(&s, 200.0f, 0.2f);
    }
    check_near(v, 200.0, 0.5, "a sustained change is followed");
    printf("done\n");
}

static void test_smoother_alpha_limits(void) {
    printf("- alpha at its limits: ");
    BaroSmoother_t s;
    BaroSmoother_Reset(&s);
    BaroSmoother_Push(&s, 100.0f, 0.5f);

    check_near(BaroSmoother_Push(&s, 500.0f, 1.0f), 500.0, 0.001, "alpha 1 is no smoothing");

    BaroSmoother_Reset(&s);
    BaroSmoother_Push(&s, 100.0f, 0.5f);
    check_near(BaroSmoother_Push(&s, 500.0f, 0.0f), 100.0, 0.001, "alpha 0 is frozen");
    printf("done\n");
}

static void test_null_arguments(void) {
    printf("- null arguments: ");
    BaroSmoother_Reset(NULL);
    check_near(BaroSmoother_Push(NULL, 42.0f, 0.1f), 42.0, 0.001,
               "a null smoother passes the sample through");
    printf("done\n");
}

int main(void) {
    printf("== test_baro_altitude ==\n");
    test_sea_level();
    test_known_altitudes();
    test_direction();
    test_plausibility();
    test_smoother_seeds_from_first_sample();
    test_smoother_converges_and_damps();
    test_smoother_alpha_limits();
    test_null_arguments();

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
