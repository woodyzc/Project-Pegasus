/* Host tests for the breadcrumb projection (src/navigation/MapProject.c).
 *
 * Geometry is easy to get subtly wrong in ways that still look like a map:
 * an inverted axis, a missing cos(latitude), a scale that fits one axis and
 * clips the other. Each is pinned separately below. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "MapProject.h"

static int checks = 0;
static int failures = 0;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_near(int got, int want, int tol, const char *what) {
    checks++;
    if (abs(got - want) > tol) {
        failures++;
        printf("  FAIL: %s (got %d, want %d)\n", what, got, want);
    }
}

#define CAP 512
static int32_t lat_store[CAP];
static int32_t lon_store[CAP];
static MapPoint_t poly[CAP];

int main(void) {
    TrackBuffer_t track;
    int16_t x;
    int16_t y;

    printf("- the view centre projects to the centre pixel: ");
    Map_Project(38.8821, -77.0194, 38.8821, -77.0194, 1.0, 120, 160, &x, &y);
    check(x == 120 && y == 160, "centre maps to centre");
    printf("done\n");

    printf("- north is up and east is right: ");
    /* 100m north at 1 m/px must be 100px UP the screen, i.e. a SMALLER y --
       latitude grows north while screen y grows downward. */
    Map_Project(38.8821 + 100.0 / MAP_EARTH_METRES_PER_DEGREE, -77.0194, 38.8821, -77.0194, 1.0,
                120, 160, &x, &y);
    check_near(y, 60, 1, "100m north is 100px up");
    check_near(x, 120, 1, "and does not move sideways");

    Map_Project(38.8821, -77.0194 + 100.0 / (MAP_EARTH_METRES_PER_DEGREE * cos(38.8821 * M_PI / 180.0)),
                38.8821, -77.0194, 1.0, 120, 160, &x, &y);
    check_near(x, 220, 1, "100m east is 100px right");
    check_near(y, 160, 1, "and does not move vertically");
    printf("done\n");

    printf("- longitude is scaled by cos(latitude): ");
    {
        int16_t x_equator;
        int16_t x_north;
        int16_t dummy;
        /* The same longitude delta covers far less ground at 60N than at the
           equator. Without the cosine the trail would be stretched sideways
           by a factor of two up there. */
        Map_Project(0.0, 0.01, 0.0, 0.0, 1.0, 0, 0, &x_equator, &dummy);
        Map_Project(60.0, 0.01, 60.0, 0.0, 1.0, 0, 0, &x_north, &dummy);
        check(x_north < x_equator, "same delta is narrower at 60N");
        /* cos(60) = 0.5 exactly, so it should be about half. */
        check_near(x_north, x_equator / 2, 2, "and about half as wide");
    }
    printf("done\n");

    printf("- scale is chosen by the tighter axis: ");
    {
        /* A track wide in longitude but shallow in latitude must be fitted by
           its width, or it runs off the sides. */
        double wide = Map_FitScale(38.88, 38.881, -77.05, -77.00, 240, 320, 10);
        double tall = Map_FitScale(38.80, 38.90, -77.02, -77.019, 240, 320, 10);
        double span_lon_m = 0.05 * MAP_EARTH_METRES_PER_DEGREE * cos(38.88 * M_PI / 180.0);
        double span_lat_m = 0.10 * MAP_EARTH_METRES_PER_DEGREE;
        check(fabs(wide - span_lon_m / 220.0) < 1.0, "wide track fitted by width");
        check(fabs(tall - span_lat_m / 300.0) < 1.0, "tall track fitted by height");
    }
    printf("done\n");

    printf("- a fitted track lands inside the viewport: ");
    {
        const double min_lat = 38.870, max_lat = 38.895;
        const double min_lon = -77.040, max_lon = -77.000;
        const double mpp = Map_FitScale(min_lat, max_lat, min_lon, max_lon, 240, 320, 10);
        const double clat = (min_lat + max_lat) / 2.0;
        const double clon = (min_lon + max_lon) / 2.0;
        int inside = 1;
        double lat;
        double lon;
        /* Walk the four corners of the bounding box. */
        for (lat = min_lat; lat <= max_lat + 1e-9; lat += (max_lat - min_lat)) {
            for (lon = min_lon; lon <= max_lon + 1e-9; lon += (max_lon - min_lon)) {
                Map_Project(lat, lon, clat, clon, mpp, 120, 160, &x, &y);
                if (x < 0 || x > 240 || y < 0 || y > 320) {
                    inside = 0;
                }
            }
        }
        check(inside, "every corner of the bounds is on screen");
    }
    printf("done\n");

    printf("- a zero-extent track still gets a usable scale: ");
    {
        /* A rider standing still, or a one-point file: the span is zero and a
           naive fit would divide by it. */
        double mpp = Map_FitScale(38.88, 38.88, -77.02, -77.02, 240, 320, 10);
        check(mpp > 0.0, "scale is positive");
        Map_Project(38.88, -77.02, 38.88, -77.02, mpp, 120, 160, &x, &y);
        check(x == 120 && y == 160, "and projection still works");
    }
    printf("done\n");

    printf("- far-away points clamp instead of wrapping: ");
    /* A point on another continent at 1 m/px would be tens of millions of
       pixels away; int16 would wrap it back across the screen as a line to
       nowhere. */
    Map_Project(0.0, 0.0, 38.8821, -77.0194, 1.0, 120, 160, &x, &y);
    check(x == -MAP_COORD_LIMIT || x == MAP_COORD_LIMIT, "x clamped to the limit");
    check(y == MAP_COORD_LIMIT, "y clamped, still pointing south");
    printf("done\n");

    printf("- the polyline collapses points sharing a pixel: ");
    TrackBuffer_Init(&track, lat_store, lon_store, CAP);
    {
        int i;
        /* 200 points at a standstill, then one 50m away. */
        for (i = 0; i < 200; i++) {
            TrackBuffer_Add(&track, 38.8821, -77.0194);
        }
        TrackBuffer_Add(&track, 38.8821 + 50.0 / MAP_EARTH_METRES_PER_DEGREE, -77.0194);
    }
    {
        size_t n = Map_BuildPolyline(&track, 38.8821, -77.0194, 1.0, 120, 160, poly, CAP);
        check(n == 2, "200 stationary points collapse to one");
        check(poly[0].x == 120 && poly[0].y == 160, "first is the centre");
        check_near(poly[1].y, 110, 1, "second is 50px north");
    }
    printf("done\n");

    printf("- the polyline respects its output limit: ");
    TrackBuffer_Init(&track, lat_store, lon_store, CAP);
    {
        int i;
        for (i = 0; i < 400; i++) {
            TrackBuffer_Add(&track, 38.80 + (double)i * 0.001, -77.0);
        }
        check(Map_BuildPolyline(&track, 38.85, -77.0, 1.0, 120, 160, poly, 16) == 16,
              "stops at max_points");
        check(Map_BuildPolyline(&track, 38.85, -77.0, 1.0, 120, 160, poly, 0) == 0,
              "zero capacity writes nothing");
    }
    printf("done\n");

    printf("- null arguments: ");
    check(Map_BuildPolyline(NULL, 0, 0, 1.0, 0, 0, poly, CAP) == 0, "null track");
    check(Map_BuildPolyline(&track, 0, 0, 1.0, 0, 0, NULL, CAP) == 0, "null output");
    Map_Project(0, 0, 0, 0, 1.0, 0, 0, NULL, &y); /* must not crash */
    printf("done\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
