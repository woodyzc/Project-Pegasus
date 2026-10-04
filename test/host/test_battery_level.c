// Host tests for src/system/BatteryLevel.c -- pack voltage to a percentage,
// and the filter in front of it.
//
// The curve spends 40 percentage points between 3400mV and 3700mV, so one
// percent is 7.5 millivolts in exactly the range a rider cares about. That
// steepness is why a reading that looks fine on a bench jitters on a panel,
// and it is what these tests pin.

#include <stdio.h>

#include "../../src/system/BatteryLevel.h"

static int checks;
static int failures;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("\n    FAIL: %s", what);
    }
}

static void test_curve_anchors(void) {
    printf("- the curve's own points read back exactly: ");
    check(BatteryLevel_PercentFromMillivolts(3000) == 0, "3000mV is empty");
    check(BatteryLevel_PercentFromMillivolts(3400) == 10, "3400mV is 10%");
    check(BatteryLevel_PercentFromMillivolts(3700) == 50, "3700mV is nominal");
    check(BatteryLevel_PercentFromMillivolts(4200) == 100, "4200mV is full");
    printf("done\n");
}

static void test_curve_is_not_a_straight_line(void) {
    printf("- it is piecewise, not a single ramp: ");
    // A straight 3000-4200 map would put 3700mV at 58%. The whole point of
    // the curve is that it does not, because a LiPo holds most of its charge
    // above nominal and a linear map flatters a tired battery.
    check(BatteryLevel_PercentFromMillivolts(3700) == 50, "3700mV is 50, not 58");

    // Monotonic throughout, which a mis-ordered table would break silently.
    uint8_t last = 0;
    for (uint16_t mv = 3000; mv <= 4200; mv += 10) {
        const uint8_t p = BatteryLevel_PercentFromMillivolts(mv);
        check(p >= last, "never goes backwards as voltage rises");
        last = p;
    }
    printf("done\n");
}

static void test_out_of_range(void) {
    printf("- beyond the ends, and no reading at all: ");
    check(BatteryLevel_PercentFromMillivolts(2500) == 0, "below cutoff is empty");
    check(BatteryLevel_PercentFromMillivolts(5000) == 100, "above full is full");
    check(BatteryLevel_PercentFromMillivolts(0) == 0, "no reading is 0, not a crash");
    printf("done\n");
}

static void test_how_steep_it_is(void) {
    printf("- one percent is a handful of millivolts: ");
    // This is the whole reason the smoother exists. 7.5mV per percent on the
    // pack is 2.5mV on the ADC pin through the 3:1 divider -- less than the
    // noise on an ESP32-S3 ADC1 channel.
    const uint8_t a = BatteryLevel_PercentFromMillivolts(3500);
    const uint8_t b = BatteryLevel_PercentFromMillivolts(3530);
    check((b - a) >= 3, "30mV moves the reading several percent");
    printf("done\n");
}

static void test_smoother_takes_the_first_whole(void) {
    printf("- the filter starts at the first reading, not at empty: ");
    BatterySmoother_t s;
    BatterySmoother_Reset(&s);
    // Ramping up from zero would walk the panel from 0% to the real level
    // over the first minute of every boot.
    check(BatterySmoother_Push(&s, 3800) == 3800, "first reading is taken whole");
    printf("done\n");
}

static void test_smoother_rejects_jitter(void) {
    printf("- noise is damped rather than displayed: ");
    BatterySmoother_t s;
    BatterySmoother_Reset(&s);
    BatterySmoother_Push(&s, 3600);

    // +/-40mV of ADC noise around a steady pack. Without the filter that is
    // about five percentage points of flicker on the panel.
    uint16_t out = 3600;
    for (int i = 0; i < 40; i++) {
        out = BatterySmoother_Push(&s, (i % 2 == 0) ? 3640 : 3560);
    }
    const uint8_t p = BatteryLevel_PercentFromMillivolts(out);
    const uint8_t want = BatteryLevel_PercentFromMillivolts(3600);
    check((p > want ? p - want : want - p) <= 1, "settles within a percent of the truth");
    printf("done\n");
}

static void test_smoother_follows_a_real_change(void) {
    printf("- but a real change still arrives: ");
    BatterySmoother_t s;
    BatterySmoother_Reset(&s);
    BatterySmoother_Push(&s, 3500);
    uint16_t out = 3500;
    for (int i = 0; i < 80; i++) {
        out = BatterySmoother_Push(&s, 4000);
    }
    check(out > 3980, "a sustained rise is followed");
    printf("done\n");
}

static void test_failed_read_is_not_a_flat_battery(void) {
    printf("- a failed read does not empty the gauge: ");
    BatterySmoother_t s;
    BatterySmoother_Reset(&s);
    BatterySmoother_Push(&s, 3900);
    // Battery_ReadMillivolts returns 0 when the ADC is not ready. Folding
    // that in would drag the filter towards empty and show a battery
    // draining at an impossible rate.
    for (int i = 0; i < 10; i++) {
        BatterySmoother_Push(&s, 0);
    }
    check(BatterySmoother_Push(&s, 0) == 3900, "zeros are ignored, not averaged");

    BatterySmoother_Reset(&s);
    check(BatterySmoother_Push(&s, 0) == 0, "but a zero before any reading is still 0");
    printf("done\n");
}

static void test_null_arguments(void) {
    printf("- null arguments: ");
    BatterySmoother_Reset(NULL);
    check(BatterySmoother_Push(NULL, 3700) == 3700, "null passes the reading through");
    printf("done\n");
}

int main(void) {
    printf("== test_battery_level ==\n");
    test_curve_anchors();
    test_curve_is_not_a_straight_line();
    test_out_of_range();
    test_how_steep_it_is();
    test_smoother_takes_the_first_whole();
    test_smoother_rejects_jitter();
    test_smoother_follows_a_real_change();
    test_failed_read_is_not_a_flat_battery();
    test_null_arguments();

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
