/* Host-side tests for the route download protocol (src/navigation/RouteParse.c)
 * and the route-following geometry (src/navigation/RouteFollow.c).
 *
 * These two files are the whole offline half of turn-by-turn, and neither can
 * be exercised on the bench without a GPS module the project has never had.
 * That makes this suite the only thing standing behind the feature, so it
 * covers the rejection cases and the awkward geometry, not just the happy
 * path.
 *
 * The geometry cases are the ones worth reading: an out-and-back and a
 * self-crossing loop are exactly where "nearest maneuver" navigation gets it
 * wrong, and they are why the implementation snaps to the line and measures
 * along it instead. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "RouteFollow.h"
#include "RouteParse.h"

static int checks = 0;
static int failures = 0;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_near(double got, double want, double tol, const char *what) {
    checks++;
    if (!(fabs(got - want) <= tol)) {
        failures++;
        printf("  FAIL: %s (got %.3f, want %.3f +/- %.3f)\n", what, got, want, tol);
    }
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* Builds a manifest payload. chunk_count is passed in rather than computed so
 * the tests can deliberately supply a wrong one. */
static void build_manifest(uint8_t *p, uint32_t route_id, uint16_t points,
                           uint16_t maneuvers, uint16_t chunk_count, uint32_t length_m) {
    memset(p, 0, ROUTE_MANIFEST_LEN);
    put_u32(p + 0, route_id);
    put_u16(p + 4, points);
    put_u16(p + 6, maneuvers);
    put_u16(p + 8, chunk_count);
    put_u32(p + 12, length_m);
}

static void put_point(uint8_t *blob, uint16_t index, int32_t lat, int32_t lon) {
    uint8_t *p = blob + (size_t)index * ROUTE_POINT_SIZE;
    put_u32(p + 0, (uint32_t)lat);
    put_u32(p + 4, (uint32_t)lon);
}

static void put_maneuver(uint8_t *blob, uint16_t point_count, uint16_t index,
                         uint8_t icon, uint8_t exit, uint32_t along, const char *name) {
    uint8_t *p = blob + (size_t)point_count * ROUTE_POINT_SIZE +
                 (size_t)index * ROUTE_MANEUVER_SIZE;
    memset(p, 0, ROUTE_MANEUVER_SIZE);
    p[0] = icon;
    p[1] = exit;
    put_u32(p + 4, along);
    if (name) {
        size_t n = strlen(name);
        if (n > ROUTE_STREET_NAME_LEN) {
            n = ROUTE_STREET_NAME_LEN;
        }
        memcpy(p + 8, name, n);
    }
}

/* ---- chunk header ---- */
static void test_chunk_header(void) {
    uint8_t buf[ROUTE_CHUNK_HEADER_LEN + 8];
    uint16_t index = 0xFFFF;
    const uint8_t *payload = NULL;
    uint16_t payload_len = 0xFFFF;

    buf[0] = ROUTE_CHUNK_MAGIC;
    buf[1] = ROUTE_CHUNK_VERSION;
    put_u16(buf + 2, 7);
    put_u16(buf + 4, 8);
    memset(buf + ROUTE_CHUNK_HEADER_LEN, 0xAB, 8);

    check(Route_ParseChunkHeader(buf, sizeof(buf), &index, &payload, &payload_len),
          "well-formed chunk header accepted");
    check(index == 7, "chunk index decoded");
    check(payload_len == 8, "payload length decoded");
    check(payload == buf + ROUTE_CHUNK_HEADER_LEN, "payload points past the header");

    buf[0] = 0x53;
    check(!Route_ParseChunkHeader(buf, sizeof(buf), &index, &payload, &payload_len),
          "wrong magic rejected");
    buf[0] = ROUTE_CHUNK_MAGIC;

    buf[1] = 0x02;
    check(!Route_ParseChunkHeader(buf, sizeof(buf), &index, &payload, &payload_len),
          "wrong version rejected");
    buf[1] = ROUTE_CHUNK_VERSION;

    /* The check that matters: a truncated BLE write must not be mistaken for a
     * complete one, or the missing bytes are assembled as stale buffer. */
    check(!Route_ParseChunkHeader(buf, sizeof(buf) - 1, &index, &payload, &payload_len),
          "declared length longer than the data rejected");
    check(!Route_ParseChunkHeader(buf, sizeof(buf) + 1, &index, &payload, &payload_len),
          "declared length shorter than the data rejected");
    check(!Route_ParseChunkHeader(buf, 3, &index, &payload, &payload_len),
          "runt shorter than a header rejected");
}

/* ---- manifest ---- */
static void test_manifest(void) {
    uint8_t p[ROUTE_MANIFEST_LEN];
    RouteManifest_t m;

    /* 10 points + 2 maneuvers = 80 + 80 = 160 bytes, one payload chunk. */
    build_manifest(p, 0xDEADBEEF, 10, 2, 2, 1234);
    check(Route_ParseManifest(p, sizeof(p), &m), "consistent manifest accepted");
    check(m.route_id == 0xDEADBEEF, "route id decoded");
    check(m.point_count == 10, "point count decoded");
    check(m.maneuver_count == 2, "maneuver count decoded");
    check(m.total_length_m == 1234, "route length decoded");
    check(Route_BlobSize(&m) == 160, "blob size from the counts");

    build_manifest(p, 1, 10, 2, 3, 0);
    check(!Route_ParseManifest(p, sizeof(p), &m),
          "chunk count disagreeing with the sizes rejected");

    build_manifest(p, 1, 1, 0, 1, 0);
    check(!Route_ParseManifest(p, sizeof(p), &m), "route with under two points rejected");

    build_manifest(p, 1, ROUTE_MAX_POINTS + 1, 0, 2, 0);
    check(!Route_ParseManifest(p, sizeof(p), &m), "point count over the ceiling rejected");

    build_manifest(p, 1, 10, ROUTE_MAX_MANEUVERS + 1, 2, 0);
    check(!Route_ParseManifest(p, sizeof(p), &m), "maneuver count over the ceiling rejected");

    build_manifest(p, 1, 10, 2, 2, 0);
    check(!Route_ParseManifest(p, ROUTE_MANIFEST_LEN - 1, &m), "short manifest rejected");

    /* Exact multiple of the payload size must not claim a spare empty chunk:
     * 180 bytes is one chunk, not two. 22 points + 1 maneuver = 176+... use
     * numbers that land exactly. 20 points = 160, plus 0 maneuvers... 180
     * needs 22.5 points, so use maneuvers: 10 points (80) + 5 maneuvers (200)
     * = 280 -> 2 payload chunks. Exactness is checked below instead. */
    build_manifest(p, 1, 10, 5, 3, 0);
    check(Route_ParseManifest(p, sizeof(p), &m), "two-payload-chunk manifest accepted");
    check(Route_BlobSize(&m) == 280, "blob size 280");
    check(Route_ChunkCount(&m) == 3, "280 bytes needs two payload chunks plus manifest");
}

/* A blob whose size is an exact multiple of the chunk payload must not be
 * given a trailing empty chunk -- the sender would never write it and the
 * receiver would wait for it forever. */
static void test_exact_multiple(void) {
    RouteManifest_t m;
    memset(&m, 0, sizeof(m));
    /* 45 points * 8 = 360 = exactly two chunks of 180. */
    m.point_count = 45;
    m.maneuver_count = 0;
    check(Route_BlobSize(&m) == 360, "360-byte blob");
    check(Route_ChunkCount(&m) == 3, "exact multiple gets no trailing empty chunk");
}

/* ---- records ---- */
static void test_records(void) {
    enum { POINTS = 4, MANEUVERS = 2 };
    static uint8_t blob[POINTS * ROUTE_POINT_SIZE + MANEUVERS * ROUTE_MANEUVER_SIZE];
    RouteManifest_t m;
    memset(&m, 0, sizeof(m));
    m.point_count = POINTS;
    m.maneuver_count = MANEUVERS;

    /* Negative coordinates on purpose: Germantown is west of Greenwich, so the
     * sign bit is set on every longitude this project will ever see. */
    put_point(blob, 0, 391694000, -773000000);
    put_point(blob, 1, 391700000, -772990000);
    put_point(blob, 2, 391710000, -772980000);
    put_point(blob, 3, 391720000, -772970000);

    put_maneuver(blob, POINTS, 0, 3, 0, 120, "Richter Farm Rd");
    /* Exactly 32 bytes, filling the field with no room for a terminator. */
    put_maneuver(blob, POINTS, 1, 9, 2, 800, "AbcdefghijAbcdefghijAbcdefghijAb");

    RoutePoint_t pt;
    check(Route_Point(blob, &m, 0, &pt), "first point read");
    check(pt.lat == 391694000, "latitude decoded");
    check(pt.lon == -773000000, "negative longitude decoded");
    check(Route_Point(blob, &m, 3, &pt), "last point read");
    check(pt.lat == 391720000, "last latitude decoded");
    check(!Route_Point(blob, &m, 4, &pt), "point past the end rejected");

    RouteManeuver_t mv;
    check(Route_Maneuver(blob, &m, 0, &mv), "first maneuver read");
    check(mv.icon_id == 3, "icon decoded");
    check(mv.distance_along_route_m == 120, "distance along decoded");
    check(strcmp(mv.street_name, "Richter Farm Rd") == 0, "street name decoded");

    check(Route_Maneuver(blob, &m, 1, &mv), "second maneuver read");
    check(mv.exit_number == 2, "roundabout exit decoded");
    check(strlen(mv.street_name) == ROUTE_STREET_NAME_LEN,
          "a name filling the field is still terminated");
    check(!Route_Maneuver(blob, &m, 2, &mv), "maneuver past the end rejected");
}

/* ---- geometry ---- */

/* A straight run due east, so the expected distances are checkable by hand. */
static void test_snap_straight(void) {
    enum { POINTS = 3 };
    static uint8_t blob[POINTS * ROUTE_POINT_SIZE];
    static uint32_t cum[POINTS];
    RouteManifest_t m;
    memset(&m, 0, sizeof(m));
    m.point_count = POINTS;

    /* 39.0N, three points 0.001 degrees of longitude apart. */
    put_point(blob, 0, 390000000, -770000000);
    put_point(blob, 1, 390000000, -769990000);
    put_point(blob, 2, 390000000, -769980000);

    const uint32_t total = RouteFollow_BuildCumulative(blob, &m, cum);
    const double leg = RouteFollow_MetresBetween(390000000, -770000000, 390000000, -769990000);
    check(cum[0] == 0, "cumulative starts at zero");
    check_near((double)cum[1], leg, 1.0, "first leg length");
    check_near((double)total, 2.0 * leg, 1.0, "total is both legs");

    RouteFix_t fix;
    /* Sitting exactly on the middle point. */
    check(RouteFollow_Snap(blob, &m, cum, 39.0, -76.999, &fix), "snap on the line");
    check(fix.cross_track_m == 0, "on the line means no cross-track");
    check_near((double)fix.distance_along_m, leg, 2.0, "distance along at the middle point");
    check(!fix.off_route, "on the line is not off-route");

    /* Offset north by about 20 metres: still on-route, same distance along. */
    check(RouteFollow_Snap(blob, &m, cum, 39.00018, -76.999, &fix), "snap offset from the line");
    check_near((double)fix.cross_track_m, 20.0, 3.0, "cross-track of a 20m offset");
    check(!fix.off_route, "20m off the line is still on-route");
    check_near((double)fix.distance_along_m, leg, 3.0, "offset does not move distance along");

    /* Far enough off to be declared off-route. */
    check(RouteFollow_Snap(blob, &m, cum, 39.0018, -76.999, &fix), "snap well off the line");
    check(fix.off_route, "200m off the line is off-route");

    /* Before the start: clamps to the first point rather than extrapolating
     * backwards onto the infinite line. */
    check(RouteFollow_Snap(blob, &m, cum, 39.0, -77.001, &fix), "snap before the start");
    check(fix.distance_along_m == 0, "before the start clamps to zero");

    /* Past the end: clamps to the total. */
    check(RouteFollow_Snap(blob, &m, cum, 39.0, -76.997, &fix), "snap past the end");
    check_near((double)fix.distance_along_m, (double)total, 2.0, "past the end clamps to total");
}

/* The case that kills "nearest maneuver in a straight line".
 *
 * An out-and-back: east along a line, then back west a few metres to the
 * north. A rider on the outbound leg is metres from the inbound leg, and a
 * naive nearest-point search would happily report the turn at the far end of
 * the ride. Snapping along the line gives two distinct distances for the same
 * patch of ground. */
static void test_snap_out_and_back(void) {
    enum { POINTS = 4 };
    static uint8_t blob[POINTS * ROUTE_POINT_SIZE + 2 * ROUTE_MANEUVER_SIZE];
    static uint32_t cum[POINTS];
    RouteManifest_t m;
    memset(&m, 0, sizeof(m));
    m.point_count = POINTS;
    m.maneuver_count = 2;

    /* Out east along 39.0, u-turn, back west along 39.00009 (about 10m north). */
    put_point(blob, 0, 390000000, -770000000);
    put_point(blob, 1, 390000000, -769980000);
    put_point(blob, 2, 390000900, -769980000);
    put_point(blob, 3, 390000900, -770000000);

    const uint32_t total = RouteFollow_BuildCumulative(blob, &m, cum);
    check(total > 300, "out-and-back has real length");

    RouteFix_t out_leg;
    RouteFix_t back_leg;
    /* Two fixes about 10m apart across the two legs, near the west end. */
    check(RouteFollow_Snap(blob, &m, cum, 39.0, -76.9999, &out_leg), "snap on the outbound leg");
    check(RouteFollow_Snap(blob, &m, cum, 39.00009, -76.9999, &back_leg), "snap on the return leg");

    check(out_leg.segment_index == 0, "outbound fix lands on the first segment");
    check(back_leg.segment_index == 2, "return fix lands on the third segment");
    /* The whole point: metres apart on the ground, hundreds of metres apart
     * along the route. */
    check(back_leg.distance_along_m > out_leg.distance_along_m + 200,
          "same place on the ground, far apart along the route");

    /* And the maneuver each one gets is different. */
    put_maneuver(blob, POINTS, 0, 8, 0, cum[1], "U-turn");
    put_maneuver(blob, POINTS, 1, 10, 0, total, "Arrive");

    RouteManeuver_t mv;
    uint32_t to = 0;
    check(RouteFollow_NextManeuver(blob, &m, out_leg.distance_along_m, &mv, &to),
          "outbound leg has a next maneuver");
    check(mv.icon_id == 8, "outbound leg is heading for the u-turn");

    check(RouteFollow_NextManeuver(blob, &m, back_leg.distance_along_m, &mv, &to),
          "return leg has a next maneuver");
    check(mv.icon_id == 10, "return leg is heading for the arrival");
}

static void test_next_maneuver(void) {
    enum { POINTS = 2, MANEUVERS = 3 };
    static uint8_t blob[POINTS * ROUTE_POINT_SIZE + MANEUVERS * ROUTE_MANEUVER_SIZE];
    RouteManifest_t m;
    memset(&m, 0, sizeof(m));
    m.point_count = POINTS;
    m.maneuver_count = MANEUVERS;

    put_point(blob, 0, 390000000, -770000000);
    put_point(blob, 1, 390000000, -769900000);
    put_maneuver(blob, POINTS, 0, 2, 0, 100, "First St");
    put_maneuver(blob, POINTS, 1, 3, 0, 500, "Second St");
    put_maneuver(blob, POINTS, 2, 10, 0, 900, "Destination");

    RouteManeuver_t mv;
    uint32_t to = 0;

    check(RouteFollow_NextManeuver(blob, &m, 0, &mv, &to), "at the start there is a maneuver");
    check(strcmp(mv.street_name, "First St") == 0, "first maneuver chosen at the start");
    check(to == 100, "distance to the first maneuver");

    /* Standing exactly on a maneuver still reports it, rather than skipping to
     * the next -- the rider has not made the turn yet. */
    check(RouteFollow_NextManeuver(blob, &m, 100, &mv, &to), "standing on a maneuver");
    check(strcmp(mv.street_name, "First St") == 0, "a maneuver at zero distance is still next");
    check(to == 0, "zero distance to a maneuver underfoot");

    check(RouteFollow_NextManeuver(blob, &m, 101, &mv, &to), "past the first maneuver");
    check(strcmp(mv.street_name, "Second St") == 0, "second maneuver chosen after the first");
    check(to == 399, "distance to the second maneuver");

    check(RouteFollow_NextManeuver(blob, &m, 900, &mv, &to), "at the destination");
    check(mv.icon_id == 10, "arrival is the last maneuver");

    check(!RouteFollow_NextManeuver(blob, &m, 901, &mv, &to),
          "past the last maneuver there is nothing left");
}

int main(void) {
    test_chunk_header();
    test_manifest();
    test_exact_multiple();
    test_records();
    test_snap_straight();
    test_snap_out_and_back();
    test_next_maneuver();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
