/* Host-side tests for ride averages and maxima (src/system/RideStatsCore.c).
 *
 * Every rule here exists because the obvious implementation is wrong in a way
 * that only shows up on a real ride: a mean of samples weights a slow patch
 * with dense fixes over a fast one with sparse fixes, a stop decays an average
 * that should hold, and one bad fix owns the "max" field for the rest of the
 * day. */
#include <stdio.h>

#include "RideStatsCore.h"

static int checks = 0;
static int failures = 0;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static int close_to(float a, float b) {
    const float d = a - b;
    return (d < 0.05f) && (d > -0.05f);
}

int main(void) {
    RideStats_t s;

    printf("- a fresh ride has nothing to report: ");
    RideStatsCore_Reset(&s);
    check(close_to(RideStatsCore_AvgSpeedKmh(&s), 0.0f), "no average speed");
    check(RideStatsCore_AvgBpm(&s) == 0, "no average heart rate");
    check(close_to(s.max_kmh, 0.0f), "no max speed");
    check(s.max_bpm == 0, "no max heart rate");
    printf("ok\n");

    printf("- a steady speed averages to itself: ");
    RideStatsCore_Reset(&s);
    for (int i = 0; i < 60; i++) {
        RideStatsCore_AddSpeed(&s, 20.0f, 1.0);
    }
    check(close_to(RideStatsCore_AvgSpeedKmh(&s), 20.0f), "average is 20");
    check(close_to(s.max_kmh, 20.0f), "max is 20");
    printf("ok\n");

    printf("- the average is weighted by time, not by sample count: ");
    /* Ten seconds at 10 and one second at 30 must not average to 20. A mean
       of samples would say exactly that, and a bike computer reporting 20 for
       a ride spent almost entirely at 10 is simply wrong. */
    RideStatsCore_Reset(&s);
    RideStatsCore_AddSpeed(&s, 10.0f, 10.0);
    RideStatsCore_AddSpeed(&s, 30.0f, 1.0);
    check(close_to(RideStatsCore_AvgSpeedKmh(&s), (10.0f * 10 + 30.0f) / 11.0f),
          "time-weighted, ~11.8");
    printf("ok\n");

    printf("- stopping does not decay the average: ");
    RideStatsCore_Reset(&s);
    for (int i = 0; i < 60; i++) {
        RideStatsCore_AddSpeed(&s, 20.0f, 1.0);
    }
    for (int i = 0; i < 600; i++) {
        RideStatsCore_AddSpeed(&s, 0.0f, 1.0); /* ten minutes at a cafe */
    }
    check(close_to(RideStatsCore_AvgSpeedKmh(&s), 20.0f), "still 20 after a long stop");
    printf("ok\n");

    printf("- GNSS wander while parked is not movement: ");
    RideStatsCore_Reset(&s);
    for (int i = 0; i < 600; i++) {
        RideStatsCore_AddSpeed(&s, RIDE_STATS_MOVING_KMH - 0.1f, 1.0);
    }
    check(close_to(RideStatsCore_AvgSpeedKmh(&s), 0.0f), "nothing counted");
    check(close_to(s.max_kmh, RIDE_STATS_MOVING_KMH - 0.1f), "but max still saw it");
    printf("ok\n");

    printf("- a cold fix cannot own the max for the rest of the ride: ");
    RideStatsCore_Reset(&s);
    RideStatsCore_AddSpeed(&s, 25.0f, 1.0);
    RideStatsCore_AddSpeed(&s, 400.0f, 1.0); /* a receiver settling */
    check(close_to(s.max_kmh, 25.0f), "the impossible sample is refused");
    RideStatsCore_AddSpeed(&s, RIDE_STATS_MAX_PLAUSIBLE_KMH, 1.0);
    check(close_to(s.max_kmh, RIDE_STATS_MAX_PLAUSIBLE_KMH), "the limit itself is allowed");
    printf("ok\n");

    printf("- a repeated timestamp adds nothing: ");
    RideStatsCore_Reset(&s);
    RideStatsCore_AddSpeed(&s, 20.0f, 1.0);
    {
        const float before = RideStatsCore_AvgSpeedKmh(&s);
        RideStatsCore_AddSpeed(&s, 20.0f, 0.0);
        RideStatsCore_AddSpeed(&s, 20.0f, -5.0);
        check(close_to(RideStatsCore_AvgSpeedKmh(&s), before), "average unchanged");
    }
    printf("ok\n");

    printf("- heart rate averages and peaks: ");
    RideStatsCore_Reset(&s);
    RideStatsCore_AddHeartRate(&s, 140);
    RideStatsCore_AddHeartRate(&s, 150);
    RideStatsCore_AddHeartRate(&s, 160);
    check(RideStatsCore_AvgBpm(&s) == 150, "average is 150");
    check(s.max_bpm == 160, "max is 160");
    printf("ok\n");

    printf("- the average rounds rather than truncates: ");
    RideStatsCore_Reset(&s);
    RideStatsCore_AddHeartRate(&s, 149);
    RideStatsCore_AddHeartRate(&s, 150);
    check(RideStatsCore_AvgBpm(&s) == 150, "149.5 reads as 150");
    printf("ok\n");

    printf("- a strap losing contact does not drag the average down: ");
    /* Straps report 0 when they lose skin contact, which is most of a ride
       for anyone who has not wetted the electrodes. */
    RideStatsCore_Reset(&s);
    RideStatsCore_AddHeartRate(&s, 150);
    RideStatsCore_AddHeartRate(&s, 0);
    RideStatsCore_AddHeartRate(&s, 250);
    check(RideStatsCore_AvgBpm(&s) == 150, "only the real sample counted");
    check(s.max_bpm == 150, "and the 250 did not become the max");
    printf("ok\n");

    printf("- a null ride is refused rather than crashed on: ");
    RideStatsCore_Reset(NULL);
    RideStatsCore_AddSpeed(NULL, 20.0f, 1.0);
    RideStatsCore_AddHeartRate(NULL, 150);
    check(close_to(RideStatsCore_AvgSpeedKmh(NULL), 0.0f), "no average from nothing");
    check(RideStatsCore_AvgBpm(NULL) == 0, "no bpm from nothing");
    printf("ok\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
