/* Host-side tests for the clock frame the phone writes over BLE
 * (src/system/ClockFrame.c).
 *
 * This frame sets the head unit's clock, and a clock that is wrong is worse
 * than one that is blank: ride files are named from it and their trackpoints
 * are stamped with it, so a bad value is written to the card permanently. The
 * rejection cases therefore matter more than the happy path. */
#include <stdio.h>
#include <string.h>

#include "ClockFrame.h"

static int checks = 0;
static int failures = 0;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static size_t build(uint8_t *buf, uint32_t seconds, int16_t offset, const char *zone) {
    size_t zone_len = zone ? strlen(zone) : 0;
    buf[0] = CLOCK_FRAME_MAGIC;
    buf[1] = CLOCK_FRAME_VERSION;
    buf[2] = (uint8_t)(seconds & 0xFF);
    buf[3] = (uint8_t)((seconds >> 8) & 0xFF);
    buf[4] = (uint8_t)((seconds >> 16) & 0xFF);
    buf[5] = (uint8_t)((seconds >> 24) & 0xFF);
    buf[6] = (uint8_t)((uint16_t)offset & 0xFF);
    buf[7] = (uint8_t)(((uint16_t)offset >> 8) & 0xFF);
    buf[8] = (uint8_t)zone_len;
    if (zone_len) {
        memcpy(buf + CLOCK_FRAME_HEADER_LEN, zone, zone_len);
    }
    return CLOCK_FRAME_HEADER_LEN + zone_len;
}

int main(void) {
    uint8_t buf[64];
    uint32_t seconds;
    int16_t offset;
    char zone[CLOCK_ZONE_LEN + 1];
    size_t len;

    /* 2026-09-16T18:30:00Z */
    const uint32_t kNow = 1789583400u;

    printf("- a well-formed frame round-trips: ");
    len = build(buf, kNow, -240, "EDT");
    check(Clock_ParseFrame(buf, len, &seconds, &offset, zone, sizeof(zone)), "accepts");
    check(seconds == kNow, "seconds survive");
    check(offset == -240, "negative offset survives");
    check(strcmp(zone, "EDT") == 0, "zone survives");
    printf("ok\n");

    printf("- a positive offset survives: ");
    len = build(buf, kNow, 330, "IST"); /* India, +5:30 -- the half-hour case */
    check(Clock_ParseFrame(buf, len, &seconds, &offset, zone, sizeof(zone)), "accepts");
    check(offset == 330, "half-hour offset survives");
    printf("ok\n");

    printf("- a frame with no zone is fine: ");
    len = build(buf, kNow, 0, "");
    check(Clock_ParseFrame(buf, len, &seconds, &offset, zone, sizeof(zone)), "accepts");
    check(zone[0] == '\0', "zone is empty");
    check(offset == 0, "UTC offset survives");
    printf("ok\n");

    printf("- the longest zone fits: ");
    len = build(buf, kNow, 0, "ABCDEFG");
    check(Clock_ParseFrame(buf, len, &seconds, &offset, zone, sizeof(zone)), "accepts 7 chars");
    check(strcmp(zone, "ABCDEFG") == 0, "all seven survive");
    printf("ok\n");

    printf("- a phone that has not set its own clock is rejected: ");
    /* Android reports 1970 before the network supplies a time, and accepting
       it would name a ride file half a century ago. */
    len = build(buf, 0, 0, "UTC");
    check(!Clock_ParseFrame(buf, len, &seconds, &offset, zone, sizeof(zone)), "rejects epoch 0");
    len = build(buf, CLOCK_EPOCH_FLOOR - 1, 0, "UTC");
    check(!Clock_ParseFrame(buf, len, &seconds, &offset, zone, sizeof(zone)),
          "rejects just below the floor");
    len = build(buf, CLOCK_EPOCH_FLOOR, 0, "UTC");
    check(Clock_ParseFrame(buf, len, &seconds, &offset, zone, sizeof(zone)),
          "accepts the floor itself");
    printf("ok\n");

    printf("- an impossible offset is rejected: ");
    len = build(buf, kNow, CLOCK_OFFSET_MIN_LOWEST - 1, "XXX");
    check(!Clock_ParseFrame(buf, len, &seconds, &offset, zone, sizeof(zone)), "rejects under -12h");
    len = build(buf, kNow, CLOCK_OFFSET_MIN_HIGHEST + 1, "XXX");
    check(!Clock_ParseFrame(buf, len, &seconds, &offset, zone, sizeof(zone)), "rejects over +14h");
    len = build(buf, kNow, CLOCK_OFFSET_MIN_LOWEST, "XXX");
    check(Clock_ParseFrame(buf, len, &seconds, &offset, zone, sizeof(zone)), "accepts -12h");
    len = build(buf, kNow, CLOCK_OFFSET_MIN_HIGHEST, "XXX");
    check(Clock_ParseFrame(buf, len, &seconds, &offset, zone, sizeof(zone)), "accepts +14h");
    printf("ok\n");

    printf("- malformed frames are rejected: ");
    len = build(buf, kNow, 0, "UTC");
    buf[0] = 0x54;
    check(!Clock_ParseFrame(buf, len, &seconds, &offset, zone, sizeof(zone)), "bad magic");

    len = build(buf, kNow, 0, "UTC");
    buf[1] = 0x02;
    check(!Clock_ParseFrame(buf, len, &seconds, &offset, zone, sizeof(zone)), "bad version");

    len = build(buf, kNow, 0, "UTC");
    buf[8] = CLOCK_ZONE_LEN + 1;
    check(!Clock_ParseFrame(buf, len, &seconds, &offset, zone, sizeof(zone)), "zone too long");

    len = build(buf, kNow, 0, "UTC");
    check(!Clock_ParseFrame(buf, len - 1, &seconds, &offset, zone, sizeof(zone)),
          "declared length exceeds what arrived");
    check(!Clock_ParseFrame(buf, len + 1, &seconds, &offset, zone, sizeof(zone)),
          "trailing junk");
    check(!Clock_ParseFrame(buf, CLOCK_FRAME_HEADER_LEN - 1, &seconds, &offset, zone,
                            sizeof(zone)),
          "short of a header");
    check(!Clock_ParseFrame(NULL, len, &seconds, &offset, zone, sizeof(zone)), "null data");
    check(!Clock_ParseFrame(buf, len, NULL, &offset, zone, sizeof(zone)), "null seconds out");
    check(!Clock_ParseFrame(buf, len, &seconds, NULL, zone, sizeof(zone)), "null offset out");
    check(!Clock_ParseFrame(buf, len, &seconds, &offset, NULL, sizeof(zone)), "null zone out");
    check(!Clock_ParseFrame(buf, len, &seconds, &offset, zone, 2), "zone buffer too small");
    printf("done\n");

    printf("- a rejected frame leaves the outputs untouched: ");
    seconds = 1234;
    offset = 99;
    strcpy(zone, "KEEP");
    len = build(buf, kNow, 0, "UTC");
    buf[0] = 0x00;
    check(!Clock_ParseFrame(buf, len, &seconds, &offset, zone, sizeof(zone)), "rejected");
    check(seconds == 1234, "seconds untouched");
    check(offset == 99, "offset untouched");
    check(strcmp(zone, "KEEP") == 0, "zone untouched");
    printf("ok\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
