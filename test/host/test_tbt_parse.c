/* Host-side tests for the turn-by-turn frame parser (src/navigation/TbtParse.c).
 *
 * The parser is the one part of BLE TBT reception that can be exercised off
 * target: everything else is NimBLE callbacks. It also handles data straight
 * off the air from another device, so the rejection cases matter as much as
 * the happy path. */
#include <stdio.h>
#include <string.h>

#include "TbtParse.h"

static int checks = 0;
static int failures = 0;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* Builds a well-formed frame into buf, returns its length. */
static size_t build(uint8_t *buf, uint8_t icon, uint32_t distance, const char *name) {
    size_t name_len = name ? strlen(name) : 0;
    buf[0] = TBT_FRAME_MAGIC;
    buf[1] = TBT_FRAME_VERSION;
    buf[2] = icon;
    buf[3] = (uint8_t)name_len;
    buf[4] = (uint8_t)(distance & 0xFF);
    buf[5] = (uint8_t)((distance >> 8) & 0xFF);
    buf[6] = (uint8_t)((distance >> 16) & 0xFF);
    buf[7] = (uint8_t)((distance >> 24) & 0xFF);
    if (name_len) {
        memcpy(buf + TBT_FRAME_HEADER_LEN, name, name_len);
    }
    return TBT_FRAME_HEADER_LEN + name_len;
}

int main(void) {
    uint8_t buf[64];
    uint8_t icon;
    uint32_t distance;
    char street[TBT_STREET_NAME_LEN + 1];
    size_t len;

    printf("- well-formed frame round-trips: ");
    len = build(buf, 3 /* turn right */, 250, "Hongqiao Road");
    check(TBT_ParseFrame(buf, len, &icon, &distance, street, sizeof(street)), "accepts frame");
    check(icon == 3, "icon survives");
    check(distance == 250, "distance survives");
    check(strcmp(street, "Hongqiao Road") == 0, "street survives");
    printf("done\n");

    printf("- distance endianness and full range: ");
    len = build(buf, 1, 0xDEADBEEF, "");
    check(TBT_ParseFrame(buf, len, &icon, &distance, street, sizeof(street)), "accepts");
    check(distance == 0xDEADBEEF, "little-endian uint32 decoded");
    len = build(buf, 1, 0, "");
    check(TBT_ParseFrame(buf, len, &icon, &distance, street, sizeof(street)), "zero distance ok");
    check(distance == 0, "zero decoded");
    printf("done\n");

    printf("- empty street name yields empty string: ");
    len = build(buf, 10, 0, "");
    check(TBT_ParseFrame(buf, len, &icon, &distance, street, sizeof(street)), "accepts");
    check(street[0] == '\0', "street is empty, not garbage");
    printf("done\n");

    printf("- maximum length street name: ");
    {
        char longest[TBT_STREET_NAME_LEN + 1];
        memset(longest, 'A', TBT_STREET_NAME_LEN);
        longest[TBT_STREET_NAME_LEN] = '\0';
        len = build(buf, 2, 100, longest);
        check(TBT_ParseFrame(buf, len, &icon, &distance, street, sizeof(street)), "accepts 31 chars");
        check(strlen(street) == TBT_STREET_NAME_LEN, "all 31 chars kept");
        check(street[TBT_STREET_NAME_LEN] == '\0', "NUL terminated");
    }
    printf("done\n");

    printf("- every valid icon id accepted, one past rejected: ");
    for (uint8_t i = 0; i <= TBT_ICON_MAX_ID; i++) {
        len = build(buf, i, 10, "x");
        check(TBT_ParseFrame(buf, len, &icon, &distance, street, sizeof(street)), "valid icon");
    }
    len = build(buf, TBT_ICON_MAX_ID + 1, 10, "x");
    check(!TBT_ParseFrame(buf, len, &icon, &distance, street, sizeof(street)), "icon out of range");
    printf("done\n");

    printf("- malformed frames rejected: ");
    len = build(buf, 1, 10, "x");
    buf[0] = 0x55;
    check(!TBT_ParseFrame(buf, len, &icon, &distance, street, sizeof(street)), "bad magic");

    len = build(buf, 1, 10, "x");
    buf[1] = 0x02;
    check(!TBT_ParseFrame(buf, len, &icon, &distance, street, sizeof(street)), "bad version");

    len = build(buf, 1, 10, "Main St");
    check(!TBT_ParseFrame(buf, len - 1, &icon, &distance, street, sizeof(street)),
          "truncated payload");
    check(!TBT_ParseFrame(buf, len + 1, &icon, &distance, street, sizeof(street)),
          "trailing junk");

    len = build(buf, 1, 10, "x");
    buf[3] = 40; /* claims more name than the frame can hold */
    check(!TBT_ParseFrame(buf, len, &icon, &distance, street, sizeof(street)), "name_len too large");

    for (size_t short_len = 0; short_len < TBT_FRAME_HEADER_LEN; short_len++) {
        build(buf, 1, 10, "");
        check(!TBT_ParseFrame(buf, short_len, &icon, &distance, street, sizeof(street)),
              "shorter than header");
    }

    check(!TBT_ParseFrame(NULL, 8, &icon, &distance, street, sizeof(street)), "null data");
    check(!TBT_ParseFrame(buf, 8, NULL, &distance, street, sizeof(street)), "null icon out");
    check(!TBT_ParseFrame(buf, 8, &icon, NULL, street, sizeof(street)), "null distance out");
    check(!TBT_ParseFrame(buf, 8, &icon, &distance, NULL, sizeof(street)), "null street out");
    check(!TBT_ParseFrame(buf, 8, &icon, &distance, street, 4), "street buffer too small");
    printf("done\n");

    printf("- rejected frames leave outputs untouched: ");
    icon = 99;
    distance = 12345;
    strcpy(street, "untouched");
    len = build(buf, 1, 777, "New Street");
    buf[0] = 0x00; /* corrupt it */
    check(!TBT_ParseFrame(buf, len, &icon, &distance, street, sizeof(street)), "rejects");
    check(icon == 99, "icon not written");
    check(distance == 12345, "distance not written");
    check(strcmp(street, "untouched") == 0, "street not written");
    printf("done\n");

    printf("- unknown-distance sentinel survives decoding: ");
    /* The phone sends this when Maps names a turn without saying how far away
       it is -- "Turn right onto Richter Farm Rd" with no distance anywhere in
       the notification, which on one drive was 38 of 109 maneuvers. It must
       reach the UI intact rather than being rejected as an absurd distance:
       the arrow and the street name are still worth showing. */
    len = build(buf, 3, TBT_DISTANCE_UNKNOWN, "Richter Farm Rd");
    check(TBT_ParseFrame(buf, len, &icon, &distance, street, sizeof(street)), "accepts sentinel");
    check(distance == TBT_DISTANCE_UNKNOWN, "sentinel survives");
    check(icon == 3, "icon still decoded");
    check(strcmp(street, "Richter Farm Rd") == 0, "street still decoded");
    /* One metre below the sentinel is an ordinary distance, not a near-miss to
       be treated as unknown. */
    len = build(buf, 3, TBT_DISTANCE_UNKNOWN - 1, "");
    check(TBT_ParseFrame(buf, len, &icon, &distance, street, sizeof(street)), "accepts");
    check(distance == TBT_DISTANCE_UNKNOWN - 1, "adjacent value is a real distance");
    printf("done\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
