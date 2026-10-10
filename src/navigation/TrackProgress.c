#include "TrackProgress.h"

#include <math.h>

/* All the geometry is done in single precision, in metres, relative to the
   fix: the ESP32-S3 has no double-precision FPU (see MapProject.h), and once
   the fix is subtracted every number here is at most a few tens of kilometres,
   which a float carries to well under a metre. The subtraction itself is
   integer, in TrackBuffer's own 1e-7 degree units, so nothing large ever
   reaches a float. */

#define METRES_PER_E7_LAT 0.0111320f /* 111320 m per degree, 1e7 units per degree */

typedef struct {
    int32_t lat_e7;
    int32_t lon_e7;
    float m_per_e7_lon;
} Origin_t;

static void ToLocal(const Origin_t *o, int32_t lat_e7, int32_t lon_e7, float *x, float *y) {
    *x = (float)((int64_t)lon_e7 - o->lon_e7) * o->m_per_e7_lon;
    *y = (float)((int64_t)lat_e7 - o->lat_e7) * METRES_PER_E7_LAT;
}

/* Distance from the origin to segment a-b, with how far along it the closest
   point lies (0..1) and the segment's length. */
static float SegmentDistance(float ax, float ay, float bx, float by, float *out_t,
                             float *out_len) {
    const float dx = bx - ax;
    const float dy = by - ay;
    const float len2 = dx * dx + dy * dy;
    float t = 0.0f;
    if (len2 > 0.0f) {
        t = -(ax * dx + ay * dy) / len2;
        if (t < 0.0f) {
            t = 0.0f;
        } else if (t > 1.0f) {
            t = 1.0f;
        }
    }
    {
        const float px = ax + t * dx;
        const float py = ay + t * dy;
        *out_t = t;
        *out_len = sqrtf(len2);
        return sqrtf(px * px + py * py);
    }
}

void TrackProgress_Reset(TrackProgress_t *p) {
    if (p == NULL) {
        return;
    }
    p->mark = 0;
    p->have_mark = false;
    p->pending = 0;
    p->pending_count = 0;
    p->pending_from_lat_e7 = 0;
    p->pending_from_lon_e7 = 0;
}

/* Tracking: the best on-track segment in the window ahead of the mark. */
static bool SearchWindow(const TrackProgress_t *p, const TrackBuffer_t *track, const Origin_t *o,
                         size_t *out_seg) {
    float ahead = 0.0f;
    float best_score = 0.0f;
    bool found = false;
    float ax;
    float ay;
    size_t i;

    ToLocal(o, track->lat_e7[p->mark], track->lon_e7[p->mark], &ax, &ay);
    for (i = p->mark; i + 1 < track->count && ahead <= TRACK_PROGRESS_WINDOW_M; i++) {
        float bx;
        float by;
        float t;
        float len;
        float d;
        ToLocal(o, track->lat_e7[i + 1], track->lon_e7[i + 1], &bx, &by);
        d = SegmentDistance(ax, ay, bx, by, &t, &len);
        if (d <= TRACK_PROGRESS_ONTRACK_M) {
            const float score = d + TRACK_PROGRESS_AHEAD_COST * (ahead + t * len);
            if (!found || score < best_score) {
                best_score = score;
                *out_seg = i;
                found = true;
            }
        }
        ahead += len;
        ax = bx;
        ay = by;
    }
    return found;
}

/* (Re)acquiring: the earliest on-track segment from `from` to the end, unless
   a later one is clearly closer. Most segments are dismissed by an integer box
   before any arithmetic, which is what lets this run over a 20,000-point track
   once a second while the rider is off the window. */
static bool SearchRest(const TrackBuffer_t *track, const Origin_t *o, size_t from,
                       size_t *out_seg) {
    const int32_t half_lat = (int32_t)(TRACK_PROGRESS_ONTRACK_M / METRES_PER_E7_LAT) + 1;
    const int32_t half_lon = (int32_t)(TRACK_PROGRESS_ONTRACK_M / o->m_per_e7_lon) + 1;
    const int32_t lat_lo = o->lat_e7 - half_lat;
    const int32_t lat_hi = o->lat_e7 + half_lat;
    const int32_t lon_lo = o->lon_e7 - half_lon;
    const int32_t lon_hi = o->lon_e7 + half_lon;
    float best_d = 0.0f;
    bool found = false;
    size_t i;

    for (i = from; i + 1 < track->count; i++) {
        const int32_t alat = track->lat_e7[i];
        const int32_t alon = track->lon_e7[i];
        const int32_t blat = track->lat_e7[i + 1];
        const int32_t blon = track->lon_e7[i + 1];
        float ax;
        float ay;
        float bx;
        float by;
        float t;
        float len;
        float d;

        /* Both ends past the same edge of the box: the segment cannot come
           within the on-track distance. Conservative, never wrong. */
        if ((alat < lat_lo && blat < lat_lo) || (alat > lat_hi && blat > lat_hi) ||
            (alon < lon_lo && blon < lon_lo) || (alon > lon_hi && blon > lon_hi)) {
            continue;
        }
        ToLocal(o, alat, alon, &ax, &ay);
        ToLocal(o, blat, blon, &bx, &by);
        d = SegmentDistance(ax, ay, bx, by, &t, &len);
        if (d > TRACK_PROGRESS_ONTRACK_M) {
            continue;
        }
        if (!found || d < best_d - TRACK_PROGRESS_TIE_M) {
            best_d = d;
            *out_seg = i;
            found = true;
        }
    }
    return found;
}

static float VertexDistance(const TrackBuffer_t *track, const Origin_t *o, size_t a, size_t b) {
    float ax;
    float ay;
    float bx;
    float by;
    ToLocal(o, track->lat_e7[a], track->lon_e7[a], &ax, &ay);
    ToLocal(o, track->lat_e7[b], track->lon_e7[b], &bx, &by);
    return sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
}

/* How far the rider has moved along segment `seg`'s direction since the first
   fix of the current streak. The origin IS the current fix, so the
   displacement is minus the first fix's local position. */
static float ForwardSinceStreakStart(const TrackProgress_t *p, const TrackBuffer_t *track,
                                     const Origin_t *o, size_t seg) {
    float ax;
    float ay;
    float bx;
    float by;
    float fx;
    float fy;
    float len;
    ToLocal(o, track->lat_e7[seg], track->lon_e7[seg], &ax, &ay);
    ToLocal(o, track->lat_e7[seg + 1], track->lon_e7[seg + 1], &bx, &by);
    len = sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
    if (len <= 0.0f) {
        return 0.0f; /* A repeated point has no direction to move along. */
    }
    ToLocal(o, p->pending_from_lat_e7, p->pending_from_lon_e7, &fx, &fy);
    return -(fx * (bx - ax) + fy * (by - ay)) / len;
}

bool TrackProgress_Feed(TrackProgress_t *p, const TrackBuffer_t *track, int32_t lat_e7,
                        int32_t lon_e7) {
    Origin_t o;
    size_t seg = 0;
    float cos_lat;

    if (p == NULL || track == NULL || track->count < 2) {
        return false;
    }
    if (p->have_mark && p->mark + 1 >= track->count) {
        return false; /* Already at the end; nothing left to advance into. */
    }

    cos_lat = cosf((float)lat_e7 * 1e-7f * 3.14159265f / 180.0f);
    if (cos_lat < 0.01f) {
        cos_lat = 0.01f;
    }
    o.lat_e7 = lat_e7;
    o.lon_e7 = lon_e7;
    o.m_per_e7_lon = METRES_PER_E7_LAT * cos_lat;

    if (p->have_mark && SearchWindow(p, track, &o, &seg)) {
        /* Locked on. Any evidence for a jump elsewhere is void. */
        p->pending_count = 0;
        if (seg > p->mark) {
            p->mark = seg;
            return true;
        }
        return false;
    }

    if (!SearchRest(track, &o, p->have_mark ? p->mark : 0, &seg)) {
        p->pending_count = 0;
        return false;
    }

    if (p->pending_count > 0 && seg >= p->pending &&
        VertexDistance(track, &o, p->pending, seg) <= TRACK_PROGRESS_ACQUIRE_STEP_M) {
        if (p->pending_count < 255) {
            p->pending_count++;
        }
    } else {
        p->pending_count = 1;
        p->pending_from_lat_e7 = lat_e7;
        p->pending_from_lon_e7 = lon_e7;
    }
    p->pending = seg;

    if (p->pending_count < TRACK_PROGRESS_ACQUIRE_FIXES ||
        ForwardSinceStreakStart(p, track, &o, seg) < TRACK_PROGRESS_ACQUIRE_FORWARD_M) {
        /* Not enough evidence yet -- or a rider standing still, riding the
           route backwards, or crossing it. The streak is kept, so a rider
           who sets off forwards commits as soon as they have covered the
           distance. */
        return false;
    }
    p->pending_count = 0;
    if (!p->have_mark || seg > p->mark) {
        p->mark = seg;
        p->have_mark = true;
        return true;
    }
    return false;
}
