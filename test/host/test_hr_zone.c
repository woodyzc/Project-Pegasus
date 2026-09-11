// Host tests for src/system/HrZone.c.
//
// The first test is the one that matters: it reproduces, beat for beat, the
// zone table the rider's own phone shows. The band edges are unusual (30/40/
// 60/90% of reserve rather than the textbook 50/60/70/80/90) and were read off
// a screenshot, so pinning them here is what stops a plausible-looking
// "correction" to the conventional numbers from silently disagreeing with the
// phone.

#include <math.h>
#include <stdio.h>

#include "../../src/system/HrZone.h"

static int checks;
static int failures;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("\n    FAIL: %s", what);
    }
}

static void check_int(int got, int want, const char *what) {
    checks++;
    if (got != want) {
        failures++;
        printf("\n    FAIL: %s (got %d, want %d)", what, got, want);
    }
}

static void check_close(double got, double want, double tol, const char *what) {
    checks++;
    if (fabs(got - want) > tol) {
        failures++;
        printf("\n    FAIL: %s (got %.4f, want %.4f)", what, got, want);
    }
}

// The rider's figures, from the phone.
#define REST 51
#define MAX 174

int main(void) {
    printf("- reproduces the phone's zone table exactly: ");
    {
        // Samsung Health, heart-rate reserve, rest 51 / max 174:
        //   zone 1  51-87    zone 2  88-100   zone 3  101-124
        //   zone 4  125-161  zone 5  162-174
        const uint8_t lower[HR_ZONE_COUNT] = {51, 88, 101, 125, 162};
        const uint8_t upper[HR_ZONE_COUNT] = {87, 100, 124, 161, 174};

        for (int z = 0; z < HR_ZONE_COUNT; z++) {
            char label[64];
            snprintf(label, sizeof(label), "zone %d lower", z + 1);
            check_int(HrZone_LowerBpm(z, REST, MAX), lower[z], label);
            snprintf(label, sizeof(label), "zone %d upper", z + 1);
            check_int(HrZone_UpperBpm(z, REST, MAX), upper[z], label);
        }

        // ...and every boundary beat lands in the band the phone puts it in.
        for (int z = 0; z < HR_ZONE_COUNT; z++) {
            char label[64];
            snprintf(label, sizeof(label), "zone %d lower beat classifies", z + 1);
            check_int(HrZone_Index(lower[z], REST, MAX), z, label);
            snprintf(label, sizeof(label), "zone %d upper beat classifies", z + 1);
            check_int(HrZone_Index(upper[z], REST, MAX), z, label);
        }
    }
    printf("done\n");

    printf("- the bands are contiguous with no gap or overlap: ");
    for (int z = 1; z < HR_ZONE_COUNT; z++) {
        check_int(HrZone_LowerBpm(z, REST, MAX), HrZone_UpperBpm(z - 1, REST, MAX) + 1,
                  "lower edge is one beat above the band below");
    }
    printf("done\n");

    printf("- outside the range clamps rather than running off the table: ");
    check_int(HrZone_Index(0, REST, MAX), 0, "zero is zone 1");
    check_int(HrZone_Index(40, REST, MAX), 0, "below rest is zone 1");
    check_int(HrZone_Index(255, REST, MAX), HR_ZONE_COUNT - 1, "above max is zone 5");
    check_close(HrZone_Fraction(40, REST, MAX), 0.0, 1e-9, "below rest pins left");
    check_close(HrZone_Fraction(255, REST, MAX), 1.0, 1e-9, "above max pins right");
    printf("done\n");

    printf("- position along the bar is linear in bpm: ");
    check_close(HrZone_Fraction(REST, REST, MAX), 0.0, 1e-9, "rest at the left edge");
    check_close(HrZone_Fraction(MAX, REST, MAX), 1.0, 1e-9, "max at the right edge");
    // Halfway in beats is halfway along the bar. This is what makes the marker
    // move at a constant rate as the reading changes, rather than speeding up
    // and slowing down as it crosses zones of different widths.
    check_close(HrZone_Fraction((uint8_t)((REST + MAX) / 2), REST, MAX), 0.5, 0.005,
                "midpoint sits mid-bar");
    {
        const double a = HrZone_Fraction(100, REST, MAX);
        const double b = HrZone_Fraction(110, REST, MAX);
        const double c = HrZone_Fraction(120, REST, MAX);
        // Equal beat steps move the marker equally far, across a zone boundary
        // (101 and 125 both sit between these readings).
        check_close(b - a, c - b, 1e-9, "equal bpm steps are equal distances");
    }
    printf("done\n");

    printf("- segment widths match the bands they represent: ");
    {
        double total = 0.0;
        for (int z = 0; z < HR_ZONE_COUNT; z++) {
            total += HrZone_SpanFraction(z);
        }
        check_close(total, 1.0, 1e-9, "the five spans fill the bar");

        // Deliberately unequal: this is why segments are drawn proportionally.
        check_close(HrZone_SpanFraction(0), 0.30, 1e-9, "zone 1 is 30% of reserve");
        check_close(HrZone_SpanFraction(1), 0.10, 1e-9, "zone 2 is 10%");
        check_close(HrZone_SpanFraction(2), 0.20, 1e-9, "zone 3 is 20%");
        check_close(HrZone_SpanFraction(3), 0.30, 1e-9, "zone 4 is 30%");
        check_close(HrZone_SpanFraction(4), 0.10, 1e-9, "zone 5 is 10%");

        // A marker at a band's upper edge must land inside that band's segment,
        // which is the whole point of sizing them this way.
        double left = 0.0;
        for (int z = 0; z < HR_ZONE_COUNT; z++) {
            const double right = left + HrZone_SpanFraction(z);
            const double at_top = HrZone_Fraction(HrZone_UpperBpm(z, REST, MAX), REST, MAX);
            check(at_top >= left - 1e-6 && at_top <= right + 1e-6,
                  "upper beat of the band sits within its own segment");
            left = right;
        }
    }
    printf("done\n");

    printf("- an equal-width bar puts the marker in the segment that is lit: ");
    {
        check_close(HrZone_EqualWidthFraction(REST, REST, MAX), 0.0, 1e-9, "rest at the left");
        check_close(HrZone_EqualWidthFraction(MAX, REST, MAX), 1.0, 1e-9, "max at the right");

        // The property the whole function exists for: whatever the reading,
        // the marker lands inside the fifth of the bar belonging to its own
        // zone. Every beat from well below rest to well above max.
        for (int bpm = 0; bpm <= 255; bpm++) {
            const int zone = HrZone_Index((uint8_t)bpm, REST, MAX);
            const double at = HrZone_EqualWidthFraction((uint8_t)bpm, REST, MAX);
            const double left = (double)zone / (double)HR_ZONE_COUNT;
            const double right = (double)(zone + 1) / (double)HR_ZONE_COUNT;
            if (!(at >= left - 1e-9 && at <= right + 1e-9)) {
                char label[64];
                snprintf(label, sizeof(label), "bpm %d lands in zone %d's segment", bpm, zone + 1);
                check(0, label);
            } else {
                check(1, "marker within its zone's segment");
            }
        }

        // Reserve-space placement would NOT satisfy that, which is why the two
        // are separate functions. Zone 4 tops out at 90% of reserve but its
        // segment ends at 80% of an equal-width bar.
        check(HrZone_Fraction(HrZone_UpperBpm(3, REST, MAX), REST, MAX) > 0.8,
              "reserve placement really does fall outside the equal segment");
    }
    printf("done\n");

    printf("- a nonsensical rest/max pair is refused, not divided by: ");
    check_int(HrZone_Index(120, 180, 170), 0, "max below rest");
    check_int(HrZone_Index(120, 60, 60), 0, "max equal to rest");
    check_close(HrZone_Fraction(120, 60, 60), 0.0, 1e-9, "fraction stays at zero");
    check_close(HrZone_EqualWidthFraction(120, 60, 60), 0.0, 1e-9, "equal-width stays at zero");
    check_int(HrZone_SpanFraction(-1) == 0.0, 1, "negative zone");
    check_int(HrZone_SpanFraction(HR_ZONE_COUNT) == 0.0, 1, "zone past the end");
    printf("done\n");

    printf("- a different rider gets a table scaled to them: ");
    {
        // rest 60 / max 190: reserve 130, so the edges land elsewhere but the
        // same 30/40/60/90 proportions hold.
        check_int(HrZone_UpperBpm(0, 60, 190), 99, "30% of 130 above 60");
        check_int(HrZone_LowerBpm(1, 60, 190), 100, "next band starts a beat later");
        check_int(HrZone_UpperBpm(4, 60, 190), 190, "top band ends at max");
    }
    printf("done\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
