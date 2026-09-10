/* Host tests for the breadcrumb store (src/navigation/TrackBuffer.c).
 *
 * The thinning is the interesting part: it has to bound memory for a file
 * whose length is unknown until it has been read, without distorting the
 * shape of the trail. */
#include <math.h>
#include <stdio.h>

#include "TrackBuffer.h"

static int checks = 0;
static int failures = 0;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_close(double got, double want, double tol, const char *what) {
    checks++;
    if (fabs(got - want) > tol) {
        failures++;
        printf("  FAIL: %s (got %.7f, want %.7f)\n", what, got, want);
    }
}

#define CAP 8
static int32_t lat_store[CAP];
static int32_t lon_store[CAP];

int main(void) {
    TrackBuffer_t track;
    double lat;
    double lon;
    size_t i;

    printf("- points round-trip through the e7 representation: ");
    TrackBuffer_Init(&track, lat_store, lon_store, CAP);
    TrackBuffer_Add(&track, 38.8821000, -77.0194000);
    check(track.count == 1, "stored");
    check(TrackBuffer_Get(&track, 0, &lat, &lon), "readable");
    check_close(lat, 38.8821, 1e-7, "latitude survives");
    check_close(lon, -77.0194, 1e-7, "longitude survives");
    check(!TrackBuffer_Get(&track, 1, &lat, &lon), "index past the end refused");
    printf("done\n");

    printf("- rounding is symmetric, not biased toward zero: ");
    TrackBuffer_Init(&track, lat_store, lon_store, CAP);
    /* Truncation would pull both of these toward the equator/meridian, which
       accumulates as drift along a trail rather than cancelling out. */
    TrackBuffer_Add(&track, 0.00000019, -0.00000019);
    TrackBuffer_Get(&track, 0, &lat, &lon);
    check_close(lat, 0.0000002, 1e-9, "positive rounds up");
    check_close(lon, -0.0000002, 1e-9, "negative rounds down");
    printf("done\n");

    printf("- capacity is never exceeded, however many points arrive: ");
    TrackBuffer_Init(&track, lat_store, lon_store, CAP);
    for (i = 0; i < 1000; i++) {
        TrackBuffer_Add(&track, 10.0 + (double)i * 0.001, 20.0 + (double)i * 0.001);
    }
    check(track.count <= CAP, "count stayed within capacity");
    check(track.stride > 1, "stride grew to compensate");
    printf("done\n");

    printf("- the start of the trail is never dropped: ");
    TrackBuffer_Init(&track, lat_store, lon_store, CAP);
    for (i = 0; i < 100; i++) {
        TrackBuffer_Add(&track, 10.0 + (double)i, 20.0 + (double)i);
    }
    TrackBuffer_Get(&track, 0, &lat, &lon);
    /* Halving keeps index 0, so the first point survives every generation of
       thinning -- otherwise the trail would appear to start somewhere else. */
    check_close(lat, 10.0, 1e-6, "first point is still the first point");
    printf("done\n");

    printf("- thinning keeps points evenly spread, not clustered: ");
    TrackBuffer_Init(&track, lat_store, lon_store, CAP);
    for (i = 0; i < 64; i++) {
        TrackBuffer_Add(&track, (double)i, 0.0);
    }
    {
        double first;
        double last;
        double prev;
        double gap;
        int even = 1;
        TrackBuffer_Get(&track, 0, &first, &lon);
        TrackBuffer_Get(&track, track.count - 1, &last, &lon);
        TrackBuffer_Get(&track, 0, &prev, &lon);
        TrackBuffer_Get(&track, 1, &gap, &lon);
        gap = gap - prev;
        for (i = 1; i < track.count; i++) {
            double current;
            TrackBuffer_Get(&track, i, &current, &lon);
            if (fabs((current - prev) - gap) > 1e-6) {
                even = 0;
            }
            prev = current;
        }
        check(even, "spacing between kept points is uniform");
        check(last > first, "and the trail still spans forward");
    }
    printf("done\n");

    printf("- bounds cover every point offered, not just those kept: ");
    TrackBuffer_Init(&track, lat_store, lon_store, CAP);
    for (i = 0; i < 100; i++) {
        TrackBuffer_Add(&track, 10.0 + (double)i * 0.1, -20.0 - (double)i * 0.1);
    }
    /* The last point is almost certainly thinned away, but framing the view
       from the bounds must still include it. */
    check_close((double)track.max_lat_e7 * 1e-7, 19.9, 1e-6, "max latitude includes the last point");
    check_close((double)track.min_lat_e7 * 1e-7, 10.0, 1e-6, "min latitude is the first");
    check(TrackBuffer_Center(&track, &lat, &lon), "centre available");
    check_close(lat, 14.95, 1e-6, "centre latitude");
    check_close(lon, -24.95, 1e-6, "centre longitude");
    printf("done\n");

    printf("- an empty track reports no centre: ");
    TrackBuffer_Init(&track, lat_store, lon_store, CAP);
    check(!TrackBuffer_Center(&track, &lat, &lon), "no bounds yet");
    check(!TrackBuffer_Get(&track, 0, &lat, &lon), "nothing to read");
    printf("done\n");

    printf("- null storage is refused rather than crashing: ");
    TrackBuffer_Init(&track, NULL, NULL, CAP);
    check(track.capacity == 0, "capacity forced to zero");
    TrackBuffer_Add(&track, 1.0, 2.0); /* must not write through NULL */
    check(track.count == 0, "add is a no-op");
    TrackBuffer_Init(NULL, lat_store, lon_store, CAP);
    TrackBuffer_Add(NULL, 1.0, 2.0);
    printf("done\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
