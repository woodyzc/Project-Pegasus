#include "RouteParse.h"

#include <string.h>

// Little-endian readers. Byte at a time rather than a cast, for two reasons
// that both bite here: the blob lives in PSRAM where an unaligned 32-bit load
// faults, and a cast would make these tests pass only on a little-endian host.
static uint16_t ReadU16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t ReadU32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t ReadI32(const uint8_t *p) {
    // Via uint32_t: a direct shift into a signed type is implementation
    // defined once the sign bit is set, and coordinates west of Greenwich or
    // south of the equator set it on every single point.
    return (int32_t)ReadU32(p);
}

bool Route_ParseChunkHeader(const uint8_t *data,
                            size_t length,
                            uint16_t *out_chunk_index,
                            const uint8_t **out_payload,
                            uint16_t *out_payload_len) {
    if (data == NULL || out_chunk_index == NULL || out_payload == NULL ||
        out_payload_len == NULL) {
        return false;
    }
    if (length < ROUTE_CHUNK_HEADER_LEN) {
        return false;
    }
    if (data[0] != ROUTE_CHUNK_MAGIC || data[1] != ROUTE_CHUNK_VERSION) {
        return false;
    }

    const uint16_t index = ReadU16(data + 2);
    const uint16_t payload_len = ReadU16(data + 4);

    // The declared length must match what actually arrived. A short write is
    // otherwise indistinguishable from a full one, and the missing bytes would
    // be stored as whatever the assembly buffer already held.
    if ((size_t)payload_len + ROUTE_CHUNK_HEADER_LEN != length) {
        return false;
    }

    *out_chunk_index = index;
    *out_payload = data + ROUTE_CHUNK_HEADER_LEN;
    *out_payload_len = payload_len;
    return true;
}

bool Route_ParseManifest(const uint8_t *payload, uint16_t payload_len, RouteManifest_t *out) {
    if (payload == NULL || out == NULL || payload_len != ROUTE_MANIFEST_LEN) {
        return false;
    }

    RouteManifest_t m;
    m.route_id = ReadU32(payload + 0);
    m.point_count = ReadU16(payload + 4);
    m.maneuver_count = ReadU16(payload + 6);
    m.chunk_count = ReadU16(payload + 8);
    // payload + 10 is reserved; ignored rather than rejected, so a later
    // version can use it without this build refusing the whole route.
    m.total_length_m = ReadU32(payload + 12);

    if (m.point_count > ROUTE_MAX_POINTS || m.maneuver_count > ROUTE_MAX_MANEUVERS) {
        return false;
    }
    // A route with no line to follow is not a route. Maneuvers may legitimately
    // be zero for a straight shot with only an arrival.
    if (m.point_count < 2) {
        return false;
    }
    // The declared chunk count must be the one the declared sizes imply.
    // Without this a manifest could promise fewer chunks than the blob needs
    // and the transfer would report complete with its tail never sent.
    if (m.chunk_count != Route_ChunkCount(&m)) {
        return false;
    }

    *out = m;
    return true;
}

size_t Route_BlobSize(const RouteManifest_t *manifest) {
    if (manifest == NULL) {
        return 0;
    }
    return (size_t)manifest->point_count * ROUTE_POINT_SIZE +
           (size_t)manifest->maneuver_count * ROUTE_MANEUVER_SIZE;
}

uint16_t Route_ChunkCount(const RouteManifest_t *manifest) {
    const size_t blob = Route_BlobSize(manifest);
    // +1 for the manifest chunk itself, which carries no blob payload.
    const size_t payload_chunks =
        (blob + ROUTE_CHUNK_PAYLOAD - 1) / ROUTE_CHUNK_PAYLOAD;
    return (uint16_t)(payload_chunks + 1);
}

bool Route_Point(const uint8_t *blob, const RouteManifest_t *manifest, uint16_t index,
                 RoutePoint_t *out) {
    if (blob == NULL || manifest == NULL || out == NULL || index >= manifest->point_count) {
        return false;
    }
    const uint8_t *p = blob + (size_t)index * ROUTE_POINT_SIZE;
    out->lat = ReadI32(p + 0);
    out->lon = ReadI32(p + 4);
    return true;
}

bool Route_Maneuver(const uint8_t *blob, const RouteManifest_t *manifest, uint16_t index,
                    RouteManeuver_t *out) {
    if (blob == NULL || manifest == NULL || out == NULL || index >= manifest->maneuver_count) {
        return false;
    }
    // Maneuvers sit after the whole polyline.
    const uint8_t *p = blob + (size_t)manifest->point_count * ROUTE_POINT_SIZE +
                       (size_t)index * ROUTE_MANEUVER_SIZE;

    out->icon_id = p[0];
    out->exit_number = p[1];
    out->distance_along_route_m = ReadU32(p + 4);

    // The wire field is not NUL-guaranteed -- a 32-byte name fills it exactly.
    // Copy and terminate rather than trusting it, because everything
    // downstream treats this as a C string.
    memcpy(out->street_name, p + 8, ROUTE_STREET_NAME_LEN);
    out->street_name[ROUTE_STREET_NAME_LEN] = '\0';
    return true;
}
