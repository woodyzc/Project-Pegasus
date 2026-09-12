#include "TimeSource.h"

#include <string.h>

/* The best time anyone has supplied, and the tick it was supplied at. The
   clock is not stored as a running value: it is an anchor plus elapsed
   milliseconds, so nothing has to be updated on a timer and there is no
   accumulating rounding error from repeatedly adding to a counter. */
static uint32_t s_anchor_utc = 0;
static uint32_t s_anchor_ms = 0;
static uint8_t s_kind = TIME_SRC_NONE;

/* The zone travels separately because its best source is the other one. See
   the ranking note in the header. */
static int16_t s_offset_min = 0;
static bool s_offset_known = false;
static char s_zone[8] = {0};
static uint32_t s_offset_ms = 0;

static bool Fresh(uint32_t now_ms, uint32_t then_ms) {
    /* Unsigned subtraction, so a tick counter that wraps after 49 days still
       yields the right elapsed time rather than an enormous one. */
    return (uint32_t)(now_ms - then_ms) < TIME_SOURCE_STALE_MS;
}

void TimeSource_SetFromPhone(uint32_t utc_seconds, int16_t offset_min, const char *zone,
                             uint32_t now_ms) {
    /* The offset and zone are taken unconditionally. A satellite knows the
       instant and has no idea what the rider would call it. */
    s_offset_min = offset_min;
    s_offset_known = true;
    s_offset_ms = now_ms;
    if (zone != NULL) {
        strncpy(s_zone, zone, sizeof(s_zone) - 1);
        s_zone[sizeof(s_zone) - 1] = '\0';
    } else {
        s_zone[0] = '\0';
    }

    /* The time itself only if nothing better is currently standing. */
    if (s_kind == TIME_SRC_GNSS && Fresh(now_ms, s_anchor_ms)) {
        return;
    }
    s_anchor_utc = utc_seconds;
    s_anchor_ms = now_ms;
    s_kind = TIME_SRC_PHONE;
}

void TimeSource_SetFromGnss(uint32_t utc_seconds, uint32_t now_ms) {
    s_anchor_utc = utc_seconds;
    s_anchor_ms = now_ms;
    s_kind = TIME_SRC_GNSS;
}

bool TimeSource_Now(uint32_t now_ms, TimeReading_t *out) {
    uint32_t elapsed_ms;

    if (out == NULL || s_kind == TIME_SRC_NONE) {
        return false;
    }
    if (!Fresh(now_ms, s_anchor_ms)) {
        /* Nothing has confirmed the time for hours. Saying so beats showing
           an hour that has quietly drifted. */
        return false;
    }

    elapsed_ms = (uint32_t)(now_ms - s_anchor_ms);
    out->utc_seconds = s_anchor_utc + elapsed_ms / 1000u;
    out->kind = s_kind;

    /* The offset can go stale on its own: a rider whose phone has been away
       for hours may well have crossed a zone, and a remembered offset would
       silently misplace every hour shown from then on. */
    if (s_offset_known && Fresh(now_ms, s_offset_ms)) {
        out->offset_min = s_offset_min;
        out->offset_known = true;
        strncpy(out->zone, s_zone, sizeof(out->zone) - 1);
        out->zone[sizeof(out->zone) - 1] = '\0';
    } else {
        out->offset_min = 0;
        out->offset_known = false;
        out->zone[0] = '\0';
    }
    return true;
}

void TimeSource_Reset(void) {
    s_anchor_utc = 0;
    s_anchor_ms = 0;
    s_kind = TIME_SRC_NONE;
    s_offset_min = 0;
    s_offset_known = false;
    s_offset_ms = 0;
    s_zone[0] = '\0';
}
