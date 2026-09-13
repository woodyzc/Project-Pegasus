#include "Ascent.h"

#include <stddef.h>

void Ascent_Reset(Ascent_t *a) {
    if (a == NULL) {
        return;
    }
    a->has_smooth = false;
    a->smooth_m = 0.0f;
    a->has_raw = false;
    a->last_raw_m = 0.0f;
    a->last_ms = 0;
    a->has_ref = false;
    a->ref_m = 0.0f;
    a->ascent_m = 0.0;
    a->descent_m = 0.0;
}

bool Ascent_Feed(Ascent_t *a, float alt_m, uint32_t time_ms) {
    if (a == NULL) {
        return false;
    }

    // Filter 1: not a place a bicycle is. Also catches the NaN a receiver can
    // produce before it has a vertical solution, since every comparison with
    // NaN is false and this one is written to reject on false.
    if (!(alt_m >= ASCENT_MIN_ALT_M && alt_m <= ASCENT_MAX_ALT_M)) {
        return false;
    }

    // Filter 2: a rate no rider achieves. Skipped on the first sample, when
    // there is nothing to compare against, and across a long gap, where a
    // large change is a lost fix rather than a spike.
    if (a->has_raw) {
        const uint32_t gap_ms = time_ms - a->last_ms;
        if (gap_ms > 0 && gap_ms <= ASCENT_MAX_GAP_MS) {
            float change = alt_m - a->last_raw_m;
            if (change < 0.0f) {
                change = -change;
            }
            const float rate = change / ((float)gap_ms / 1000.0f);
            if (rate > ASCENT_MAX_RATE_MPS) {
                // Deliberately does not update last_raw_m. A spike must not
                // become the baseline the next sample is judged against, or a
                // single bad fix would reject the good one that follows it.
                return false;
            }
        }
    }
    a->has_raw = true;
    a->last_raw_m = alt_m;
    a->last_ms = time_ms;

    // Filter 3a: smooth what survived.
    if (!a->has_smooth) {
        a->smooth_m = alt_m;
        a->has_smooth = true;
    } else {
        a->smooth_m += ASCENT_SMOOTH_ALPHA * (alt_m - a->smooth_m);
    }

    // Filter 3b: count only what leaves the band. The reference stays put
    // while the smoothed altitude wanders inside it, which is what makes an
    // hour of flat riding add nothing rather than adding everything.
    if (!a->has_ref) {
        a->ref_m = a->smooth_m;
        a->has_ref = true;
        return true;
    }

    const float above = a->smooth_m - a->ref_m;
    if (above > ASCENT_BAND_M) {
        a->ascent_m += (double)above;
        a->ref_m = a->smooth_m;
    } else if (above < -ASCENT_BAND_M) {
        a->descent_m += (double)(-above);
        a->ref_m = a->smooth_m;
    }
    return true;
}

double Ascent_Metres(const Ascent_t *a) {
    return (a != NULL) ? a->ascent_m : 0.0;
}

double Ascent_DescentMetres(const Ascent_t *a) {
    return (a != NULL) ? a->descent_m : 0.0;
}

bool Ascent_HasAltitude(const Ascent_t *a) {
    return (a != NULL) && a->has_smooth;
}

double Ascent_AltitudeMetres(const Ascent_t *a) {
    return (a != NULL && a->has_smooth) ? (double)a->smooth_m : 0.0;
}
