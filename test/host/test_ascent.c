// Host tests for src/system/Ascent.c -- total climbing from GNSS altitude.
//
// The case that matters is the first one. A sum of positive differences turns
// an hour of flat riding into hundreds of metres of climbing, and the number
// looks plausible enough that nobody questions it. Every other test here
// exists to make sure the filter that prevents that does not also throw away
// the hills.

#include <math.h>
#include <stdio.h>

#include "../../src/system/Ascent.h"

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
    if (fabs(got - want) > tol) {
        failures++;
        printf("\n    FAIL: %s (got %.1f, want %.1f +/- %.1f)", what, got, want, tol);
    }
}

// A cheap deterministic wobble, so the "flat" test is a real noise sequence
// rather than a constant. Amplitude in metres, peak to peak 2x.
static float Wobble(int i, float amplitude) {
    static const float shape[] = {0.0f, 0.7f, 1.0f, 0.6f, -0.2f, -0.9f, -1.0f, -0.5f};
    return amplitude * shape[i % 8];
}

int main(void) {
    Ascent_t a;

    printf("- an hour of flat riding with noisy fixes climbs nothing: ");
    Ascent_Reset(&a);
    for (int i = 0; i < 3600; i++) {
        // 100m above sea level, +/-3m of wander: an ordinary good fix.
        Ascent_Feed(&a, 100.0f + Wobble(i, 3.0f), (uint32_t)(i * 1000));
    }
    // The naive sum over this exact sequence is 2,698m of "climbing".
    check_near(Ascent_Metres(&a), 0.0, 0.001, "no ascent invented");
    check_near(Ascent_DescentMetres(&a), 0.0, 0.001, "and none descended");
    printf("done\n");

    printf("- a real climb is counted, within a band of the truth: ");
    Ascent_Reset(&a);
    {
        // 200m of climbing at 1 m/s, one fix a second, with noise on top.
        for (int i = 0; i <= 200; i++) {
            Ascent_Feed(&a, 100.0f + (float)i + Wobble(i, 1.5f), (uint32_t)(i * 1000));
        }
        // The smoothing lags and the band swallows up to its own width, so
        // the answer is a little short. Short and honest beats long and
        // invented.
        check_near(Ascent_Metres(&a), 200.0, 12.0, "200m climb measured");
        check_near(Ascent_DescentMetres(&a), 0.0, 1.0, "with no descent");
    }
    printf("done\n");

    printf("- an out and back climbs and drops the same amount: ");
    Ascent_Reset(&a);
    {
        int t = 0;
        for (int i = 0; i <= 150; i++, t++) {
            Ascent_Feed(&a, 50.0f + (float)i, (uint32_t)(t * 1000));
        }
        for (int i = 150; i >= 0; i--, t++) {
            Ascent_Feed(&a, 50.0f + (float)i, (uint32_t)(t * 1000));
        }
        check_near(Ascent_Metres(&a), 150.0, 10.0, "up 150m");
        check_near(Ascent_DescentMetres(&a), 150.0, 10.0, "and down 150m");
        // Ending where it began means the two must agree, and they do to
        // within one band. The residue is inherent rather than a bug: at the
        // end of the ride the smoothed altitude sits somewhere inside the band
        // below the reference, and that last part-band is never counted. One
        // band is the bound, and a rule that treated the two directions
        // differently would blow straight through it.
        const double imbalance = Ascent_Metres(&a) - Ascent_DescentMetres(&a);
        check(fabs(imbalance) <= ASCENT_BAND_M, "up and down agree to within one band");
    }
    printf("done\n");

    printf("- a single wild fix is rejected, not climbed: ");
    Ascent_Reset(&a);
    {
        for (int i = 0; i < 30; i++) {
            Ascent_Feed(&a, 100.0f, (uint32_t)(i * 1000));
        }
        // 40m in one second. No rider does this; a receiver does it often.
        check(!Ascent_Feed(&a, 140.0f, 30000), "the spike is refused");
        // ...and the good sample after it is still accepted, which is the
        // part a naive "remember the last value" filter gets wrong.
        check(Ascent_Feed(&a, 100.0f, 31000), "the next good sample still counts");
        check_near(Ascent_Metres(&a), 0.0, 0.001, "and nothing was climbed");
    }
    printf("done\n");

    printf("- altitudes that are not places are refused: ");
    Ascent_Reset(&a);
    check(!Ascent_Feed(&a, -2000.0f, 0), "two kilometres underground");
    check(!Ascent_Feed(&a, 30000.0f, 1000), "in the stratosphere");
    check(!Ascent_Feed(&a, NAN, 2000), "not a number at all");
    check_near(Ascent_Metres(&a), 0.0, 0.001, "none of them counted");
    // Below sea level is a real place people ride.
    check(Ascent_Feed(&a, -400.0f, 3000), "but the Dead Sea road is fine");
    printf("done\n");

    printf("- a long gap is a lost fix, not a spike: ");
    Ascent_Reset(&a);
    {
        Ascent_Feed(&a, 100.0f, 0);
        // Five minutes later, 300m higher: the rider drove up a hill with the
        // receiver indoors. The rate test must not reject this, because over
        // five minutes it is not a rate.
        check(Ascent_Feed(&a, 400.0f, 300000), "accepted across the gap");
    }
    printf("done\n");

    printf("- the rate test survives a millis() wrap: ");
    Ascent_Reset(&a);
    {
        Ascent_Feed(&a, 100.0f, 0xFFFFFF00u);
        // 512ms later, past the wrap. Unsigned subtraction gets this right;
        // signed arithmetic would see a gap of minus 49 days.
        check(Ascent_Feed(&a, 101.0f, 0x000000FFu), "a normal sample straddling the wrap");
        check(!Ascent_Feed(&a, 141.0f, 0x000001FFu), "and a spike is still a spike");
    }
    printf("done\n");

    printf("- a staircase of small rises adds up: ");
    Ascent_Reset(&a);
    {
        // Ten rises of 20m, each well clear of the band. This is the case a
        // band that was too wide would quietly lose.
        float alt = 100.0f;
        uint32_t t = 0;
        for (int step = 0; step < 10; step++) {
            for (int i = 0; i < 40; i++, t += 1000) {
                alt += 0.5f;
                Ascent_Feed(&a, alt, t);
            }
            for (int i = 0; i < 20; i++, t += 1000) {
                Ascent_Feed(&a, alt, t);
            }
        }
        check_near(Ascent_Metres(&a), 200.0, 15.0, "ten rises of twenty metres");
    }
    printf("done\n");

    printf("- reset clears everything, and nulls are refused: ");
    Ascent_Reset(&a);
    check_near(Ascent_Metres(&a), 0.0, 0.001, "ascent cleared");
    check_near(Ascent_DescentMetres(&a), 0.0, 0.001, "descent cleared");
    check(!Ascent_Feed(NULL, 100.0f, 0), "a null accumulator is refused");
    check_near(Ascent_Metres(NULL), 0.0, 0.001, "and reads as zero");
    check_near(Ascent_DescentMetres(NULL), 0.0, 0.001, "both of them");
    Ascent_Reset(NULL); // must not crash
    printf("done\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
