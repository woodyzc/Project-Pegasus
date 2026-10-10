// Host tests for src/navigation/TrackProgress.c -- how far along the loaded
// GPX the rider has got, which is where the map's trail turns from grey to
// magenta.
//
// The mark only moves forward, so the failures that matter are the ones that
// move it forward WRONGLY: there is no taking it back. The first version
// picked the nearest vertex anywhere on the track and greyed out whole
// stretches the moment a later leg came close (2026-10-09, on the road). Every
// case below that is about a sequence walks the whole sequence, feeding each
// fix in turn -- a single-fix test cannot see a mark that creeps.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../src/navigation/TrackBuffer.h"
#include "../../src/navigation/TrackProgress.h"

static int checks;
static int failures;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("\n    FAIL: %s", what);
    }
}

// Germantown, MD -- the riding area -- so the longitude scale is a real one.
static const double ORIGIN_LAT = 39.17;
static const double ORIGIN_LON = -77.27;

#define CAP 4096
static int32_t s_lat[CAP];
static int32_t s_lon[CAP];
static TrackBuffer_t s_track;

static double LatOf(double y_m) {
    return ORIGIN_LAT + y_m / 111320.0;
}

static double LonOf(double x_m) {
    return ORIGIN_LON + x_m / (111320.0 * cos(ORIGIN_LAT * M_PI / 180.0));
}

static void TrackBegin(void) {
    TrackBuffer_Init(&s_track, s_lat, s_lon, CAP);
}

static void TrackAdd(double x_m, double y_m) {
    TrackBuffer_Add(&s_track, LatOf(y_m), LonOf(x_m));
}

// A straight run of points from (x0,y0) to (x1,y1), every `step` metres,
// including both ends unless `skip_first`.
static void TrackRun(double x0, double y0, double x1, double y1, double step, int skip_first) {
    const double len = sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    const int n = (int)(len / step + 0.5);
    for (int i = skip_first ? 1 : 0; i <= n; i++) {
        const double f = (n == 0) ? 0.0 : (double)i / n;
        TrackAdd(x0 + f * (x1 - x0), y0 + f * (y1 - y0));
    }
}

static bool Feed(TrackProgress_t *p, double x_m, double y_m) {
    const int32_t lat = (int32_t)lround(LatOf(y_m) * 1e7);
    const int32_t lon = (int32_t)lround(LonOf(x_m) * 1e7);
    return TrackProgress_Feed(p, &s_track, lat, lon);
}

// The track vertex's own position, for asserting where a mark landed.
static double VertexY(size_t i) {
    return ((double)s_track.lat_e7[i] * 1e-7 - ORIGIN_LAT) * 111320.0;
}

int main(void) {
    TrackProgress_t p;

    printf("- nothing to follow on an empty or one-point track: ");
    TrackBegin();
    TrackProgress_Reset(&p);
    check(!Feed(&p, 0, 0), "empty track");
    TrackAdd(0, 0);
    for (int i = 0; i < 10; i++) {
        Feed(&p, 0, 0);
    }
    check(!p.have_mark, "one point is not a line");
    printf("done\n");

    // ---------------------------------------------------------------------
    // The case that was seen on the road.
    //
    // North along x=0 with vertices 500m apart -- what a planner emits for a
    // straight road -- then across and back south along a parallel street
    // 40m east, densely sampled. Riding the first leg, the rider is up to
    // 250m from the nearest vertex of their own leg and 40m from a vertex of
    // the return leg. Nearest-vertex marked the return leg, which greys out
    // everything; it must stay on the leg actually being ridden.
    // ---------------------------------------------------------------------
    printf("- a nearby later leg does not steal the mark: ");
    TrackBegin();
    TrackRun(0, 0, 0, 2000, 500, 0);       // indices 0..4
    TrackRun(0, 2000, 40, 2000, 40, 1);    // 5
    TrackRun(40, 2000, 40, 0, 10, 1);      // 6..205
    TrackProgress_Reset(&p);
    {
        size_t worst = 0;
        for (double y = 0; y <= 1990; y += 7) {
            Feed(&p, 0, y);
            if (p.have_mark && p.mark > worst) {
                worst = p.mark;
            }
        }
        check(p.have_mark, "acquired on the first leg");
        check(worst <= 3, "never past the first leg while riding it");
        check(VertexY(p.mark) <= 1990, "and the mark is behind the rider");
    }
    printf("done\n");

    printf("- ...and progress carries on round the corner and down the return: ");
    for (double x = 0; x <= 40; x += 7) {
        Feed(&p, x, 2000);
    }
    for (double y = 2000; y >= 600; y -= 7) {
        Feed(&p, 40, y);
    }
    check(p.mark > 5, "onto the return leg");
    check(fabs(VertexY(p.mark) - 600) <= 20, "and level with the rider on it");
    printf("done\n");

    // ---------------------------------------------------------------------
    // An exact out-and-back: the same points out and back, which is what a
    // planner returns for a there-and-back. Both legs are on the line
    // everywhere, so only memory can say which one the rider is on.
    // ---------------------------------------------------------------------
    printf("- an out-and-back is followed out, through the turn, and home: ");
    TrackBegin();
    TrackRun(0, 0, 0, 1000, 20, 0);     // 0..50, out
    TrackRun(0, 1000, 0, 0, 20, 1);     // 51..100, back
    TrackProgress_Reset(&p);
    {
        bool ok_out = true;
        for (double y = 0; y <= 990; y += 6) {
            Feed(&p, 0, y);
            if (p.have_mark && p.mark > 50) {
                ok_out = false;
            }
        }
        check(ok_out, "the return leg is never marked on the way out");
        check(p.have_mark && p.mark >= 45, "and the outbound leg is");
        bool monotonic = true;
        size_t last = p.mark;
        for (double y = 1000; y >= 0; y -= 6) {
            Feed(&p, 0, y);
            if (p.mark < last) {
                monotonic = false;
            }
            last = p.mark;
        }
        check(monotonic, "never backwards");
        check(p.mark >= 95, "home again at the end of the route");
    }
    printf("done\n");

    // The map page is destroyed every time the rider leaves it, and the head
    // unit can be restarted mid-ride -- either way the search starts from
    // nothing, and here it starts on the way home. Both legs are on the line
    // under the rider. By position alone the earlier index wins, which is the
    // outbound leg, running the other way; the forward check then fails on
    // every fix and the rider is never picked up at all (found in review,
    // 2026-10-10: 600m ridden home with no mark).
    printf("- picked up on the way home, it is the way home: ");
    TrackProgress_Reset(&p);
    for (double y = 600; y >= 300; y -= 7) {
        Feed(&p, 0, y);
    }
    check(p.have_mark, "acquired on the return leg");
    check(p.have_mark && p.mark > 50, "the return leg, not the outbound one");
    check(p.have_mark && fabs(VertexY(p.mark) - 300) <= 30, "and level with the rider");
    printf("done\n");

    // Below the motion floor the direction is receiver wander, so the search
    // goes by position alone. That must still let a slow rider be picked up.
    printf("- walking the bike forwards is still picked up: ");
    TrackProgress_Reset(&p);
    for (double y = 0; y <= 40; y += 1.2) {
        Feed(&p, 0, y);
    }
    check(p.have_mark && p.mark <= 2, "acquired at the start without a direction");
    printf("done\n");

    // ---------------------------------------------------------------------
    // Stray fixes.
    // ---------------------------------------------------------------------
    printf("- one stray fix near a later stretch moves nothing: ");
    TrackBegin();
    TrackRun(0, 0, 0, 5000, 25, 0); // 0..200
    TrackProgress_Reset(&p);
    for (double y = 0; y <= 500; y += 7) {
        Feed(&p, 0, y);
    }
    {
        const size_t before = p.mark;
        check(!Feed(&p, 0, 4000), "a jump 3.5km ahead is not taken");
        check(p.mark == before, "the mark is where it was");
        Feed(&p, 0, 507);
        check(p.mark >= before && p.mark <= before + 2, "and tracking resumes");
    }
    printf("done\n");

    printf("- a sustained jump ahead (a shortcut, a gap) is taken, but not at once: ");
    {
        const size_t before = p.mark;
        int moved_at = -1;
        for (int k = 0; k < 10; k++) {
            if (Feed(&p, 0, 3000 + k * 7) && moved_at < 0) {
                moved_at = k;
            }
        }
        check(moved_at == TRACK_PROGRESS_ACQUIRE_FIXES - 1, "committed on the Nth fix");
        check(p.mark > before && fabs(VertexY(p.mark) - 3060) <= 30, "at the rider");
    }
    printf("done\n");

    printf("- off the track entirely, nothing happens: ");
    {
        const size_t before = p.mark;
        for (int k = 0; k < 20; k++) {
            Feed(&p, 120, 3100 + k * 7);
        }
        check(p.mark == before, "120m off is not on the route");
    }
    printf("done\n");

    printf("- never backwards, however long the rider sits behind the mark: ");
    {
        const size_t before = p.mark;
        for (int k = 0; k < 30; k++) {
            Feed(&p, 0, 100 + k * 7);
        }
        check(p.mark == before, "riding an earlier stretch again leaves it grey");
    }
    printf("done\n");

    // ---------------------------------------------------------------------
    // Direction.
    // ---------------------------------------------------------------------
    printf("- riding the route in reverse does not grey it out: ");
    TrackBegin();
    TrackRun(0, 0, 0, 3000, 25, 0);
    TrackProgress_Reset(&p);
    for (double y = 3000; y >= 0; y -= 7) {
        Feed(&p, 0, y);
    }
    check(!p.have_mark, "a rider going the wrong way never acquires");
    printf("done\n");

    printf("- crossing the route at a junction does not acquire it: ");
    TrackProgress_Reset(&p);
    for (double x = -200; x <= 200; x += 7) {
        Feed(&p, x, 1500);
    }
    check(!p.have_mark, "perpendicular passage");
    printf("done\n");

    printf("- standing at the start, then setting off: ");
    TrackProgress_Reset(&p);
    for (int k = 0; k < 20; k++) {
        Feed(&p, (k % 3) - 1.0, (k % 2) * 2.0); // a few metres of receiver wander
    }
    check(!p.have_mark || p.mark <= 1, "wander alone marks nothing of note");
    for (double y = 0; y <= 100; y += 7) {
        Feed(&p, 0, y);
    }
    check(p.have_mark, "acquired once actually moving forwards");
    printf("done\n");

    // ---------------------------------------------------------------------
    // A loop whose start and finish are the same place.
    // ---------------------------------------------------------------------
    printf("- a loop is picked up at its start, not its finish: ");
    TrackBegin();
    TrackRun(0, 0, 0, 1000, 20, 0);
    TrackRun(0, 1000, 1000, 1000, 20, 1);
    TrackRun(1000, 1000, 1000, 0, 20, 1);
    TrackRun(1000, 0, 0, 0, 20, 1);
    TrackProgress_Reset(&p);
    for (double y = 0; y <= 100; y += 7) {
        Feed(&p, 0, y);
    }
    check(p.have_mark && p.mark <= 6, "the first leg, not the last");
    printf("done\n");

    printf("%d checks, %d failures\n", checks, failures);
    if (failures != 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
