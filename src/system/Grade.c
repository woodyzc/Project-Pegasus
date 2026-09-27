#include "Grade.h"

#include <stddef.h>

// Ring accessor: i == 0 is the oldest live sample.
static GradeSample_t *At(Grade_t *g, uint8_t i) {
    return &g->s[(uint8_t)((g->oldest + i) % GRADE_SAMPLES)];
}

static void DropOldest(Grade_t *g) {
    g->oldest = (uint8_t)((g->oldest + 1) % GRADE_SAMPLES);
    g->count--;
}

void Grade_Reset(Grade_t *g) {
    if (g == NULL) {
        return;
    }
    g->oldest = 0;
    g->count = 0;
    g->grade_pct = 0.0f;
    g->have_grade = false;
}

bool Grade_Feed(Grade_t *g, double dist_m, float alt_m, uint32_t t_ms) {
    if (g == NULL) {
        return false;
    }

    // Distance running backwards is the odometer being reset, not a rider in
    // reverse. Starting over costs one window; carrying on would compute a
    // negative run and hand back a grade of the wrong sign.
    if (g->count > 0 && dist_m < At(g, (uint8_t)(g->count - 1))->dist_m) {
        Grade_Reset(g);
    }

    if (g->count == GRADE_SAMPLES) {
        DropOldest(g);
    }
    GradeSample_t *slot = At(g, g->count);
    slot->dist_m = dist_m;
    slot->alt_m = alt_m;
    slot->t_ms = t_ms;
    g->count++;

    // Unsigned subtraction, so a tick counter wrapping after 49 days still
    // yields the real age rather than an enormous one.
    while (g->count > 1 && (uint32_t)(t_ms - At(g, 0)->t_ms) > GRADE_MAX_WINDOW_MS) {
        DropOldest(g);
    }

    const GradeSample_t *newest = At(g, (uint8_t)(g->count - 1));

    // Walk backwards and take the FIRST sample far enough behind to be a
    // usable baseline -- i.e. the shortest baseline that satisfies
    // GRADE_MIN_RUN_M, not the longest available.
    //
    // That choice is what keeps the window roughly a fixed distance at any
    // speed. Taking the oldest qualifying sample instead would give a 300 m
    // baseline at 36 km/h, averaging the grade over terrain the rider has
    // finished with, and would make the reading lag more the faster they go.
    for (int i = (int)g->count - 2; i >= 0; i--) {
        const GradeSample_t *old = At(g, (uint8_t)i);
        const double run = newest->dist_m - old->dist_m;
        if (run < (double)GRADE_MIN_RUN_M) {
            continue;
        }

        float raw = (float)((((double)newest->alt_m - (double)old->alt_m) / run) * 100.0);
        if (raw > GRADE_MAX_PCT) {
            raw = GRADE_MAX_PCT;
        } else if (raw < -GRADE_MAX_PCT) {
            raw = -GRADE_MAX_PCT;
        }

        if (g->have_grade) {
            g->grade_pct += GRADE_SMOOTH_ALPHA * (raw - g->grade_pct);
        } else {
            // Taken whole. Filtering up from zero would walk the figure from
            // flat to the real grade over several seconds every time the
            // reading appears, and a rider glancing down mid-climb would read
            // the filter rather than the hill.
            g->grade_pct = raw;
            g->have_grade = true;
        }
        return true;
    }

    // Not enough run in the window. Either the rider has barely started, or
    // they have been stopped long enough that every sample from before the
    // stop has aged out.
    g->have_grade = false;
    return false;
}

bool Grade_Have(const Grade_t *g) { return (g != NULL) && g->have_grade; }

float Grade_Pct(const Grade_t *g) { return (g != NULL) ? g->grade_pct : 0.0f; }
