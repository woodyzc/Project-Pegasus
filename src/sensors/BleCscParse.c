#include "BleCscParse.h"

#include <string.h>

// Event times are 1/1024 s. Kept as a name because the arithmetic below reads
// as nonsense without it.
#define CSC_TICKS_PER_SECOND 1024u

bool Csc_ParseMeasurement(const uint8_t *data, size_t length, CscMeasurement_t *out) {
    if (data == NULL || out == NULL || length < 1) {
        return false;
    }

    const uint8_t flags = data[0];
    size_t offset = 1;

    // Wheel data comes first when present, and is skipped rather than read.
    // The length check is the point: without it a sensor sending wheel-only
    // packets would have its speed fields read as a cadence.
    if (flags & CSC_FLAG_WHEEL_PRESENT) {
        if (length < offset + 6) {
            return false;
        }
        offset += 6; // uint32 revolutions + uint16 event time
    }

    if (!(flags & CSC_FLAG_CRANK_PRESENT)) {
        return false;
    }
    if (length < offset + 4) {
        return false;
    }

    // Little-endian, as everything on this wire is.
    out->crank_revs = (uint16_t)((uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8));
    out->crank_event_time =
        (uint16_t)((uint16_t)data[offset + 2] | ((uint16_t)data[offset + 3] << 8));
    return true;
}

void CadenceTracker_Reset(CadenceTracker_t *t) {
    if (t == NULL) {
        return;
    }
    memset(t, 0, sizeof(*t));
}

bool CadenceTracker_Update(CadenceTracker_t *t, const CscMeasurement_t *m, uint32_t now_ms,
                           uint16_t *out_rpm) {
    if (t == NULL || m == NULL || out_rpm == NULL) {
        return false;
    }

    // A baseline older than the event time's own 64-second domain cannot be
    // subtracted from -- the wrap count is unknown, so the difference is a
    // guess that looks like a measurement. Drop it and re-seed below. See
    // CADENCE_BASELINE_MAX_GAP_MS for why the rejected rpm is not enough on
    // its own: the fabricated value is usually plausible.
    if (t->have_last &&
        (uint32_t)(now_ms - t->last_sample_ms) >= CADENCE_BASELINE_MAX_GAP_MS) {
        t->have_last = false;
    }

    if (!t->have_last) {
        t->last_revs = m->crank_revs;
        t->last_event_time = m->crank_event_time;
        t->last_sample_ms = now_ms;
        t->have_last = true;
        // Not motion: one sample says the crank has a position, not that it
        // moved. Starting the idle clock here would report a stopped rider as
        // zero three seconds after connecting, which is correct, and a
        // pedalling one as zero too until their second revolution, which is
        // not -- so the clock starts on the first real advance instead.
        return false;
    }

    // Both counters wrap at 65536. Unsigned subtraction in 16 bits gives the
    // right answer across the wrap for free, which is the whole reason the
    // spec sizes them this way -- as long as nothing here promotes to int
    // first, hence the casts.
    const uint16_t d_revs = (uint16_t)(m->crank_revs - t->last_revs);
    const uint16_t d_ticks = (uint16_t)(m->crank_event_time - t->last_event_time);

    // The same crank event again. Sensors notify on a timer, not on a pedal
    // stroke, so this arrives constantly at low cadence and while stopped. It
    // is not new information and must not be treated as a zero-length
    // interval -- that is a division by zero, and on the revs side it is the
    // difference between "not moving" and "no news".
    if (d_ticks == 0) {
        // The counters standing still IS the evidence the rider has stopped,
        // and the idle timeout below is what acts on it.
        return CadenceTracker_Tick(t, now_ms, out_rpm);
    }

    t->last_revs = m->crank_revs;
    t->last_event_time = m->crank_event_time;
    t->last_sample_ms = now_ms;

    if (d_revs == 0) {
        // A new crank event with no new revolution should not happen, but a
        // sensor that does it is telling us time passed and the crank did
        // not turn.
        return CadenceTracker_Tick(t, now_ms, out_rpm);
    }

    t->last_motion_ms = now_ms;
    t->have_motion = true;

    // revolutions per minute = revs / (ticks / 1024) * 60, rearranged to stay
    // in integers and to round rather than truncate. d_revs is at most a few
    // and d_ticks at most 65535, so the numerator cannot overflow 32 bits.
    const uint32_t numerator = (uint32_t)d_revs * CSC_TICKS_PER_SECOND * 60u;
    const uint32_t rpm = (numerator + (uint32_t)d_ticks / 2u) / (uint32_t)d_ticks;

    if (rpm > CADENCE_MAX_RPM) {
        // Implausible. The likeliest cause is a missed notification across the
        // 64-second event-time wrap, where a long interval decodes as a short
        // one. Dropping the sample rather than clamping it: a clamped 250 is a
        // number a rider might believe.
        return false;
    }

    const uint16_t value = (uint16_t)rpm;
    if (value == t->rpm) {
        return false;
    }
    t->rpm = value;
    *out_rpm = value;
    return true;
}

bool CadenceTracker_Tick(CadenceTracker_t *t, uint32_t now_ms, uint16_t *out_rpm) {
    if (t == NULL || out_rpm == NULL) {
        return false;
    }
    if (t->rpm == 0) {
        return false;
    }
    // Never seen the crank move, yet holding a non-zero rpm, is not a state
    // this can reach -- but if it ever did, holding that number for ever would
    // be the worst of the options.
    if (t->have_motion && (uint32_t)(now_ms - t->last_motion_ms) < CADENCE_IDLE_MS) {
        return false;
    }
    t->rpm = 0;
    *out_rpm = 0;
    return true;
}
