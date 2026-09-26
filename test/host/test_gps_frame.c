/* Host-side tests for the phone-supplied position frame (src/sensors/GpsFrame.c).
 *
 * This frame carries data straight off the air from another device and every
 * consumer downstream treats a published fix as true -- the odometer counts
 * it, the ride log writes it to the card, the router snaps to it. So the
 * rejection cases matter more here than in most parsers, and the boundary
 * values are tested on both sides rather than sampled. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "GpsFrame.h"

static int checks = 0;
static int failures = 0;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
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

/* A well-formed frame: a fix in the riding area, moving, with a valid time. */
static void build(uint8_t *b) {
    memset(b, 0, GPS_FRAME_LEN);
    b[0] = GPS_FRAME_MAGIC;
    b[1] = GPS_FRAME_VERSION;
    b[2] = 0x03; /* fix_valid | time_valid */
    b[3] = 9;    /* satellites */
    put_i32(b + 4, 418823000);   /*  41.8823 N */
    put_i32(b + 8, -881234000);  /* -88.1234 E */
    put_u16(b + 12, 617);        /* 6.17 m/s, about 22 km/h */
    put_u16(b + 14, (uint16_t)(int16_t)232); /* 232 m */
    put_u16(b + 16, 27350);      /* 273.50 degrees */
    put_u16(b + 18, 2026);
    b[20] = 9;
    b[21] = 17;
    b[22] = 20;
    b[23] = 3;
    b[24] = 45;
}

static int near(double a, double b, double eps) {
    return fabs(a - b) <= eps;
}

int main(void) {
    uint8_t b[GPS_FRAME_LEN];
    GpsFrame_t f;

    printf("- a well-formed frame decodes every field: ");
    build(b);
    check(Gps_ParseFrame(b, sizeof(b), &f), "accepted");
    check(f.fix_valid, "fix_valid set");
    check(f.time_valid, "time_valid set");
    check(f.num_sv == 9, "satellites");
    check(near(f.lat, 41.8823, 1e-6), "latitude");
    check(near(f.lon, -88.1234, 1e-6), "longitude");
    check(near(f.speed, 6.17, 1e-4), "speed in m/s, not cm/s");
    check(near(f.alt, 232.0, 1e-4), "altitude");
    check(near(f.heading, 273.5, 1e-4), "heading in degrees, not centidegrees");
    check(f.year == 2026 && f.month == 9 && f.day == 17, "date");
    check(f.hour == 20 && f.minute == 3 && f.second == 45, "time");
    printf("done\n");

    printf("- a southern, western fix keeps its sign: ");
    build(b);
    put_i32(b + 4, -337000000);  /* -33.7 */
    put_i32(b + 8, -704000000);  /* -70.4 */
    check(Gps_ParseFrame(b, sizeof(b), &f), "accepted");
    /* The reason ReadI32 goes via uint32_t: a direct shift into a signed type
     * is implementation defined once the sign bit is set, which it is for
     * every coordinate in the southern or western hemisphere. */
    check(near(f.lat, -33.7, 1e-6), "southern latitude");
    check(near(f.lon, -70.4, 1e-6), "western longitude");
    printf("done\n");

    printf("- the magic, version and length are all required: ");
    build(b);
    b[0] = 0x48;
    check(!Gps_ParseFrame(b, sizeof(b), &f), "wrong magic rejected");
    build(b);
    b[1] = 2;
    check(!Gps_ParseFrame(b, sizeof(b), &f), "unknown version rejected");
    build(b);
    check(!Gps_ParseFrame(b, GPS_FRAME_LEN - 1, &f), "short frame rejected");
    check(!Gps_ParseFrame(b, GPS_FRAME_LEN + 1, &f), "long frame rejected");
    check(!Gps_ParseFrame(NULL, GPS_FRAME_LEN, &f), "null data rejected");
    check(!Gps_ParseFrame(b, GPS_FRAME_LEN, NULL), "null out rejected");
    printf("done\n");

    printf("- no fix is reported, not rejected: ");
    build(b);
    b[2] = 0x00; /* neither flag */
    check(Gps_ParseFrame(b, sizeof(b), &f), "still a valid frame");
    check(!f.fix_valid, "and it says so");
    /* This distinction is the whole point: "the phone is here and still
     * acquiring" and "the phone is not talking" are different states, and only
     * a frame that arrives can report the first. */
    printf("done\n");

    printf("- a frame using reserved flag bits is refused: ");
    build(b);
    b[2] = 0x07;
    check(!Gps_ParseFrame(b, sizeof(b), &f), "reserved bit set");
    printf("done\n");

    printf("- coordinates are bounded on both sides: ");
    build(b);
    put_i32(b + 4, 900000000);
    check(Gps_ParseFrame(b, sizeof(b), &f), "exactly 90N is a place");
    put_i32(b + 4, 900000001);
    check(!Gps_ParseFrame(b, sizeof(b), &f), "beyond 90N is not");
    build(b);
    put_i32(b + 4, -900000001);
    check(!Gps_ParseFrame(b, sizeof(b), &f), "beyond 90S is not");
    build(b);
    put_i32(b + 8, 1800000000);
    check(Gps_ParseFrame(b, sizeof(b), &f), "exactly 180E is a place");
    put_i32(b + 8, 1800000001);
    check(!Gps_ParseFrame(b, sizeof(b), &f), "beyond 180E is not");
    printf("done\n");

    printf("- heading wraps below 360, and 360 itself is refused: ");
    build(b);
    put_u16(b + 16, 35999);
    check(Gps_ParseFrame(b, sizeof(b), &f), "359.99 accepted");
    check(near(f.heading, 359.99, 1e-3), "and decoded");
    put_u16(b + 16, 36000);
    check(!Gps_ParseFrame(b, sizeof(b), &f), "360.00 refused -- it is 0, spelled wrong");
    printf("done\n");

    printf("- the date is checked only when the sender vouches for it: ");
    build(b);
    b[2] = 0x01;  /* fix_valid, time NOT valid */
    b[20] = 0;    /* month 0, which a phone really does send while acquiring */
    b[21] = 0;
    check(Gps_ParseFrame(b, sizeof(b), &f), "a junk date rides along with a good fix");
    check(f.fix_valid && !f.time_valid, "and is flagged as unusable");
    build(b);
    b[20] = 13;   /* time_valid still set */
    check(!Gps_ParseFrame(b, sizeof(b), &f), "but a vouched-for month 13 is refused");
    build(b);
    b[24] = 60;
    check(Gps_ParseFrame(b, sizeof(b), &f), "second 60 accepted -- leap seconds exist");
    build(b);
    b[24] = 61;
    check(!Gps_ParseFrame(b, sizeof(b), &f), "second 61 does not");
    printf("done\n");

    printf("- a stationary rider is a fix, not an absence: ");
    build(b);
    put_u16(b + 12, 0);
    check(Gps_ParseFrame(b, sizeof(b), &f), "accepted");
    check(f.fix_valid && f.speed == 0.0f, "zero speed with a valid fix");
    printf("done\n");

    printf("- altitude below sea level survives the round trip: ");
    build(b);
    put_u16(b + 14, (uint16_t)(int16_t)-40);
    check(Gps_ParseFrame(b, sizeof(b), &f), "accepted");
    check(near(f.alt, -40.0, 1e-4), "Death Valley is a real ride");
    printf("done\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
