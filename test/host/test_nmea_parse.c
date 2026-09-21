/* Host-side tests for the NMEA GGA/RMC parser (src/sensors/NmeaParse.c).
 *
 * The ATGM336H that replaced the never-fitted MAX-M10S on the bench speaks
 * plain NMEA, not UBX, so this is the receiver path actually wired up right
 * now. As with test_ubx_parse.c, this is the only verification the decoder
 * has -- field positions, the ddmm.mmmm-to-degrees conversion and the
 * checksum are each pinned separately. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "NmeaParse.h"

static int checks = 0;
static int failures = 0;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_close(double got, double want, double tol, const char *what) {
    checks++;
    if (fabs(got - want) > tol) {
        failures++;
        printf("  FAIL: %s (got %.7f, want %.7f)\n", what, got, want);
    }
}

/* Appends "*CC\r\n" to a bare "$...,fields" body, computing CC the same way
   the parser checks it: XOR of every byte between '$' and '*'. */
static size_t build_sentence(char *out, const char *body) {
    size_t n = strlen(body);
    uint8_t sum = 0;
    size_t i;
    char hex[3];

    memcpy(out, body, n);
    for (i = 1; i < n; i++) {
        sum ^= (uint8_t)body[i];
    }
    snprintf(hex, sizeof(hex), "%02X", sum);
    out[n++] = '*';
    out[n++] = hex[0];
    out[n++] = hex[1];
    out[n++] = '\r';
    out[n++] = '\n';
    return n;
}

static int feed_all(NmeaParser_t *p, const char *buf, size_t n, NmeaFix_t *out) {
    int count = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        if (Nmea_Feed(p, (uint8_t)buf[i], out)) {
            count++;
        }
    }
    return count;
}

int main(void) {
    char buf[256];
    char line[256];
    size_t len;
    NmeaParser_t parser;
    NmeaFix_t fix;

    printf("- a GGA with no RMC yet decodes position/fix/altitude, leaves speed/heading/time unset: ");
    len = build_sentence(buf, "$GNGGA,143222.00,3852.9260,N,07701.1640,W,1,08,1.01,105.4,M,-34.0,M,,");
    Nmea_Init(&parser);
    memset(&fix, 0, sizeof(fix));
    check(feed_all(&parser, buf, len, &fix) == 1, "one GGA completes");
    check(fix.fix_valid, "fix quality 1 -> valid");
    check(fix.fix_quality == 1, "raw fix quality survives");
    check(fix.num_sv == 8, "numSV survives");
    check_close(fix.lat_deg, 38.882100, 1e-5, "lat ddmm.mmmm -> degrees");
    check_close(fix.lon_deg, -77.019400, 1e-5, "lon dddmm.mmmm -> degrees, W is negative");
    check_close(fix.alt_m, 105.4, 1e-3, "altitude survives");
    check(!fix.heading_valid, "no RMC seen yet -> heading not valid");
    check(!fix.time_valid, "no RMC seen yet -> time not valid");
    printf("done\n");

    printf("- fix quality 0 means no fix, and Nmea_Feed blanks the position rather than keep a stale one: ");
    len = build_sentence(buf, "$GNGGA,143222.00,,,,,0,00,,,M,,M,,");
    Nmea_Init(&parser);
    memset(&fix, 0, sizeof(fix));
    fix.lat_deg = 12.0; /* stands in for a previous, real fix already on screen */
    check(feed_all(&parser, buf, len, &fix) == 1, "GGA with no fix still decodes");
    check(!fix.fix_valid, "quality 0 -> not valid");
    check(fix.num_sv == 0, "numSV 0 while acquiring");
    check_close(fix.lat_deg, 0.0, 1e-9, "position blanked, same rule as the dashboard's own staleness handling");
    printf("done\n");

    printf("- Nmea_DecodeGGA on its own, below Nmea_Feed's blanking, leaves other fields as documented: ");
    {
        const char *body = "$GNGGA,143222.00,,,,,0,00,,,M,,M,,";
        char sentence[128];
        size_t body_len = strlen(body);
        memcpy(sentence, body, body_len);
        memset(&fix, 0, sizeof(fix));
        fix.lat_deg = 12.0;
        check(Nmea_DecodeGGA(sentence, body_len, &fix), "decodes without a checksum suffix");
        check_close(fix.lat_deg, 12.0, 1e-9, "no fix -> position field left as the caller had it");
    }
    printf("done\n");

    printf("- an RMC caches speed/heading/date, and the next GGA merges it in: ");
    Nmea_Init(&parser);
    len = build_sentence(buf, "$GNRMC,143222.00,A,3852.9260,N,07701.1640,W,10.5,271.3,210926,,,A");
    check(feed_all(&parser, buf, len, &fix) == 0, "RMC alone does not publish");
    len = build_sentence(buf, "$GNGGA,143222.00,3852.9260,N,07701.1640,W,1,08,1.01,105.4,M,-34.0,M,,");
    memset(&fix, 0, sizeof(fix));
    check(feed_all(&parser, buf, len, &fix) == 1, "GGA after RMC publishes");
    check_close(fix.speed_mps, 10.5 * 0.514444, 1e-3, "knots -> m/s");
    check(fix.heading_valid, "course over ground present");
    check_close(fix.heading_deg, 271.3, 1e-3, "heading survives");
    check(fix.time_valid, "status A -> time valid");
    check(fix.year == 2026 && fix.month == 9 && fix.day == 21, "ddmmyy decodes, y2k-assumed");
    check(fix.hour == 14 && fix.minute == 32 && fix.second == 22, "hhmmss decodes");
    printf("done\n");

    printf("- a stopped rider: RMC status V, blank course, must not read as heading north: ");
    Nmea_Init(&parser);
    len = build_sentence(buf, "$GNRMC,143222.00,V,3852.9260,N,07701.1640,W,0.0,,210926,,,N");
    feed_all(&parser, buf, len, &fix);
    len = build_sentence(buf, "$GNGGA,143222.00,3852.9260,N,07701.1640,W,1,08,1.01,105.4,M,-34.0,M,,");
    memset(&fix, 0, sizeof(fix));
    check(feed_all(&parser, buf, len, &fix) == 1, "GGA publishes");
    check(!fix.heading_valid, "blank course -> heading not valid");
    check(!fix.time_valid, "status V -> time not valid");
    printf("done\n");

    printf("- a later RMC replaces the cache, not just adds to it: ");
    Nmea_Init(&parser);
    len = build_sentence(buf, "$GNRMC,143222.00,A,3852.9260,N,07701.1640,W,10.5,271.3,210926,,,A");
    feed_all(&parser, buf, len, &fix);
    len = build_sentence(buf, "$GNRMC,143322.00,A,3852.9260,N,07701.1640,W,0.0,,210926,,,A");
    feed_all(&parser, buf, len, &fix);
    len = build_sentence(buf, "$GNGGA,143322.00,3852.9260,N,07701.1640,W,1,08,1.01,105.4,M,-34.0,M,,");
    memset(&fix, 0, sizeof(fix));
    check(feed_all(&parser, buf, len, &fix) == 1, "GGA publishes");
    check_close(fix.speed_mps, 0.0, 1e-6, "second RMC's speed wins");
    check(!fix.heading_valid, "second RMC's blank course wins");
    check(fix.second == 22, "second RMC's time wins");
    printf("done\n");

    printf("- southern and eastern hemispheres, and the 0/0 sentinel case: ");
    len = build_sentence(buf, "$GNGGA,000000.00,3352.9260,S,15112.3450,E,1,12,0.8,20.0,M,0.0,M,,");
    Nmea_Init(&parser);
    memset(&fix, 0, sizeof(fix));
    check(feed_all(&parser, buf, len, &fix) == 1, "accepts");
    check(fix.lat_deg < 0.0, "S -> negative latitude");
    check(fix.lon_deg > 0.0, "E -> positive longitude");
    printf("done\n");

    printf("- a bad checksum is rejected whole: ");
    len = build_sentence(buf, "$GNGGA,143222.00,3852.9260,N,07701.1640,W,1,08,1.01,105.4,M,-34.0,M,,");
    buf[len - 4] ^= 0x0F; /* flip one checksum hex digit */
    Nmea_Init(&parser);
    memset(&fix, 0, sizeof(fix));
    check(feed_all(&parser, buf, len, &fix) == 0, "bad checksum rejected");
    printf("done\n");

    printf("- a corrupted payload byte is caught by the checksum: ");
    len = build_sentence(buf, "$GNGGA,143222.00,3852.9260,N,07701.1640,W,1,08,1.01,105.4,M,-34.0,M,,");
    buf[10] ^= 0x01; /* one bit inside the time field */
    Nmea_Init(&parser);
    memset(&fix, 0, sizeof(fix));
    check(feed_all(&parser, buf, len, &fix) == 0, "flipped byte rejected");
    printf("done\n");

    printf("- unrelated sentences (GSA/VTG) are skipped without desyncing: ");
    {
        char noisy[512];
        size_t n = 0;
        n += build_sentence(noisy + n, "$GNGSA,A,3,10,12,25,,,,,,,,,,1.2,0.8,0.9");
        n += build_sentence(noisy + n, "$GPVTG,271.3,T,,M,10.5,N,19.4,K,A");
        n += build_sentence(noisy + n, "$GNGGA,143222.00,3852.9260,N,07701.1640,W,1,08,1.01,105.4,M,-34.0,M,,");
        Nmea_Init(&parser);
        memset(&fix, 0, sizeof(fix));
        check(feed_all(&parser, noisy, n, &fix) == 1, "only the GGA publishes");
        check_close(fix.lat_deg, 38.8821, 1e-4, "and it decoded correctly");
    }
    printf("done\n");

    printf("- a '$' that arrives mid-sentence resyncs onto the next one: ");
    {
        char noisy[512];
        size_t n = 0;
        const char *garbled = "$GNGGA,143222.00,3852"; /* truncated, never terminated */
        size_t i;
        for (i = 0; i < strlen(garbled); i++) {
            noisy[n++] = garbled[i];
        }
        n += build_sentence(noisy + n, "$GNGGA,143222.00,3852.9260,N,07701.1640,W,1,08,1.01,105.4,M,-34.0,M,,");
        Nmea_Init(&parser);
        memset(&fix, 0, sizeof(fix));
        check(feed_all(&parser, noisy, n, &fix) == 1, "the second, complete sentence publishes");
    }
    printf("done\n");

    printf("- an oversized line does not overflow the buffer and recovers after: ");
    {
        char noisy[512];
        size_t n = 0;
        size_t i;
        noisy[n++] = '$';
        for (i = 0; i < 400; i++) {
            noisy[n++] = 'A';
        }
        noisy[n++] = '\r';
        noisy[n++] = '\n';
        Nmea_Init(&parser);
        memset(&fix, 0, sizeof(fix));
        check(feed_all(&parser, noisy, n, &fix) == 0, "oversized line produces nothing");

        len = build_sentence(buf, "$GNGGA,143222.00,3852.9260,N,07701.1640,W,1,08,1.01,105.4,M,-34.0,M,,");
        check(feed_all(&parser, buf, len, &fix) == 1, "parser recovered for the next sentence");
    }
    printf("done\n");

    printf("- Nmea_ChecksumOk and the decode helpers directly: ");
    {
        const char *good = "$GNGGA,143222.00,3852.9260,N,07701.1640,W,1,08,1.01,105.4,M,-34.0,M,,*7A";
        check(!Nmea_ChecksumOk(good, strlen(good)), "wrong checksum by construction rejected");
    }
    len = build_sentence(line, "$GNGGA,143222.00,3852.9260,N,07701.1640,W,1,08,1.01,105.4,M,-34.0,M,,");
    line[len - 2] = '\0'; /* strip trailing \r\n for the direct checksum check */
    check(Nmea_ChecksumOk(line, strlen(line)), "a correctly built checksum validates");
    printf("done\n");

    printf("- null arguments: ");
    Nmea_Init(NULL); /* must not crash */
    Nmea_Init(&parser);
    check(!Nmea_Feed(NULL, '$', &fix), "null parser");
    check(!Nmea_Feed(&parser, '$', NULL), "null output");
    check(!Nmea_DecodeGGA(NULL, 10, &fix), "null sentence, GGA");
    check(!Nmea_DecodeRMC(NULL, 10, &fix), "null sentence, RMC");
    check(!Nmea_ChecksumOk(NULL, 10), "null sentence, checksum");
    printf("done\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
