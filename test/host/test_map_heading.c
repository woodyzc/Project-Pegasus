// Host tests for src/navigation/MapHeading.c -- which way is up on a track-up
// map.
//
// The wrap at north is the case worth having: a smoother written the obvious
// way takes 350 towards 10 through 180, which on screen is the whole map
// spinning a half-turn because the rider drifted a few degrees.

#include <math.h>
#include <stdio.h>

#include "../../src/navigation/MapHeading.h"

static int checks;
static int failures;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("\n    FAIL: %s", what);
    }
}

static void check_near(float got, float want, float tol, const char *what) {
    checks++;
    if (fabsf(got - want) > tol) {
        failures++;
        printf("\n    FAIL: %s (got %.1f, want %.1f)", what, got, want);
    }
}

// Riding at 5 m/s, which is clear of the floor.
static bool feed(MapHeading_t *h, float deg) {
    return MapHeading_Feed(h, true, 5.0f, deg);
}

int main(void) {
    MapHeading_t h;

    printf("- nothing is known until the rider moves: ");
    MapHeading_Reset(&h);
    check(!MapHeading_Valid(&h), "no heading at rest");
    check_near(MapHeading_Degrees(&h), 0.0f, 0.001f, "and it reads as north");
    // No fix, and a fix while stationary, are both refused.
    check(!MapHeading_Feed(&h, false, 5.0f, 90.0f), "no fix, no heading");
    check(!MapHeading_Feed(&h, true, 0.2f, 90.0f), "a fix while stopped is refused");
    check(!MapHeading_Valid(&h), "so the map stays north-up");
    printf("done\n");

    printf("- the first heading while moving is taken whole: ");
    MapHeading_Reset(&h);
    check(feed(&h, 90.0f), "and asks for a redraw");
    check(MapHeading_Valid(&h), "now valid");
    // Not smoothed towards north from nothing: there is nothing to smooth from,
    // and starting at 0 would swing the map a quarter turn on the first fix.
    check_near(MapHeading_Degrees(&h), 90.0f, 0.001f, "taken exactly, not eased into");
    printf("done\n");

    printf("- a stop holds the last heading rather than spinning: ");
    MapHeading_Reset(&h);
    feed(&h, 180.0f);
    for (int i = 0; i < 40; i++) {
        // A stationary receiver reports the direction of its own noise, which
        // covers the whole circle within seconds.
        MapHeading_Feed(&h, true, 0.1f, (float)((i * 37) % 360));
    }
    check_near(MapHeading_Degrees(&h), 180.0f, 0.001f, "unmoved through forty bad samples");
    printf("done\n");

    printf("- smoothing crosses north the short way: ");
    MapHeading_Reset(&h);
    feed(&h, 350.0f);
    feed(&h, 10.0f);
    // A quarter of the way from 350 to 10 is 355, not 260.
    check_near(MapHeading_Degrees(&h), 355.0f, 0.5f, "350 to 10 goes through 0");
    check(MapHeading_Degrees(&h) >= 0.0f && MapHeading_Degrees(&h) < 360.0f, "and stays in range");

    MapHeading_Reset(&h);
    feed(&h, 10.0f);
    feed(&h, 350.0f);
    check_near(MapHeading_Degrees(&h), 5.0f, 0.5f, "and the same in reverse");
    printf("done\n");

    printf("- the delta is signed and wrapped: ");
    check_near(MapHeading_Delta(10.0f, 350.0f), 20.0f, 0.001f, "across north, forwards");
    check_near(MapHeading_Delta(350.0f, 10.0f), -20.0f, 0.001f, "across north, backwards");
    check_near(MapHeading_Delta(90.0f, 80.0f), 10.0f, 0.001f, "an ordinary difference");
    check_near(MapHeading_Delta(0.0f, 0.0f), 0.0f, 0.001f, "no difference at all");
    // Exactly opposite has two equally short ways round; the rule has to pick
    // one and stick to it, or the map would flip between them.
    check_near(fabsf(MapHeading_Delta(180.0f, 0.0f)), 180.0f, 0.001f, "diametrically opposite");
    printf("done\n");

    printf("- a turn settles, and does not overshoot: ");
    MapHeading_Reset(&h);
    feed(&h, 0.0f);
    for (int i = 0; i < 40; i++) { feed(&h, 90.0f); }
    check_near(MapHeading_Degrees(&h), 90.0f, 1.0f, "a right turn arrives at 90");
    printf("done\n");

    printf("- small wobbles do not ask for a redraw: ");
    MapHeading_Reset(&h);
    feed(&h, 90.0f);
    {
        int redraws = 0;
        for (int i = 0; i < 60; i++) {
            // Half a degree either side: a real receiver, a straight road.
            if (feed(&h, 90.0f + ((i % 2) ? 0.5f : -0.5f))) { redraws++; }
        }
        check(redraws == 0, "sixty jittering fixes, no redraws");
    }
    // ...but a real turn does.
    check(feed(&h, 140.0f), "a genuine turn asks for one");
    printf("done\n");

    printf("- a heading that is not a heading is refused: ");
    MapHeading_Reset(&h);
    check(!MapHeading_Feed(&h, true, 5.0f, -1.0f), "negative");
    check(!MapHeading_Feed(&h, true, 5.0f, 361.0f), "past a full circle");
    check(!MapHeading_Feed(&h, true, 5.0f, NAN), "not a number");
    check(!MapHeading_Valid(&h), "none of them counted");
    check(feed(&h, 360.0f), "but exactly 360 is north and is fine");
    check_near(MapHeading_Degrees(&h), 0.0f, 0.001f, "normalised to 0");
    printf("done\n");

    printf("- nulls are refused rather than crashed on: ");
    check(!MapHeading_Feed(NULL, true, 5.0f, 90.0f), "feeding a null");
    check(!MapHeading_Valid(NULL), "asking a null");
    check_near(MapHeading_Degrees(NULL), 0.0f, 0.001f, "reading a null");
    MapHeading_Reset(NULL);
    printf("done\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
