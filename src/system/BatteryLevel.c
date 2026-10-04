#include "BatteryLevel.h"

#include <stddef.h>

typedef struct {
    uint16_t mv;
    uint8_t percent;
} CurvePoint_t;

static const CurvePoint_t CURVE[] = {
    {3000, 0},   /* cutoff */
    {3400, 10},  /* low-battery warning */
    {3700, 50},  /* nominal */
    {4200, 100}, /* fully charged */
};
#define CURVE_LEN (sizeof(CURVE) / sizeof(CURVE[0]))

uint8_t BatteryLevel_PercentFromMillivolts(uint16_t millivolts) {
    if (millivolts == 0) {
        return 0; /* no reading yet */
    }
    if (millivolts <= CURVE[0].mv) {
        return 0;
    }
    if (millivolts >= CURVE[CURVE_LEN - 1].mv) {
        return 100;
    }

    for (size_t i = 1; i < CURVE_LEN; i++) {
        if (millivolts <= CURVE[i].mv) {
            const CurvePoint_t lo = CURVE[i - 1];
            const CurvePoint_t hi = CURVE[i];
            const uint32_t span_mv = (uint32_t)(hi.mv - lo.mv);
            const uint32_t span_pct = (uint32_t)(hi.percent - lo.percent);
            return (uint8_t)(lo.percent + (((uint32_t)(millivolts - lo.mv) * span_pct) / span_mv));
        }
    }
    return 100;
}

void BatterySmoother_Reset(BatterySmoother_t *s) {
    if (s == NULL) {
        return;
    }
    s->millivolts = 0.0f;
    s->have = false;
}

uint16_t BatterySmoother_Push(BatterySmoother_t *s, uint16_t millivolts) {
    if (s == NULL) {
        return millivolts;
    }
    /* A failed read is not a flat battery. Passing it through would drag the
       filter towards zero and show the rider a battery emptying fast. */
    if (millivolts == 0) {
        return s->have ? (uint16_t)(s->millivolts + 0.5f) : 0;
    }

    if (!s->have) {
        s->millivolts = (float)millivolts;
        s->have = true;
    } else {
        s->millivolts += BATTERY_SMOOTH_ALPHA * ((float)millivolts - s->millivolts);
    }
    return (uint16_t)(s->millivolts + 0.5f);
}
