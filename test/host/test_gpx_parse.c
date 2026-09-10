/* Host tests for the GPX track-point extractor (src/navigation/GpxParse.c).
 *
 * No SD card has been attached to this project, so as with the UBX parser
 * these tests are the only verification this code has. GPX files come from
 * arbitrary tools -- Garmin, Strava, komoot, hand-edited exports -- so the
 * attribute-order, quoting and whitespace variations below are not
 * hypothetical. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "GpxParse.h"

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

#define MAX_POINTS 64
static double lats[MAX_POINTS];
static double lons[MAX_POINTS];

/* Streams a whole document; returns how many points came out. */
static int feed_document(const char *xml) {
    GpxParser_t parser;
    size_t i;
    int count = 0;
    double lat;
    double lon;

    Gpx_Init(&parser);
    for (i = 0; i < strlen(xml); i++) {
        if (Gpx_Feed(&parser, xml[i], &lat, &lon)) {
            if (count < MAX_POINTS) {
                lats[count] = lat;
                lons[count] = lon;
            }
            count++;
        }
    }
    return count;
}

int main(void) {
    printf("- a normal track yields its points in order: ");
    {
        const char *gpx =
            "<?xml version=\"1.0\"?>\n"
            "<gpx version=\"1.1\" creator=\"Test\">\n"
            "  <trk><name>Ride</name><trkseg>\n"
            "    <trkpt lat=\"38.8821\" lon=\"-77.0194\"><ele>105.2</ele></trkpt>\n"
            "    <trkpt lat=\"38.8830\" lon=\"-77.0180\"><ele>106.0</ele></trkpt>\n"
            "    <trkpt lat=\"38.8840\" lon=\"-77.0170\"/>\n"
            "  </trkseg></trk>\n"
            "</gpx>\n";
        check(feed_document(gpx) == 3, "three points");
        check_close(lats[0], 38.8821, 1e-9, "first latitude");
        check_close(lons[0], -77.0194, 1e-9, "first longitude");
        check_close(lats[2], 38.8840, 1e-9, "self-closing tag still counts");
    }
    printf("done\n");

    printf("- attribute order, quoting and whitespace vary between exporters: ");
    check(feed_document("<trkpt lon=\"-77.0\" lat=\"38.8\"/>") == 1, "lon before lat");
    check_close(lats[0], 38.8, 1e-9, "and still reads the right one");
    check_close(lons[0], -77.0, 1e-9, "and the other right one");

    check(feed_document("<trkpt lat='38.8' lon='-77.0'/>") == 1, "single quotes");
    check(feed_document("<trkpt   lat = \"38.8\"   lon = \"-77.0\"  />") == 1, "loose spacing");
    check(feed_document("<trkpt\n  lat=\"38.8\"\n  lon=\"-77.0\"\n/>") == 1, "newlines in tag");
    check(feed_document("<rtept lat=\"38.8\" lon=\"-77.0\"/>") == 1, "route points too");
    printf("done\n");

    printf("- other elements are ignored: ");
    {
        const char *gpx =
            "<gpx><metadata><name>Not a point</name>"
            "<bounds minlat=\"38.0\" minlon=\"-78.0\" maxlat=\"39.0\" maxlon=\"-77.0\"/>"
            "</metadata>"
            "<wpt lat=\"1.0\" lon=\"2.0\"><name>Waypoint</name></wpt>"
            "<trk><trkseg><trkpt lat=\"38.8\" lon=\"-77.0\"/></trkseg></trk></gpx>";
        /* <bounds> carries minlat/maxlat, and <wpt> is a standalone waypoint
           rather than part of the trail -- neither should be mistaken for a
           track point. */
        check(feed_document(gpx) == 1, "only the trkpt is emitted");
        check_close(lats[0], 38.8, 1e-9, "and it is the right one");
    }
    printf("done\n");

    printf("- element names must match exactly: ");
    check(feed_document("<TRKPT lat=\"38.8\" lon=\"-77.0\"/>") == 0, "uppercase rejected");
    check(feed_document("<trkptx lat=\"38.8\" lon=\"-77.0\"/>") == 0, "trkptx is not trkpt");
    check(feed_document("<mytrkpt lat=\"38.8\" lon=\"-77.0\"/>") == 0, "suffix match rejected");
    printf("done\n");

    printf("- attribute names must be whole words: ");
    /* "translat" ends in "lat"; matching inside it would read the wrong
       number, and a plausible-looking one at that. */
    check(feed_document("<trkpt translat=\"9.9\" lat=\"38.8\" lon=\"-77.0\"/>") == 1,
          "decoy attribute does not confuse it");
    check_close(lats[0], 38.8, 1e-9, "real lat used, not the decoy");
    printf("done\n");

    printf("- malformed points are dropped, not half-read: ");
    check(feed_document("<trkpt lat=\"38.8\"/>") == 0, "missing lon");
    check(feed_document("<trkpt lon=\"-77.0\"/>") == 0, "missing lat");
    check(feed_document("<trkpt lat=\"\" lon=\"-77.0\"/>") == 0, "empty value");
    check(feed_document("<trkpt lat=\"north\" lon=\"-77.0\"/>") == 0, "non-numeric value");
    check(feed_document("<trkpt lat=\"38.8 lon=\"-77.0\"/>") == 0, "unterminated quote");
    printf("done\n");

    printf("- coordinates outside the valid range are corruption: ");
    check(feed_document("<trkpt lat=\"91.0\" lon=\"0.0\"/>") == 0, "latitude past the pole");
    check(feed_document("<trkpt lat=\"-91.0\" lon=\"0.0\"/>") == 0, "latitude past the other pole");
    check(feed_document("<trkpt lat=\"0.0\" lon=\"181.0\"/>") == 0, "longitude past the date line");
    check(feed_document("<trkpt lat=\"90.0\" lon=\"180.0\"/>") == 1, "the limits themselves are ok");
    check(feed_document("<trkpt lat=\"0.0\" lon=\"0.0\"/>") == 1, "null island is a real place");
    printf("done\n");

    printf("- a huge tag cannot truncate into a half coordinate: ");
    {
        char big[GPX_TAG_BUFFER + 128];
        int n;
        /* A long styling element, then a real point after it. The long tag
           must be skipped whole, and the parser must still find the point. */
        n = snprintf(big, sizeof(big), "<extensions ");
        while (n < GPX_TAG_BUFFER + 40) {
            n += snprintf(big + n, sizeof(big) - (size_t)n, "pad=\"x\" ");
        }
        snprintf(big + n, sizeof(big) - (size_t)n, "lat=\"9.9\" lon=\"9.9\"/>");
        check(feed_document(big) == 0, "oversized tag yields nothing");
    }
    {
        char doc[GPX_TAG_BUFFER + 256];
        int n = snprintf(doc, sizeof(doc), "<extensions ");
        while (n < GPX_TAG_BUFFER + 40) {
            n += snprintf(doc + n, sizeof(doc) - (size_t)n, "pad=\"x\" ");
        }
        snprintf(doc + n, sizeof(doc) - (size_t)n, "/><trkpt lat=\"38.8\" lon=\"-77.0\"/>");
        check(feed_document(doc) == 1, "and the parser recovers for the next point");
        check_close(lats[0], 38.8, 1e-9, "recovered point is correct");
    }
    printf("done\n");

    printf("- an unclosed tag does not swallow the next point: ");
    check(feed_document("<trkpt lat=\"1.0\" lon=\"2.0\" <trkpt lat=\"38.8\" lon=\"-77.0\"/>") == 1,
          "restarts at the second '<'");
    check_close(lats[0], 38.8, 1e-9, "and emits the well-formed one");
    printf("done\n");

    printf("- a truncated file simply ends: ");
    check(feed_document("<gpx><trk><trkseg><trkpt lat=\"38.8\" lon=\"-7") == 0,
          "cut mid-attribute yields nothing");
    printf("done\n");

    printf("- null arguments: ");
    {
        GpxParser_t parser;
        double lat;
        double lon;
        Gpx_Init(NULL);
        Gpx_Init(&parser);
        check(!Gpx_Feed(NULL, '<', &lat, &lon), "null parser");
        check(!Gpx_Feed(&parser, '<', NULL, &lon), "null lat out");
        check(!Gpx_ParseTagAttributes(NULL, &lat, &lon), "null tag");
    }
    printf("done\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
