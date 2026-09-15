// Host tests for src/system/RideSummary.c -- what a ride amounted to.
//
// The duration is the part worth testing off-target: it has three shapes, and
// two of them only appear after an hour or a day of riding, which is not
// something a bench reproduces.

#include <stdio.h>
#include <string.h>

#include "../../src/system/RideSummary.h"

static int checks;
static int failures;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("\n    FAIL: %s", what);
    }
}

static void check_time(uint32_t seconds, const char *want) {
    char buf[RIDE_SUMMARY_TIME_MAX];
    const bool ok = RideSummary_FormatDuration(seconds, buf, sizeof(buf));
    checks++;
    if (!ok || strcmp(buf, want) != 0) {
        failures++;
        printf("\n    FAIL: %us -> \"%s\", want \"%s\"", (unsigned)seconds, ok ? buf : "(failed)",
               want);
    }
}

int main(void) {
    printf("- under an hour reads as minutes and seconds: ");
    check_time(0, "0:00");
    check_time(5, "0:05");
    check_time(65, "1:05");
    check_time(425, "7:05");
    check_time(3599, "59:59");
    printf("done\n");

    printf("- an hour and over grows a field, and only then: ");
    check_time(3600, "1:00:00");
    check_time(3661, "1:01:01");
    check_time(45296, "12:34:56");
    printf("done\n");

    printf("- minutes are unpadded, seconds are padded: ");
    // "07:05" for a seven-minute ride is noise on a number read at a glance,
    // and the colon count already says which unit is which.
    check_time(425, "7:05");
    check_time(3665, "1:01:05");
    printf("done\n");

    printf("- a stuck timer is clamped rather than overflowing: ");
    check_time(0xFFFFFFFFu, "99:59:59");
    {
        char buf[RIDE_SUMMARY_TIME_MAX];
        check(RideSummary_FormatDuration(0xFFFFFFFFu, buf, sizeof(buf)),
              "the longest output still fits RIDE_SUMMARY_TIME_MAX");
        check(strlen(buf) < RIDE_SUMMARY_TIME_MAX, "with room for its terminator");
    }
    printf("done\n");

    printf("- a buffer that cannot hold the answer is refused: ");
    {
        char small[4];
        check(!RideSummary_FormatDuration(60, small, sizeof(small)), "reports failure");
        check(small[0] == '\0', "and leaves nothing behind rather than a half time");
        check(!RideSummary_FormatDuration(60, NULL, 10), "a null buffer too");
        check(!RideSummary_FormatDuration(60, small, 0), "and a zero size");
    }
    printf("done\n");

    printf("- the short form is five glyphs, whatever the duration: ");
    {
        char buf[RIDE_SUMMARY_SHORT_TIME_MAX];
        struct { uint32_t s; const char *want; } cases[] = {
            {0, "0:00"},
            {5, "0:05"},
            {425, "7:05"},
            {3599, "59:59"},   // the widest sub-hour form
            {3600, "1:00"},    // and the seconds go, because nothing reads them
            {3661, "1:01"},
            {45296, "12:34"},  // the widest form there is
            {0xFFFFFFFFu, "99:59"},
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            checks++;
            const bool ok = RideSummary_FormatDurationShort(cases[i].s, buf, sizeof(buf));
            if (!ok || strcmp(buf, cases[i].want) != 0) {
                failures++;
                printf("\n    FAIL: %us -> \"%s\", want \"%s\"", (unsigned)cases[i].s,
                       ok ? buf : "(failed)", cases[i].want);
            } else {
                // The cell is laid out for five glyphs. Anything longer would
                // be clipped on the panel rather than reported anywhere.
                check(strlen(buf) <= 5, cases[i].want);
            }
        }
        char small[4];
        check(!RideSummary_FormatDurationShort(60, small, sizeof(small)), "a short buffer refuses");
        check(small[0] == '\0', "and leaves nothing behind");
        check(!RideSummary_FormatDurationShort(60, NULL, 10), "so does a null one");
    }
    printf("done\n");

    printf("- an empty ride is recognised as having nothing to say: ");
    {
        RideSummary_t s;
        memset(&s, 0, sizeof(s));
        check(RideSummary_IsEmpty(&s), "a zeroed ride");
        check(RideSummary_IsEmpty(NULL), "and a null one");

        // Either signal alone is enough. A ride can be all distance and no
        // recorded moving time if the fixes arrived in one burst, and a second
        // of moving time with no distance is a rider who never left home.
        s.distance_km = 0.4;
        check(!RideSummary_IsEmpty(&s), "distance alone is worth reporting");

        memset(&s, 0, sizeof(s));
        s.moving_seconds = 1.0;
        check(!RideSummary_IsEmpty(&s), "so is moving time alone");

        memset(&s, 0, sizeof(s));
        s.moving_seconds = 0.4;
        check(RideSummary_IsEmpty(&s), "but under a second is not");

        // A heart rate without either is a rider sitting on the sofa wearing a
        // strap, which is not a ride.
        memset(&s, 0, sizeof(s));
        s.avg_bpm = 62;
        s.max_bpm = 71;
        check(RideSummary_IsEmpty(&s), "a strap alone is not a ride");
    }
    printf("done\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
