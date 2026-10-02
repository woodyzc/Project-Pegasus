// Host tests for src/system/LongPress.c -- the power key's long press.
//
// Tested out of proportion to its size because of what it does when it is
// wrong. This fires the power-off path: a spurious trigger switches the board
// off mid-ride, and one that repeats while the key is held switches it off
// again the moment the rider turns it back on.

#include <stdio.h>

#include "../../src/system/LongPress.h"

static int checks;
static int failures;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("\n    FAIL: %s", what);
    }
}

// Holds the key for `ms` at 50ms sampling, counting how many times it fired.
static int hold_for(LongPress_t *lp, uint32_t *t, uint32_t ms) {
    int fires = 0;
    for (uint32_t i = 0; i < ms; i += 50) {
        if (LongPress_Feed(lp, true, *t)) {
            fires++;
        }
        *t += 50;
    }
    return fires;
}

static void release(LongPress_t *lp, uint32_t *t) {
    LongPress_Feed(lp, false, *t);
    *t += 50;
}

static void test_fires_once(void) {
    printf("- a long hold fires exactly once: ");
    LongPress_t lp;
    LongPress_Reset(&lp, false);
    uint32_t t = 10000;

    // Held for four times the threshold. Still one event.
    const int fires = hold_for(&lp, &t, LONG_PRESS_MS * 4);
    check(fires == 1, "exactly one event for one press");
    printf("done\n");
}

static void test_short_press_does_nothing(void) {
    printf("- a short press does nothing at all: ");
    LongPress_t lp;
    LongPress_Reset(&lp, false);
    uint32_t t = 10000;

    check(hold_for(&lp, &t, LONG_PRESS_MS - 200) == 0, "released before the threshold");
    release(&lp, &t);
    check(hold_for(&lp, &t, 300) == 0, "and a second short one");
    printf("done\n");
}

static void test_a_second_press_can_fire(void) {
    printf("- but a genuine second press still works: ");
    LongPress_t lp;
    LongPress_Reset(&lp, false);
    uint32_t t = 10000;

    check(hold_for(&lp, &t, LONG_PRESS_MS + 200) == 1, "first press fires");
    release(&lp, &t);
    check(hold_for(&lp, &t, LONG_PRESS_MS + 200) == 1, "second press fires too");
    printf("done\n");
}

static void test_held_at_startup_is_ignored(void) {
    printf("- the press that switched the board ON cannot switch it off: ");
    // This is the real scenario: the rider holds the power key to bring the
    // rail up, firmware boots and starts watching the pin while their thumb is
    // still on it. Without the disarm, the board powers off a second later.
    LongPress_t lp;
    LongPress_Reset(&lp, true);
    uint32_t t = 10000;

    check(hold_for(&lp, &t, LONG_PRESS_MS * 3) == 0, "held from boot fires nothing");

    // Once they let go, the key behaves normally.
    release(&lp, &t);
    check(hold_for(&lp, &t, LONG_PRESS_MS + 200) == 1, "and works after the release");
    printf("done\n");
}

static void test_bounce(void) {
    printf("- a key that bounces does not accumulate hold time: ");
    LongPress_t lp;
    LongPress_Reset(&lp, false);
    uint32_t t = 10000;

    // Three nearly-long presses in a row must not add up to one long one.
    for (int i = 0; i < 3; i++) {
        check(hold_for(&lp, &t, LONG_PRESS_MS - 100) == 0, "a near miss fires nothing");
        release(&lp, &t);
    }
    printf("done\n");
}

static void test_tick_wrap(void) {
    printf("- the millisecond tick wrapping mid-press: ");
    LongPress_t lp;
    LongPress_Reset(&lp, false);
    // Straddles 2^32. Signed arithmetic here would read an enormous hold and
    // fire on the sample right after the wrap.
    uint32_t t = 0xFFFFFF00u;
    check(LongPress_Feed(&lp, true, t) == false, "the first down sample never fires");
    t += 50;
    check(LongPress_Feed(&lp, true, t) == false, "nor the one after the wrap");
    check(hold_for(&lp, &t, LONG_PRESS_MS + 200) == 1, "fires once the hold really elapses");
    printf("done\n");
}

static void test_null_arguments(void) {
    printf("- null arguments: ");
    LongPress_Reset(NULL, true);
    check(!LongPress_Feed(NULL, true, 0), "feeding null is false, not a crash");
    printf("done\n");
}

int main(void) {
    printf("== test_long_press ==\n");
    test_fires_once();
    test_short_press_does_nothing();
    test_a_second_press_can_fire();
    test_held_at_startup_is_ignored();
    test_bounce();
    test_tick_wrap();
    test_null_arguments();

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
