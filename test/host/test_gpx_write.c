// Host tests for src/navigation/GpxWrite.c -- the GPX a ride is recorded into.
//
// The last test is the one that matters most: it writes a file with the real
// writer and reads it back with the real parser (GpxParse.c, which already
// loads route files from the card). Writing something no reader accepts is the
// obvious way for this feature to fail, and neither suite alone would catch it.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../src/navigation/GpxParse.h"
#include "../../src/navigation/GpxWrite.h"

static int checks;
static int failures;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("\n    FAIL: %s", what);
    }
}

static void check_close(double got, double want, double tol, const char *what) {
    checks++;
    if (fabs(got - want) > tol) {
        failures++;
        printf("\n    FAIL: %s (got %.7f, want %.7f)", what, got, want);
    }
}

int main(void) {
    char buf[4096];
    char line[GPX_WRITE_MAX_LINE];

    printf("- header carries the name and opens every tag: ");
    check(GpxWrite_Header(buf, sizeof(buf), "Morning ride") > 0, "writes");
    check(strstr(buf, "<?xml") == buf, "starts with the declaration");
    check(strstr(buf, "<gpx ") != NULL, "opens gpx");
    check(strstr(buf, "<trk>") != NULL, "opens trk");
    check(strstr(buf, "<trkseg>") != NULL, "opens trkseg");
    check(strstr(buf, "<name>Morning ride</name>") != NULL, "carries the name");
    printf("done\n");

    printf("- a name with XML metacharacters cannot break the file: ");
    check(GpxWrite_Header(buf, sizeof(buf), "Bob & <Alice>'s \"ride\"") > 0, "writes");
    check(strstr(buf, "&amp;") != NULL, "ampersand escaped");
    check(strstr(buf, "&lt;") != NULL, "less-than escaped");
    check(strstr(buf, "&gt;") != NULL, "greater-than escaped");
    check(strstr(buf, "&quot;") != NULL, "quote escaped");
    check(strstr(buf, "&apos;") != NULL, "apostrophe escaped");
    // The raw characters must be gone from the name, or the XML is malformed.
    check(strstr(buf, "<Alice>") == NULL, "raw tag-like text removed");
    printf("done\n");

    printf("- escaping refuses to half-write when it runs out of room: ");
    char tiny[6];
    check(GpxWrite_XmlEscape(tiny, sizeof(tiny), "&&&&&&") == 0, "reports failure");
    check(tiny[0] == '\0', "leaves nothing behind rather than a split entity");
    printf("done\n");

    printf("- a point carries coordinates, elevation and time: ");
    check(GpxWrite_Point(line, sizeof(line), 38.8894, -77.0352, 17.5,
                         true, 2026, 9, 10, 14, 30, 5) > 0, "writes");
    check(strstr(line, "lat=\"38.8894000\"") != NULL, "latitude at 7 decimals");
    check(strstr(line, "lon=\"-77.0352000\"") != NULL, "longitude at 7 decimals");
    check(strstr(line, "<ele>17.5</ele>") != NULL, "elevation");
    check(strstr(line, "<time>2026-09-10T14:30:05Z</time>") != NULL, "ISO 8601 UTC");
    printf("done\n");

    printf("- a point with no resolved time omits it rather than guessing: ");
    check(GpxWrite_Point(line, sizeof(line), 1.0, 2.0, 3.0, false, 0, 0, 0, 0, 0, 0) > 0, "writes");
    check(strstr(line, "<time>") == NULL, "no time element");
    check(strstr(line, "<trkpt") != NULL, "still a point");
    printf("done\n");

    printf("- a line that will not fit writes nothing at all: ");
    char small[20];
    check(GpxWrite_Point(small, sizeof(small), 38.8894, -77.0352, 17.5,
                         true, 2026, 9, 10, 14, 30, 5) == 0, "reports failure");
    check(small[0] == '\0', "no truncated tag left in the buffer");
    check(GpxWrite_Header(small, sizeof(small), "x") == 0, "header too");
    check(GpxWrite_Footer(small, sizeof(small)) == 0, "footer too");
    printf("done\n");

    printf("- file names are derived from the fix time, or a sequence: ");
    check(GpxWrite_FileName(buf, sizeof(buf), true, 2026, 9, 10, 14, 30, 5, 0) > 0, "writes");
    check(strcmp(buf, "/rides/2026-09-10_143005.gpx") == 0, "timestamped name");
    check(GpxWrite_FileName(buf, sizeof(buf), false, 0, 0, 0, 0, 0, 0, 7) > 0, "writes");
    check(strcmp(buf, "/rides/ride-0007.gpx") == 0, "sequence fallback");
    printf("done\n");

    printf("- what this writes, GpxParse reads back: ");
    {
        // Coordinates chosen to exercise both hemispheres and the 7th decimal.
        const double lats[] = { 38.8894000, -33.8567800, 51.5007300, 0.0000001 };
        const double lons[] = { -77.0352000, 151.2152800, -0.1246200, -0.0000001 };
        const size_t count = sizeof(lats) / sizeof(lats[0]);

        size_t used = 0;
        used += GpxWrite_Header(buf + used, sizeof(buf) - used, "Round trip");
        for (size_t i = 0; i < count; i++) {
            used += GpxWrite_Point(buf + used, sizeof(buf) - used, lats[i], lons[i],
                                   (float)(10 + i), true, 2026, 9, 10, 14, 30, (uint8_t)i);
        }
        used += GpxWrite_Footer(buf + used, sizeof(buf) - used);
        check(used > 0 && used < sizeof(buf), "document built");

        GpxParser_t parser;
        Gpx_Init(&parser);

        double lat;
        double lon;
        size_t found = 0;
        for (size_t i = 0; i < used; i++) {
            if (Gpx_Feed(&parser, buf[i], &lat, &lon)) {
                if (found < count) {
                    check_close(lat, lats[found], 1e-6, "latitude survives the round trip");
                    check_close(lon, lons[found], 1e-6, "longitude survives the round trip");
                }
                found++;
            }
        }
        check(found == count, "every point written was read back");
    }
    printf("done\n");

    printf("- the footer closes what the header opened: ");
    check(GpxWrite_Footer(buf, sizeof(buf)) > 0, "writes");
    check(strstr(buf, "</trkseg>") != NULL, "closes trkseg");
    check(strstr(buf, "</trk>") != NULL, "closes trk");
    check(strstr(buf, "</gpx>") != NULL, "closes gpx");
    printf("done\n");

    printf("- null and zero-size arguments are refused: ");
    check(GpxWrite_Header(NULL, 10, "x") == 0, "null header out");
    check(GpxWrite_Point(NULL, 10, 1, 2, 3, false, 0, 0, 0, 0, 0, 0) == 0, "null point out");
    check(GpxWrite_Footer(NULL, 10) == 0, "null footer out");
    check(GpxWrite_FileName(NULL, 10, false, 0, 0, 0, 0, 0, 0, 1) == 0, "null name out");
    check(GpxWrite_Header(buf, 0, "x") == 0, "zero size");
    check(GpxWrite_XmlEscape(buf, sizeof(buf), NULL) == 0, "null input escapes to nothing");
    printf("done\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
