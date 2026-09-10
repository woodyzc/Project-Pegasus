/* Host-side tests for the UBX NAV-PVT parser (src/sensors/UbxParse.c).
 *
 * No GPS module has been connected to this project yet, so these tests are
 * the only verification this code has. They are written accordingly: the
 * field offsets, the scaling factors and the checksum are each pinned
 * independently, because a parser that is subtly wrong reports a plausible
 * position in the wrong place -- far worse than reporting nothing. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "UbxParse.h"

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

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put_i32(uint8_t *p, int32_t v) {
    uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)(u & 0xFF);
    p[1] = (uint8_t)((u >> 8) & 0xFF);
    p[2] = (uint8_t)((u >> 16) & 0xFF);
    p[3] = (uint8_t)((u >> 24) & 0xFF);
}

/* A NAV-PVT payload for a valid 3D fix near McLean, VA, moving at 5 m/s. */
static void build_payload(uint8_t *pl) {
    memset(pl, 0, UBX_NAV_PVT_LEN);
    put_u16(pl + 4, 2026); /* year */
    pl[6] = 9;             /* month */
    pl[7] = 10;            /* day */
    pl[8] = 14;            /* hour */
    pl[9] = 32;            /* min */
    pl[10] = 7;            /* sec */
    pl[11] = 0x07;         /* validDate | validTime | fullyResolved */
    pl[20] = 3;            /* fixType: 3D */
    pl[21] = 0x01;         /* gnssFixOK */
    pl[23] = 11;           /* numSV */
    put_i32(pl + 24, -770194000); /* lon = -77.0194 deg */
    put_i32(pl + 28, 388821000);  /* lat =  38.8821 deg */
    put_i32(pl + 36, 105000);     /* hMSL = 105.000 m */
    put_i32(pl + 60, 5000);       /* gSpeed = 5.000 m/s */
    put_i32(pl + 64, 9000000);    /* headMot = 90.00000 deg */
}

/* Wraps a payload into a full UBX frame with a correct checksum. */
static size_t build_frame(uint8_t *buf, uint8_t cls, uint8_t id, const uint8_t *pl, uint16_t len) {
    size_t n = 0;
    uint8_t ck_a = 0, ck_b = 0;
    size_t i;

    buf[n++] = 0xB5;
    buf[n++] = 0x62;
    buf[n++] = cls;
    buf[n++] = id;
    buf[n++] = (uint8_t)(len & 0xFF);
    buf[n++] = (uint8_t)(len >> 8);
    for (i = 0; i < len; i++) {
        buf[n++] = pl[i];
    }
    for (i = 2; i < 6 + (size_t)len; i++) {
        ck_a = (uint8_t)(ck_a + buf[i]);
        ck_b = (uint8_t)(ck_b + ck_a);
    }
    buf[n++] = ck_a;
    buf[n++] = ck_b;
    return n;
}

/* Feeds a whole buffer; returns how many NAV-PVT frames completed. */
static int feed_all(UbxParser_t *p, const uint8_t *buf, size_t n, UbxNavPvt_t *out) {
    int count = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        if (Ubx_Feed(p, buf[i], out)) {
            count++;
        }
    }
    return count;
}

int main(void) {
    uint8_t pl[UBX_NAV_PVT_LEN];
    uint8_t frame[256];
    uint8_t noisy[512];
    UbxParser_t parser;
    UbxNavPvt_t pvt;
    size_t len;

    printf("- a clean NAV-PVT frame decodes every field: ");
    build_payload(pl);
    len = build_frame(frame, UBX_NAV_PVT_CLASS, UBX_NAV_PVT_ID, pl, UBX_NAV_PVT_LEN);
    Ubx_Init(&parser);
    memset(&pvt, 0, sizeof(pvt));
    check(feed_all(&parser, frame, len, &pvt) == 1, "one frame completes");
    check(pvt.fix_valid, "fix reported valid");
    check(pvt.fix_type == 3, "fixType survives");
    check(pvt.num_sv == 11, "numSV survives");
    /* Longitude is at offset 24 and latitude at 28 -- reversed from how they
       are normally spoken. Swapping them puts you in the wrong hemisphere. */
    check_close(pvt.lon_deg, -77.0194, 1e-6, "longitude, 1e-7 scaling");
    check_close(pvt.lat_deg, 38.8821, 1e-6, "latitude, 1e-7 scaling");
    check_close(pvt.alt_m, 105.0, 1e-3, "hMSL mm -> m");
    check_close(pvt.speed_mps, 5.0, 1e-3, "gSpeed mm/s -> m/s");
    check_close(pvt.heading_deg, 90.0, 1e-3, "headMot 1e-5 deg");
    check(pvt.time_valid, "time reported valid");
    check(pvt.year == 2026 && pvt.month == 9 && pvt.day == 10, "date survives");
    check(pvt.hour == 14 && pvt.minute == 32 && pvt.second == 7, "time survives");
    printf("done\n");

    printf("- negative and zero coordinates: ");
    build_payload(pl);
    put_i32(pl + 24, -1);
    put_i32(pl + 28, -900000000); /* -90 deg, south pole */
    len = build_frame(frame, UBX_NAV_PVT_CLASS, UBX_NAV_PVT_ID, pl, UBX_NAV_PVT_LEN);
    Ubx_Init(&parser);
    check(feed_all(&parser, frame, len, &pvt) == 1, "accepts");
    check_close(pvt.lon_deg, -0.0000001, 1e-9, "smallest negative longitude");
    check_close(pvt.lat_deg, -90.0, 1e-6, "south pole latitude");
    printf("done\n");

    printf("- fix validity needs BOTH gnssFixOK and a 2D-or-better fixType: ");
    build_payload(pl);
    pl[21] = 0x00; /* gnssFixOK clear, fixType still 3 */
    len = build_frame(frame, UBX_NAV_PVT_CLASS, UBX_NAV_PVT_ID, pl, UBX_NAV_PVT_LEN);
    Ubx_Init(&parser);
    check(feed_all(&parser, frame, len, &pvt) == 1, "frame still parses");
    check(!pvt.fix_valid, "gnssFixOK clear -> not valid");

    build_payload(pl);
    pl[20] = 0; /* no fix, gnssFixOK still set */
    len = build_frame(frame, UBX_NAV_PVT_CLASS, UBX_NAV_PVT_ID, pl, UBX_NAV_PVT_LEN);
    Ubx_Init(&parser);
    check(feed_all(&parser, frame, len, &pvt) == 1, "frame still parses");
    check(!pvt.fix_valid, "fixType 0 -> not valid");

    build_payload(pl);
    pl[20] = 2; /* 2D fix is acceptable */
    len = build_frame(frame, UBX_NAV_PVT_CLASS, UBX_NAV_PVT_ID, pl, UBX_NAV_PVT_LEN);
    Ubx_Init(&parser);
    check(feed_all(&parser, frame, len, &pvt) == 1, "frame parses");
    check(pvt.fix_valid, "2D fix accepted");
    printf("done\n");

    printf("- time needs fullyResolved, not just validDate/validTime: ");
    build_payload(pl);
    pl[11] = 0x03; /* date + time valid, but NOT fullyResolved */
    len = build_frame(frame, UBX_NAV_PVT_CLASS, UBX_NAV_PVT_ID, pl, UBX_NAV_PVT_LEN);
    Ubx_Init(&parser);
    check(feed_all(&parser, frame, len, &pvt) == 1, "frame parses");
    check(!pvt.time_valid, "not fully resolved -> time not valid");
    printf("done\n");

    printf("- heading normalises into 0-360: ");
    build_payload(pl);
    put_i32(pl + 64, -9000000); /* -90 deg */
    len = build_frame(frame, UBX_NAV_PVT_CLASS, UBX_NAV_PVT_ID, pl, UBX_NAV_PVT_LEN);
    Ubx_Init(&parser);
    check(feed_all(&parser, frame, len, &pvt) == 1, "accepts");
    check_close(pvt.heading_deg, 270.0, 1e-3, "-90 becomes 270");
    printf("done\n");

    printf("- a corrupted checksum is rejected whole: ");
    build_payload(pl);
    len = build_frame(frame, UBX_NAV_PVT_CLASS, UBX_NAV_PVT_ID, pl, UBX_NAV_PVT_LEN);
    frame[len - 1] ^= 0xFF; /* break CK_B */
    Ubx_Init(&parser);
    check(feed_all(&parser, frame, len, &pvt) == 0, "bad CK_B rejected");

    len = build_frame(frame, UBX_NAV_PVT_CLASS, UBX_NAV_PVT_ID, pl, UBX_NAV_PVT_LEN);
    frame[len - 2] ^= 0xFF; /* break CK_A */
    Ubx_Init(&parser);
    check(feed_all(&parser, frame, len, &pvt) == 0, "bad CK_A rejected");

    /* A single flipped payload byte must fail the checksum, not slip through
       as a slightly wrong position. */
    len = build_frame(frame, UBX_NAV_PVT_CLASS, UBX_NAV_PVT_ID, pl, UBX_NAV_PVT_LEN);
    frame[6 + 28] ^= 0x01; /* one bit of latitude */
    Ubx_Init(&parser);
    check(feed_all(&parser, frame, len, &pvt) == 0, "flipped payload bit rejected");
    printf("done\n");

    printf("- other UBX messages are skipped without desyncing: ");
    {
        uint8_t other[40];
        size_t n = 0;
        memset(other, 0xAA, sizeof(other));
        /* NAV-STATUS (0x01 0x03), then a real NAV-PVT after it. */
        n += build_frame(noisy + n, 0x01, 0x03, other, 16);
        build_payload(pl);
        n += build_frame(noisy + n, UBX_NAV_PVT_CLASS, UBX_NAV_PVT_ID, pl, UBX_NAV_PVT_LEN);
        Ubx_Init(&parser);
        check(feed_all(&parser, noisy, n, &pvt) == 1, "only the NAV-PVT is reported");
        check_close(pvt.lat_deg, 38.8821, 1e-6, "and it decoded correctly");
    }
    printf("done\n");

    printf("- leading noise and NMEA do not prevent a later frame: ");
    {
        const char *nmea = "$GNGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
        size_t n = 0;
        size_t i;
        for (i = 0; i < strlen(nmea); i++) {
            noisy[n++] = (uint8_t)nmea[i];
        }
        noisy[n++] = 0xB5; /* false start: sync1 with no sync2 */
        noisy[n++] = 0x00;
        noisy[n++] = 0xB5; /* B5 B5 62 must still be a valid start */
        build_payload(pl);
        n += build_frame(noisy + n, UBX_NAV_PVT_CLASS, UBX_NAV_PVT_ID, pl, UBX_NAV_PVT_LEN);
        Ubx_Init(&parser);
        check(feed_all(&parser, noisy, n, &pvt) == 1, "frame found after noise");
    }
    printf("done\n");

    printf("- oversized and truncated messages: ");
    {
        uint8_t big[8];
        size_t n;
        memset(big, 0x5A, sizeof(big));
        /* Declares 500 bytes, far beyond the buffer: must not overflow, and
           must leave the parser able to find the next frame. */
        n = 0;
        noisy[n++] = 0xB5;
        noisy[n++] = 0x62;
        noisy[n++] = 0x01;
        noisy[n++] = 0x07;
        noisy[n++] = 0xF4; /* len = 500 */
        noisy[n++] = 0x01;
        {
            size_t i;
            for (i = 0; i < 500; i++) {
                noisy[n++] = 0x5A;
            }
        }
        noisy[n++] = 0x00; /* deliberately wrong checksum */
        noisy[n++] = 0x00;
        Ubx_Init(&parser);
        check(feed_all(&parser, noisy, n, &pvt) == 0, "oversized frame produces nothing");

        build_payload(pl);
        n = build_frame(frame, UBX_NAV_PVT_CLASS, UBX_NAV_PVT_ID, pl, UBX_NAV_PVT_LEN);
        check(feed_all(&parser, frame, n, &pvt) == 1, "parser recovered for the next frame");

        /* Truncated: header claims 92 bytes but the stream ends early. */
        n = build_frame(frame, UBX_NAV_PVT_CLASS, UBX_NAV_PVT_ID, pl, UBX_NAV_PVT_LEN);
        Ubx_Init(&parser);
        check(feed_all(&parser, frame, n - 10, &pvt) == 0, "truncated frame produces nothing");
        (void)big;
    }
    printf("done\n");

    printf("- null arguments: ");
    Ubx_Init(NULL); /* must not crash */
    Ubx_Init(&parser);
    check(!Ubx_Feed(NULL, 0xB5, &pvt), "null parser");
    check(!Ubx_Feed(&parser, 0xB5, NULL), "null output");
    check(!Ubx_DecodeNavPvt(NULL, UBX_NAV_PVT_LEN, &pvt), "null payload");
    check(!Ubx_DecodeNavPvt(pl, 10, &pvt), "payload too short");
    printf("done\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
