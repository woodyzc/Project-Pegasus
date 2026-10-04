// Host tests for src/navigation/MapScale.c -- the number on the scale bar.
//
// The arrangement is the opposite of the obvious one: the DISTANCE is chosen
// from a round sequence and the bar's pixel length follows. A fixed-width bar
// would be labelled "137 m", which is a number nobody can use.

#include <stdio.h>
#include <string.h>

#include "../../src/navigation/MapScale.h"

static int checks;
static int failures;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("\n    FAIL: %s", what);
    }
}

static void test_picks_round_numbers(void) {
    printf("- the label is always a number a rider can hold: ");
    const double mpps[] = {0.1, 0.37, 1.0, 2.5, 5.0, 13.0, 40.0, 150.0, 900.0};
    for (size_t i = 0; i < sizeof(mpps) / sizeof(mpps[0]); i++) {
        uint32_t m = 0;
        int px = 0;
        check(MapScale_Choose(mpps[i], 100, &m, &px), "a scale is always available");

        // Strip decades and the result must be 1, 2 or 5.
        uint32_t v = m;
        while (v >= 10 && (v % 10) == 0) {
            v /= 10;
        }
        check(v == 1 || v == 2 || v == 5, "the distance is 1, 2 or 5 times a power of ten");
    }
    printf("done\n");
}

static void test_fits_the_width(void) {
    printf("- the bar never overruns the space it was given: ");
    for (int w = 20; w <= 160; w += 7) {
        for (double mpp = 0.05; mpp < 500.0; mpp *= 1.7) {
            uint32_t m = 0;
            int px = 0;
            MapScale_Choose(mpp, w, &m, &px);
            check(px <= w, "within the budget");
            check(px >= 1, "and not nothing");
        }
    }
    printf("done\n");
}

static void test_the_bar_means_the_label(void) {
    printf("- the drawn length actually matches the printed distance: ");
    // The whole point of a scale bar: a rider measures the bar against the map
    // and reads the number. If those two disagree the bar is worse than absent.
    uint32_t m = 0;
    int px = 0;
    MapScale_Choose(5.0, 100, &m, &px);
    const double implied = (double)px * 5.0;
    const double err = (implied > (double)m) ? implied - (double)m : (double)m - implied;
    check(err <= 5.0, "bar length times scale is the labelled distance");
    printf("done\n");
}

static void test_zooming_out_never_shrinks_the_number(void) {
    printf("- zooming out never reduces the distance shown: ");
    uint32_t last = 0;
    for (double mpp = 0.1; mpp < 1000.0; mpp *= 1.3) {
        uint32_t m = 0;
        MapScale_Choose(mpp, 100, &m, NULL);
        check(m >= last, "monotonic as the view widens");
        last = m;
    }
    printf("done\n");
}

static void test_extremes(void) {
    printf("- absurd inputs: ");
    uint32_t m = 0;
    int px = 0;
    check(!MapScale_Choose(0.0, 100, &m, &px), "a zero scale has no bar");
    check(!MapScale_Choose(-1.0, 100, &m, &px), "nor a negative one");
    check(!MapScale_Choose(5.0, 0, &m, &px), "nor a zero width");

    // Zoomed so far in that even 1m does not fit: still answers, bar clamped.
    check(MapScale_Choose(0.0001, 10, &m, &px), "an extreme zoom still answers");
    check(px <= 10, "and stays inside the width");
    printf("done\n");
}

static void test_format(void) {
    printf("- metres below a kilometre, kilometres above: ");
    char buf[16];
    MapScale_Format(50, buf, sizeof(buf));
    check(strcmp(buf, "50 m") == 0, "50 m");
    MapScale_Format(500, buf, sizeof(buf));
    check(strcmp(buf, "500 m") == 0, "500 m");
    MapScale_Format(1000, buf, sizeof(buf));
    check(strcmp(buf, "1 km") == 0, "1 km, not 1.0 km");
    MapScale_Format(2000, buf, sizeof(buf));
    check(strcmp(buf, "2 km") == 0, "2 km");
    MapScale_Format(500000, buf, sizeof(buf));
    check(strcmp(buf, "500 km") == 0, "500 km");

    check(!MapScale_Format(100, NULL, 16), "null buffer refused");
    check(!MapScale_Format(100, buf, 2), "a buffer too small is refused, not overrun");
    printf("done\n");
}

int main(void) {
    printf("== test_map_scale ==\n");
    test_picks_round_numbers();
    test_fits_the_width();
    test_the_bar_means_the_label();
    test_zooming_out_never_shrinks_the_number();
    test_extremes();
    test_format();

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
