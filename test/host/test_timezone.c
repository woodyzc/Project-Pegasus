/* Host tests for lat/lon -> POSIX TZ and civil UTC -> epoch
 * (src/system/TimeZone.c).
 *
 * These run the second half too: with the TZ string applied via setenv/tzset,
 * localtime_r() is the same newlib implementation the ESP32 uses, so a test
 * that a July afternoon in Virginia shows as EDT genuinely exercises the DST
 * rule the firmware will rely on. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "TimeZone.h"

static int checks = 0;
static int failures = 0;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_str(const char *got, const char *want, const char *what) {
    checks++;
    if (strcmp(got, want) != 0) {
        failures++;
        printf("  FAIL: %s (got '%s', want '%s')\n", what, got, want);
    }
}

/* Applies a TZ string and formats an epoch as local "HH:MM %Z". */
static void local_string(const char *tz, int64_t epoch, char *out, size_t n) {
    time_t t = (time_t)epoch;
    struct tm local;
    setenv("TZ", tz, 1);
    tzset();
    localtime_r(&t, &local);
    strftime(out, n, "%H:%M %Z", &local);
}

int main(void) {
    bool approx = false;
    char buf[64];

    printf("- civil UTC to epoch: ");
    /* 1970-01-01T00:00:00Z is the definition of zero. */
    check(TimeZone_UtcToEpoch(1970, 1, 1, 0, 0, 0) == 0, "epoch zero");
    check(TimeZone_UtcToEpoch(1970, 1, 2, 0, 0, 0) == 86400, "one day");
    /* 2000-03-01 is the classic leap-year trap: 2000 IS a leap year. */
    check(TimeZone_UtcToEpoch(2000, 3, 1, 0, 0, 0) == 951868800, "2000-03-01 (leap century)");
    check(TimeZone_UtcToEpoch(2026, 9, 10, 14, 32, 7) == 1789050727, "a 2026 timestamp");
    /* 1900 was NOT a leap year, so 2100 will not be either. */
    check(TimeZone_UtcToEpoch(2100, 3, 1, 0, 0, 0) == 4107542400, "2100-03-01 (non-leap century)");
    printf("done\n");

    printf("- US zones resolve, including the Arizona exception: ");
    check_str(TimeZone_PosixFor(38.8821, -77.0194, &approx), "EST5EDT,M3.2.0,M11.1.0",
              "McLean VA -> Eastern");
    check(!approx, "and is not a fallback");
    check_str(TimeZone_PosixFor(41.8781, -87.6298, &approx), "CST6CDT,M3.2.0,M11.1.0",
              "Chicago -> Central");
    check_str(TimeZone_PosixFor(37.7749, -122.4194, &approx), "PST8PDT,M3.2.0,M11.1.0",
              "San Francisco -> Pacific");
    /* Denver at -104.99 is the case a naive -105 band gets wrong: it is
       Mountain, and the real border is Colorado's eastern edge near -102. */
    check_str(TimeZone_PosixFor(39.7392, -104.9903, &approx), "MST7MDT,M3.2.0,M11.1.0",
              "Denver -> Mountain");
    check_str(TimeZone_PosixFor(35.2220, -101.8313, &approx), "CST6CDT,M3.2.0,M11.1.0",
              "Amarillo, just east of the border -> Central");
    check_str(TimeZone_PosixFor(31.7619, -106.4850, &approx), "MST7MDT,M3.2.0,M11.1.0",
              "El Paso -> Mountain");
    /* Phoenix sits inside the Mountain rectangle but keeps standard time all
       year, which is why its entry has to be tested before Mountain's. */
    check_str(TimeZone_PosixFor(33.4484, -112.0740, &approx), "MST7",
              "Phoenix -> Mountain, no DST");
    check(!approx, "Arizona is a listed region, not a guess");
    printf("done\n");

    printf("- DST is applied by the C library, not by us: ");
    /* 2026-01-15 18:00Z in Virginia: winter, EST = UTC-5 -> 13:00 */
    local_string("EST5EDT,M3.2.0,M11.1.0", TimeZone_UtcToEpoch(2026, 1, 15, 18, 0, 0), buf,
                 sizeof(buf));
    check_str(buf, "13:00 EST", "January -> EST");
    /* 2026-07-15 18:00Z: summer, EDT = UTC-4 -> 14:00 */
    local_string("EST5EDT,M3.2.0,M11.1.0", TimeZone_UtcToEpoch(2026, 7, 15, 18, 0, 0), buf,
                 sizeof(buf));
    check_str(buf, "14:00 EDT", "July -> EDT");
    /* Phoenix never shifts. */
    local_string("MST7", TimeZone_UtcToEpoch(2026, 7, 15, 18, 0, 0), buf, sizeof(buf));
    check_str(buf, "11:00 MST", "Phoenix stays MST in July");
    printf("done\n");

    printf("- overlapping regions resolve by table order: ");
    /* Lhasa is inside both the China and India rectangles; China is listed
       first, and all of China runs on one offset. */
    check_str(TimeZone_PosixFor(29.65, 91.13, &approx), "CST-8", "Lhasa -> China");
    check_str(TimeZone_PosixFor(31.2304, 121.4737, &approx), "CST-8", "Shanghai -> China");
    check_str(TimeZone_PosixFor(35.6762, 139.6503, &approx), "JST-9", "Tokyo -> Japan");
    printf("done\n");

    printf("- half-hour offsets survive: ");
    /* India is UTC+5:30 -- a whole-hour-only implementation would fail here. */
    check_str(TimeZone_PosixFor(28.6139, 70.0, &approx), "IST-5:30", "India -> IST");
    local_string("IST-5:30", TimeZone_UtcToEpoch(2026, 7, 15, 6, 0, 0), buf, sizeof(buf));
    check_str(buf, "11:30 IST", "06:00Z -> 11:30 IST");
    printf("done\n");

    printf("- unlisted positions fall back to solar time, flagged: ");
    /* Mid-Atlantic: no region covers it. */
    const char *tz = TimeZone_PosixFor(0.0, -30.0, &approx);
    check(approx, "fallback is reported as approximate");
    check_str(tz, "UTC+2", "-30 deg -> UTC-2, written POSIX-inverted");
    local_string(tz, TimeZone_UtcToEpoch(2026, 7, 15, 12, 0, 0), buf, sizeof(buf));
    check(strncmp(buf, "10:00", 5) == 0, "12:00Z reads 10:00 local");

    TimeZone_PosixFor(0.0, 30.0, &approx);
    check(approx, "eastern hemisphere fallback also flagged");
    check_str(TimeZone_PosixFor(0.0, 30.0, &approx), "UTC-2", "+30 deg -> UTC+2");
    printf("done\n");

    printf("- extreme longitudes are clamped, not wrapped: ");
    check_str(TimeZone_PosixFor(0.0, 179.9, &approx), "UTC-12", "+180 clamps to +12");
    check_str(TimeZone_PosixFor(0.0, -179.9, &approx), "UTC+12", "-180 clamps to -12");
    printf("done\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
