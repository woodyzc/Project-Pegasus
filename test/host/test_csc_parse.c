/* Host tests for src/sensors/BleCscParse.c -- the BLE Cycling Speed and
 * Cadence Measurement (0x2A5B) decoder and the rpm tracker built on it.
 *
 * Compiles the real shipped source, not a copy.
 *
 * The tracker is the half worth this much test: a wrong decode produces an
 * obviously absurd number, while a wrong interval produces a plausible one.
 * Every counter on this wire is 16 bits and free-running, so a ride long
 * enough wraps both of them, and the wrap arrives without warning on a road
 * rather than on a bench. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "BleCscParse.h"

static int failures = 0;
static int checks = 0;

static void check(bool cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); failures++; }
}

static void check_eq(unsigned got, unsigned want, const char *what) {
    checks++;
    if (got != want) {
        printf("  FAIL: %s (got=%u want=%u)\n", what, got, want);
        failures++;
    }
}

/* One crank-only notification, the shape a cadence sensor actually sends. */
static size_t crank_packet(uint8_t *out, uint16_t revs, uint16_t ticks) {
    out[0] = CSC_FLAG_CRANK_PRESENT;
    out[1] = (uint8_t)(revs & 0xFF);
    out[2] = (uint8_t)(revs >> 8);
    out[3] = (uint8_t)(ticks & 0xFF);
    out[4] = (uint8_t)(ticks >> 8);
    return 5;
}

/* A combined speed-and-cadence sensor puts six bytes of wheel data in front. */
static size_t wheel_and_crank_packet(uint8_t *out, uint32_t wheel_revs, uint16_t wheel_ticks,
                                     uint16_t revs, uint16_t ticks) {
    out[0] = CSC_FLAG_WHEEL_PRESENT | CSC_FLAG_CRANK_PRESENT;
    out[1] = (uint8_t)(wheel_revs & 0xFF);
    out[2] = (uint8_t)((wheel_revs >> 8) & 0xFF);
    out[3] = (uint8_t)((wheel_revs >> 16) & 0xFF);
    out[4] = (uint8_t)((wheel_revs >> 24) & 0xFF);
    out[5] = (uint8_t)(wheel_ticks & 0xFF);
    out[6] = (uint8_t)(wheel_ticks >> 8);
    out[7] = (uint8_t)(revs & 0xFF);
    out[8] = (uint8_t)(revs >> 8);
    out[9] = (uint8_t)(ticks & 0xFF);
    out[10] = (uint8_t)(ticks >> 8);
    return 11;
}

/* Feeds one packet and returns the rpm, or SENTINEL when nothing was
 * published. Keeps the cases below down to what they are actually about. */
#define NO_UPDATE 0xFFFFu
static unsigned feed(CadenceTracker_t *t, uint16_t revs, uint16_t ticks, uint32_t now_ms) {
    uint8_t pkt[16];
    size_t len = crank_packet(pkt, revs, ticks);
    CscMeasurement_t m;
    if (!Csc_ParseMeasurement(pkt, len, &m)) return NO_UPDATE;
    uint16_t rpm = 0;
    if (!CadenceTracker_Update(t, &m, now_ms, &rpm)) return NO_UPDATE;
    return rpm;
}

static void test_decoding(void) {
    printf("-- the wire format --\n");

    uint8_t pkt[16];
    CscMeasurement_t m;

    memset(&m, 0, sizeof(m));
    check(Csc_ParseMeasurement(pkt, crank_packet(pkt, 1234, 5678), &m), "crank-only accepted");
    check_eq(m.crank_revs, 1234, "crank revolutions little-endian");
    check_eq(m.crank_event_time, 5678, "crank event time little-endian");

    /* The wheel fields must be skipped by exactly six bytes. Reading them as
     * the crank pair is the failure this shape exists to catch, and it would
     * produce a number rather than an error. */
    memset(&m, 0, sizeof(m));
    check(Csc_ParseMeasurement(pkt, wheel_and_crank_packet(pkt, 0xDEADBEEF, 0x1111, 77, 88), &m),
          "wheel+crank accepted");
    check_eq(m.crank_revs, 77, "crank revolutions found past the wheel fields");
    check_eq(m.crank_event_time, 88, "crank event time found past the wheel fields");

    /* Not an error on the wire -- a combined sensor may send wheel-only
     * notifications -- but it carries no cadence, and saying so is the only
     * honest answer. */
    pkt[0] = CSC_FLAG_WHEEL_PRESENT;
    memset(pkt + 1, 0x42, 6);
    check(!Csc_ParseMeasurement(pkt, 7, &m), "wheel-only packet carries no cadence");

    pkt[0] = 0x00;
    check(!Csc_ParseMeasurement(pkt, 1, &m), "flags claiming nothing is rejected");

    /* Truncation, at each field boundary. */
    crank_packet(pkt, 1, 1);
    for (size_t len = 0; len < 5; len++) {
        check(!Csc_ParseMeasurement(pkt, len, &m), "short crank packet rejected");
    }
    wheel_and_crank_packet(pkt, 1, 1, 1, 1);
    for (size_t len = 0; len < 11; len++) {
        check(!Csc_ParseMeasurement(pkt, len, &m), "short wheel+crank packet rejected");
    }

    check(!Csc_ParseMeasurement(NULL, 5, &m), "null buffer rejected");
    check(!Csc_ParseMeasurement(pkt, 5, NULL), "null output rejected");
}

static void test_first_sample_is_a_baseline(void) {
    printf("-- one sample is not a cadence --\n");

    CadenceTracker_t t;
    CadenceTracker_Reset(&t);
    /* A sensor that has been on the bike for weeks starts mid-count. Nothing
     * about the absolute values means anything. */
    check_eq(feed(&t, 40000, 30000, 1000), NO_UPDATE, "first sample publishes nothing");
}

static void test_steady_cadence(void) {
    printf("-- a steady 90rpm --\n");

    /* 90rpm is one revolution every 2/3 s, which is 682.67 ticks. Using 683
     * per revolution the arithmetic should land back on 90. */
    CadenceTracker_t t;
    CadenceTracker_Reset(&t);
    feed(&t, 100, 10000, 1000);
    check_eq(feed(&t, 101, (uint16_t)(10000 + 683), 1683), 90, "one revolution in 683 ticks");

    /* Several revolutions in one notification, which is what happens at a
     * one-second notify rate and a high cadence. 3 revolutions in exactly one
     * second is 180rpm, which is also past anything a rider sustains and
     * comfortably inside the plausibility bound. */
    CadenceTracker_Reset(&t);
    feed(&t, 0, 0, 0);
    check_eq(feed(&t, 3, 1024, 1000), 180, "three revolutions in one second");
}

static void test_the_event_time_wrap(void) {
    printf("-- the 64-second wrap --\n");

    /* Event time wraps every 64 seconds, so this happens on every ride rather
     * than at some edge. 16-bit unsigned subtraction has to carry it. */
    CadenceTracker_t t;
    CadenceTracker_Reset(&t);
    feed(&t, 500, 65400, 1000);
    check_eq(feed(&t, 501, (uint16_t)(65400 + 683), 1683), 90, "interval across the time wrap");

    /* And the revolution counter wraps too, roughly every twelve hours of
     * pedalling. */
    CadenceTracker_Reset(&t);
    feed(&t, 65535, 1000, 1000);
    check_eq(feed(&t, 0, (uint16_t)(1000 + 683), 1683), 90, "one revolution across the rev wrap");
}

static void test_repeats_are_not_news(void) {
    printf("-- a repeated crank event --\n");

    /* Sensors notify on a timer, not on a pedal stroke. At 60rpm with a 1Hz
     * notify rate roughly every other packet repeats the previous event, and
     * treating that as a zero-length interval is a division by zero. */
    CadenceTracker_t t;
    CadenceTracker_Reset(&t);
    feed(&t, 10, 1000, 1000);
    feed(&t, 11, 1683, 1683);
    check_eq(feed(&t, 11, 1683, 2000), NO_UPDATE, "identical sample publishes nothing");
    check_eq(feed(&t, 11, 1683, 2500), NO_UPDATE, "and again");
    /* The rider is still pedalling; the next real event proves it. */
    check_eq(feed(&t, 12, (uint16_t)(1683 + 683), 3000), NO_UPDATE,
             "steady rpm is not republished");
}

static void test_stopping(void) {
    printf("-- the rider stops pedalling --\n");

    CadenceTracker_t t;
    CadenceTracker_Reset(&t);
    feed(&t, 0, 0, 0);
    check_eq(feed(&t, 1, 683, 683), 90, "pedalling");

    /* The sensor keeps notifying with the counters frozen. Before the timeout
     * the last rpm stands -- a rider between pedal strokes has not stopped. */
    check_eq(feed(&t, 1, 683, 1500), NO_UPDATE, "held briefly after the last stroke");
    check_eq(feed(&t, 1, 683, 3000), NO_UPDATE, "still held just inside the timeout");
    check_eq(feed(&t, 1, 683, 3684), 0, "zero once the crank has been still for 3s");

    /* Once at zero it stays there without republishing. */
    check_eq(feed(&t, 1, 683, 5000), NO_UPDATE, "zero is not republished");

    /* And pedalling again is picked up from the next interval. */
    check_eq(feed(&t, 2, (uint16_t)(683 + 683), 5683), 90, "back to 90 when the crank turns");
}

static void test_stopping_with_no_packets_at_all(void) {
    printf("-- the sensor goes quiet while stopped --\n");

    /* Some sensors stop notifying entirely when the crank stops, so nothing
     * arrives to drive the timeout. The tick is what gets the panel to zero. */
    CadenceTracker_t t;
    CadenceTracker_Reset(&t);
    feed(&t, 0, 0, 0);
    check_eq(feed(&t, 1, 683, 683), 90, "pedalling");

    uint16_t rpm = 0xFFFF;
    check(!CadenceTracker_Tick(&t, 2000, &rpm), "tick inside the timeout changes nothing");
    check(CadenceTracker_Tick(&t, 4000, &rpm), "tick past the timeout reports a change");
    check_eq(rpm, 0, "and the change is to zero");
    check(!CadenceTracker_Tick(&t, 9000, &rpm), "and it is not reported twice");
}

static void test_implausible_intervals(void) {
    printf("-- numbers not to believe --\n");

    /* A missed notification across the time wrap makes a long interval decode
     * as a short one. The result is a huge rpm, and clamping it to the
     * maximum would produce a number a rider might act on. */
    CadenceTracker_t t;
    CadenceTracker_Reset(&t);
    feed(&t, 0, 0, 0);
    check_eq(feed(&t, 10, 100, 100), NO_UPDATE, "6144rpm is dropped, not clamped");
    /* Nothing was published, so nothing is on screen claiming to be cadence. */
    check_eq(feed(&t, 11, 783, 783), 90, "and the next good interval still reads");

    /* The boundary itself is believed. */
    CadenceTracker_Reset(&t);
    feed(&t, 0, 0, 0);
    /* 250rpm = one revolution every 245.76 ticks. */
    check_eq(feed(&t, 1, 246, 240), 250, "250rpm is accepted");
    CadenceTracker_Reset(&t);
    feed(&t, 0, 0, 0);
    check_eq(feed(&t, 1, 244, 240), NO_UPDATE, "just past 250rpm is not");
}

static void test_reconnect_forgets_everything(void) {
    printf("-- a reconnected sensor shares no history --\n");

    /* The counters of a sensor that dropped and came back have no relationship
     * to the ones before it: it may have been power-cycled, or simply kept
     * counting while out of range. Either way the first interval after a
     * reconnect is meaningless and must not be published. */
    CadenceTracker_t t;
    CadenceTracker_Reset(&t);
    feed(&t, 0, 0, 0);
    check_eq(feed(&t, 1, 683, 683), 90, "pedalling before the drop");

    CadenceTracker_Reset(&t);
    check_eq(feed(&t, 9000, 40000, 60000), NO_UPDATE, "first sample after reconnect is a baseline");
    check_eq(t.rpm, 0, "and the reset cleared the last rpm");
}

static void test_null_safety(void) {
    printf("-- null arguments --\n");

    CscMeasurement_t m = {0};
    uint16_t rpm = 0;
    CadenceTracker_t t;
    CadenceTracker_Reset(&t);
    CadenceTracker_Reset(NULL); /* must not crash */
    check(!CadenceTracker_Update(NULL, &m, 0, &rpm), "null tracker rejected");
    check(!CadenceTracker_Update(&t, NULL, 0, &rpm), "null measurement rejected");
    check(!CadenceTracker_Update(&t, &m, 0, NULL), "null output rejected");
    check(!CadenceTracker_Tick(NULL, 0, &rpm), "null tracker tick rejected");
    check(!CadenceTracker_Tick(&t, 0, NULL), "null output tick rejected");
}

int main(void) {
    test_decoding();
    test_first_sample_is_a_baseline();
    test_steady_cadence();
    test_the_event_time_wrap();
    test_repeats_are_not_news();
    test_stopping();
    test_stopping_with_no_packets_at_all();
    test_implausible_intervals();
    test_reconnect_forgets_everything();
    test_null_safety();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
