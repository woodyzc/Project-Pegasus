/* Host-side tests for the clock's source ranking (src/system/TimeSource.c).
 *
 * The ranking is the whole point and it is not symmetric: GNSS outranks the
 * phone for the TIME and the phone outranks GNSS for the ZONE, because a
 * satellite knows the instant and has no idea what a human standing there
 * would call it. Getting that backwards shows a confidently wrong hour, which
 * is worse than dashes. */
#include <stdio.h>
#include <string.h>

#include "TimeSource.h"

static int checks = 0;
static int failures = 0;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* 2026-09-16T18:30:00Z */
#define NOW 1789583400u
#define MINUTE_MS (60u * 1000u)

int main(void) {
    TimeReading_t r;

    printf("- nothing is known until something says: ");
    TimeSource_Reset();
    check(!TimeSource_Now(1000, &r), "no reading before any source");
    printf("ok\n");

    printf("- the phone sets the clock and the zone: ");
    TimeSource_Reset();
    TimeSource_SetFromPhone(NOW, -240, "EDT", 1000);
    check(TimeSource_Now(1000, &r), "reads back");
    check(r.utc_seconds == NOW, "time survives");
    check(r.kind == TIME_SRC_PHONE, "source is the phone");
    check(r.offset_known && r.offset_min == -240, "offset survives");
    check(strcmp(r.zone, "EDT") == 0, "zone survives");
    printf("ok\n");

    printf("- the clock advances between updates: ");
    TimeSource_Reset();
    TimeSource_SetFromPhone(NOW, 0, "UTC", 1000);
    check(TimeSource_Now(1000 + 90u * 1000u, &r), "still valid");
    check(r.utc_seconds == NOW + 90, "90 seconds later");
    printf("ok\n");

    printf("- GNSS displaces the phone's time: ");
    TimeSource_Reset();
    TimeSource_SetFromPhone(NOW, -240, "EDT", 1000);
    TimeSource_SetFromGnss(NOW + 5, 2000);
    check(TimeSource_Now(2000, &r), "reads back");
    check(r.utc_seconds == NOW + 5, "GNSS time wins");
    check(r.kind == TIME_SRC_GNSS, "source is GNSS");
    printf("ok\n");

    printf("- the phone cannot displace fresh GNSS time: ");
    TimeSource_SetFromPhone(NOW + 3600, -240, "EDT", 3000);
    check(TimeSource_Now(3000, &r), "reads back");
    check(r.kind == TIME_SRC_GNSS, "still GNSS");
    check(r.utc_seconds == NOW + 5 + 1, "GNSS time, advanced by the tick");
    printf("ok\n");

    printf("- but the phone still supplies the zone under GNSS: ");
    /* The asymmetry. A fix gives the instant; only the phone knows that the
       rider calls it quarter past two in the afternoon. */
    check(r.offset_known && r.offset_min == -240, "offset came from the phone");
    check(strcmp(r.zone, "EDT") == 0, "so did the abbreviation");
    printf("ok\n");

    printf("- the phone takes over once GNSS goes stale: ");
    TimeSource_Reset();
    TimeSource_SetFromGnss(NOW, 1000);
    {
        const uint32_t later = 1000 + TIME_SOURCE_STALE_MS + 1;
        check(!TimeSource_Now(later, &r), "stale GNSS reads as nothing");
        TimeSource_SetFromPhone(NOW + 100, 60, "CET", later);
        check(TimeSource_Now(later, &r), "the phone is believed again");
        check(r.kind == TIME_SRC_PHONE, "source is the phone");
        check(r.utc_seconds == NOW + 100, "the phone's time");
    }
    printf("ok\n");

    printf("- a clock nobody has confirmed for hours stops being shown: ");
    TimeSource_Reset();
    TimeSource_SetFromPhone(NOW, 0, "UTC", 1000);
    check(TimeSource_Now(1000 + TIME_SOURCE_STALE_MS - 1, &r), "just inside is fine");
    check(!TimeSource_Now(1000 + TIME_SOURCE_STALE_MS, &r), "at the limit it is dropped");
    printf("ok\n");

    printf("- a stale offset is dropped even when the time is fresh: ");
    /* A phone that has been away for hours may have crossed a zone with its
       owner. Keeping the old offset would misplace every hour shown after. */
    TimeSource_Reset();
    TimeSource_SetFromPhone(NOW, -240, "EDT", 1000);
    {
        const uint32_t later = 1000 + TIME_SOURCE_STALE_MS + 5000;
        TimeSource_SetFromGnss(NOW + 10000, later);
        check(TimeSource_Now(later, &r), "the time is fresh");
        check(!r.offset_known, "the offset is not");
        check(r.zone[0] == '\0', "and neither is the zone");
    }
    printf("ok\n");

    printf("- the tick counter may wrap without breaking the clock: ");
    /* millis() wraps every 49 days. Unsigned subtraction keeps the elapsed
       time right across the wrap; a signed comparison would read as weeks. */
    TimeSource_Reset();
    {
        const uint32_t before = 0xFFFFF000u;
        TimeSource_SetFromPhone(NOW, 0, "UTC", before);
        check(TimeSource_Now(before + 8000u, &r), "still valid across the wrap");
        check(r.utc_seconds == NOW + 8, "8 seconds, not 49 days");
    }
    printf("ok\n");

    printf("- a null output is refused rather than crashed on: ");
    TimeSource_Reset();
    TimeSource_SetFromPhone(NOW, 0, "UTC", 1000);
    check(!TimeSource_Now(1000, NULL), "null reading out");
    TimeSource_SetFromPhone(NOW, 0, NULL, 1000);
    check(TimeSource_Now(1000, &r), "a null zone is allowed");
    check(r.zone[0] == '\0', "and reads back empty");
    printf("ok\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
