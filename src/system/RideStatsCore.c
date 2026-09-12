#include "RideStatsCore.h"

#include <string.h>

void RideStatsCore_Reset(RideStats_t *s) {
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
}

void RideStatsCore_AddSpeed(RideStats_t *s, float kmh, double dt_seconds) {
    if (s == NULL) {
        return;
    }
    /* A non-positive interval is a repeated or reordered timestamp. Counting
       it would add distance with no time, which inflates the average. */
    if (dt_seconds <= 0.0) {
        return;
    }
    if (kmh < 0.0f || kmh > RIDE_STATS_MAX_PLAUSIBLE_KMH) {
        return;
    }

    if (kmh > s->max_kmh) {
        s->max_kmh = kmh;
    }

    /* Stopped time is excluded from both halves of the average, not just the
       numerator. See the threshold's note in the header. */
    if (kmh >= RIDE_STATS_MOVING_KMH) {
        s->moving_seconds += dt_seconds;
        s->moving_metres += ((double)kmh / 3.6) * dt_seconds;
    }
}

void RideStatsCore_AddHeartRate(RideStats_t *s, uint8_t bpm) {
    if (s == NULL) {
        return;
    }
    if (bpm < RIDE_STATS_MIN_BPM || bpm > RIDE_STATS_MAX_BPM) {
        return;
    }
    if (bpm > s->max_bpm) {
        s->max_bpm = bpm;
    }
    s->bpm_total += bpm;
    s->bpm_samples++;
}

float RideStatsCore_AvgSpeedKmh(const RideStats_t *s) {
    if (s == NULL || s->moving_seconds <= 0.0) {
        return 0.0f;
    }
    return (float)(s->moving_metres / s->moving_seconds * 3.6);
}

uint8_t RideStatsCore_AvgBpm(const RideStats_t *s) {
    if (s == NULL || s->bpm_samples == 0) {
        return 0;
    }
    /* Rounded, not truncated: a steady 150 reported as 149 looks like a bug
       to the one person who would notice. */
    return (uint8_t)((s->bpm_total + s->bpm_samples / 2) / s->bpm_samples);
}
