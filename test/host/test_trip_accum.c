// Host tests for src/system/TripAccum.c -- the rule deciding which GPS steps
// count towards trip distance.
//
// This logic was inside Page_Dashboard until ride logging needed it too, where
// no test could reach it.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../src/system/TripAccum.h"

static int checks;
static int failures;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("\n    FAIL: %s", what);
    }
}

static void check_close(double got, double want, double tol, const char *what) {
    checks++;
    if (fabs(got - want) > tol) {
        failures++;
        printf("\n    FAIL: %s (got %.3f, want %.3f)", what, got, want);
    }
}

// One metre of latitude, for building steps of a known size.
//
// Derived from the same sphere TripAccum uses (2*pi*6371000/360 metres per
// degree) rather than the textbook 111320, which is a different ellipsoid: the
// 0.1% difference is invisible in most assertions but lands exactly on the
// TRIP_MIN_STEP_M boundary, where it turns a step meant to be 1.0m into 0.9989m
// and silently flips it to the rejected side.
#define METRE_LAT (1.0 / 111194.9266)

int main(void) {
    TripAccum_t trip;

    printf("- distance matches known separations: ");
    // One degree of latitude is ~111.32 km anywhere on the globe.
    check_close(TripAccum_DistanceMetres(0.0, 0.0, 1.0, 0.0), 111195.0, 200.0, "1 degree lat");
    check_close(TripAccum_DistanceMetres(51.5, 0.0, 51.5, 0.0), 0.0, 0.001, "zero distance");
    // Longitude compresses with latitude: at 60 degrees it is half.
    check_close(TripAccum_DistanceMetres(60.0, 0.0, 60.0, 1.0),
                TripAccum_DistanceMetres(0.0, 0.0, 0.0, 1.0) / 2.0, 500.0, "cos(lat) scaling");
    printf("done\n");

    printf("- the first fix establishes an origin, it does not add distance: ");
    TripAccum_Init(&trip, 0.0);
    check_close(TripAccum_AddFix(&trip, true, 51.5, -0.1), 0.0, 1e-9, "first fix adds nothing");
    check_close(TripAccum_Metres(&trip), 0.0, 1e-9, "total still zero");
    printf("done\n");

    printf("- ordinary movement accumulates: ");
    TripAccum_Init(&trip, 0.0);
    TripAccum_AddFix(&trip, true, 0.0, 0.0);
    for (int i = 1; i <= 10; i++) {
        TripAccum_AddFix(&trip, true, i * 10.0 * METRE_LAT, 0.0);
    }
    check_close(TripAccum_Metres(&trip), 100.0, 1.0, "ten 10m steps make 100m");
    printf("done\n");

    printf("- a fix with no lock is ignored entirely: ");
    TripAccum_Init(&trip, 0.0);
    TripAccum_AddFix(&trip, true, 0.0, 0.0);
    check_close(TripAccum_AddFix(&trip, false, 1.0, 1.0), 0.0, 1e-9, "no-fix adds nothing");
    check_close(TripAccum_Metres(&trip), 0.0, 1e-9, "total unchanged");
    // ...and it must not have moved the origin either, or the next real fix
    // would measure from a position that was never trusted.
    check_close(TripAccum_AddFix(&trip, true, 10.0 * METRE_LAT, 0.0), 10.0, 1.0,
                "origin survived the bad fix");
    printf("done\n");

    printf("- a cold-fix jump is taken as position but not as distance: ");
    TripAccum_Init(&trip, 0.0);
    TripAccum_AddFix(&trip, true, 51.5, -0.1);
    check_close(TripAccum_AddFix(&trip, true, 48.85, 2.35), 0.0, 1e-9, "London to Paris ignored");
    check_close(TripAccum_Metres(&trip), 0.0, 1e-9, "no distance credited");
    // The new position is now the reference: a 10m step from Paris counts.
    check_close(TripAccum_AddFix(&trip, true, 48.85 + 10.0 * METRE_LAT, 2.35), 10.0, 1.0,
                "measures from the new position");
    printf("done\n");

    printf("- sub-metre wander does not creep, but slow movement still counts: ");
    TripAccum_Init(&trip, 0.0);
    TripAccum_AddFix(&trip, true, 0.0, 0.0);
    // A parked bike: 100 samples of jitter well under the floor.
    for (int i = 0; i < 100; i++) {
        const double wander = ((i % 2) == 0 ? 0.4 : -0.4) * METRE_LAT;
        TripAccum_AddFix(&trip, true, wander, 0.0);
    }
    check_close(TripAccum_Metres(&trip), 0.0, 1e-9, "stationary adds nothing");

    // Genuine slow movement: each step is below the floor, but they are all in
    // the same direction, so the origin must not follow them -- otherwise the
    // rider covers ground the odometer never sees.
    //
    // 0.6m steps rather than 0.5m to sit clearly off the 1.0m boundary: at
    // exactly half the floor, whether a pair of steps lands on 0.99999 or
    // 1.00001 is a rounding accident, and the test would be asserting the
    // behaviour of double arithmetic rather than of the accumulator.
    TripAccum_Init(&trip, 0.0);
    TripAccum_AddFix(&trip, true, 0.0, 0.0);
    for (int i = 1; i <= 20; i++) {
        TripAccum_AddFix(&trip, true, i * 0.6 * METRE_LAT, 0.0);
    }
    // Twelve metres covered in twenty sub-floor steps, credited in ten pairs.
    // Up to one floor-width can still be pending at any moment -- that is the
    // floor's inherent lag, not lost distance, and it is why the tolerance
    // here is a metre rather than a centimetre.
    check_close(TripAccum_Metres(&trip), 12.0, 1.0, "twenty 0.6m steps still make 12m");
    printf("done\n");

    printf("- a saved total resumes without inventing a step: ");
    TripAccum_Init(&trip, 12345.0);
    check_close(TripAccum_Metres(&trip), 12345.0, 1e-9, "resumes the saved distance");
    // The rider may have been carried a long way while switched off; the first
    // fix after a resume must not count that gap.
    check_close(TripAccum_AddFix(&trip, true, 48.85, 2.35), 0.0, 1e-9, "first fix adds nothing");
    check_close(TripAccum_Metres(&trip), 12345.0, 1e-9, "still just the saved distance");
    printf("done\n");

    printf("- a corrupt saved total is refused rather than displayed: ");
    TripAccum_Init(&trip, -5.0);
    check_close(TripAccum_Metres(&trip), 0.0, 1e-9, "negative rejected");
    TripAccum_Init(&trip, NAN);
    check_close(TripAccum_Metres(&trip), 0.0, 1e-9, "NaN rejected");
    TripAccum_Init(&trip, 1.0e12);
    check_close(TripAccum_Metres(&trip), 0.0, 1e-9, "absurd value rejected");
    printf("done\n");

    printf("- reset clears the total and the origin: ");
    TripAccum_Init(&trip, 0.0);
    TripAccum_AddFix(&trip, true, 0.0, 0.0);
    TripAccum_AddFix(&trip, true, 100.0 * METRE_LAT, 0.0);
    check(TripAccum_Metres(&trip) > 50.0, "distance accumulated before reset");
    TripAccum_Reset(&trip);
    check_close(TripAccum_Metres(&trip), 0.0, 1e-9, "total zeroed");
    // The next fix must start a fresh measurement, not measure the gap back to
    // wherever the rider was when they pressed reset.
    check_close(TripAccum_AddFix(&trip, true, 1000.0 * METRE_LAT, 0.0), 0.0, 1e-9,
                "next fix re-establishes the origin");
    printf("done\n");

    printf("- null is refused rather than crashing: ");
    TripAccum_Init(NULL, 0.0);
    TripAccum_Reset(NULL);
    check_close(TripAccum_AddFix(NULL, true, 1.0, 1.0), 0.0, 1e-9, "null add");
    check_close(TripAccum_Metres(NULL), 0.0, 1e-9, "null read");
    printf("done\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
