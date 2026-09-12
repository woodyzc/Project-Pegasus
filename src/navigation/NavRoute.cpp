#include "NavRoute.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace {

// The assembled blob and the cumulative distance table both live in PSRAM:
// a 4,000-point route is 32KB of polyline and another 16KB of table, which is
// a large fraction of internal RAM and none of it is touched from an ISR.
uint8_t *s_blob = nullptr;
uint32_t *s_cum = nullptr;

// Which chunks have landed. One bit each, in internal RAM because it is read
// and written on every single chunk and it is tiny.
uint8_t *s_received = nullptr;
size_t s_received_bytes = 0;
uint16_t s_received_count = 0;

RouteManifest_t s_manifest;
bool s_have_manifest = false;
bool s_loaded = false;
uint32_t s_length_m = 0;

RouteFix_t s_last_fix;
bool s_have_fix = false;

// Last time a live directive arrived from the phone. The fallback waits for
// this to go stale rather than watching the connection state -- see the header.
uint32_t s_last_live_ms = 0;
bool s_ever_live = false;

// What we last published, so an unchanged directive is not republished at 1Hz.
// DataCenter wakes subscribers on publish, and a dashboard relayout every
// second for an unchanged turn is wasted frame time.
uint8_t s_last_icon = 0xFF;
uint32_t s_last_distance = 0xFFFFFFFFu;
uint32_t s_last_publish_ms = 0;

// Republish an unchanged directive at least this often.
//
// Page_Dashboard drops a turn it has not heard about for 30 seconds, so
// silence is not neutral -- it actively blanks the panel. The phone path
// already solves this with a 10s keepalive (KEEPALIVE_INTERVAL_MS in
// BleLink.kt); the onboard path needs the same, and for the same reason. A
// rider stopped at a light has a turn and a distance that do not change, and
// that is precisely when they are looking at it.
#define NAVROUTE_KEEPALIVE_MS 10000

void FreeAll() {
    if (s_blob != nullptr) {
        heap_caps_free(s_blob);
        s_blob = nullptr;
    }
    if (s_cum != nullptr) {
        heap_caps_free(s_cum);
        s_cum = nullptr;
    }
    if (s_received != nullptr) {
        heap_caps_free(s_received);
        s_received = nullptr;
    }
    s_received_bytes = 0;
    s_received_count = 0;
    s_have_manifest = false;
    s_loaded = false;
    s_length_m = 0;
    s_have_fix = false;
    s_last_icon = 0xFF;
    s_last_distance = 0xFFFFFFFFu;
}

bool ChunkSeen(uint16_t index) {
    if (s_received == nullptr) {
        return false;
    }
    return (s_received[index >> 3] & (uint8_t)(1u << (index & 7))) != 0;
}

void MarkChunk(uint16_t index) {
    if (s_received == nullptr || ChunkSeen(index)) {
        return;
    }
    s_received[index >> 3] |= (uint8_t)(1u << (index & 7));
    s_received_count++;
}

// A manifest starts a transfer. Allocating here rather than on the first
// payload chunk means an out-of-order arrival -- which BLE permits and Android
// does in practice under load -- is stored rather than dropped.
bool BeginTransfer(const RouteManifest_t &m) {
    FreeAll();

    const size_t blob_size = Route_BlobSize(&m);
    s_blob = (uint8_t *)heap_caps_calloc(blob_size, 1, MALLOC_CAP_SPIRAM);
    if (s_blob == nullptr) {
        return false;
    }
    s_cum = (uint32_t *)heap_caps_calloc(m.point_count, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (s_cum == nullptr) {
        FreeAll();
        return false;
    }

    s_received_bytes = ((size_t)m.chunk_count + 7) / 8;
    s_received = (uint8_t *)heap_caps_calloc(s_received_bytes, 1, MALLOC_CAP_INTERNAL);
    if (s_received == nullptr) {
        FreeAll();
        return false;
    }

    s_manifest = m;
    s_have_manifest = true;
    MarkChunk(0); // the manifest is chunk 0 and has now arrived
    return true;
}

// Called once the last chunk lands. Builds the cumulative table so every
// subsequent fix is a snap rather than a re-measure of the whole polyline.
void FinishTransfer() {
    s_length_m = RouteFollow_BuildCumulative(s_blob, &s_manifest, s_cum);
    // A route whose points are all identical measures zero and cannot be
    // navigated -- treat it as a failed transfer rather than dividing by it
    // later.
    s_loaded = s_length_m > 0;
}

} // namespace

void NavRoute_Clear() {
    FreeAll();
}

bool NavRoute_AcceptChunk(const uint8_t *data, size_t length) {
    uint16_t index = 0;
    const uint8_t *payload = nullptr;
    uint16_t payload_len = 0;

    if (!Route_ParseChunkHeader(data, length, &index, &payload, &payload_len)) {
        return false;
    }

    if (index == 0) {
        RouteManifest_t m;
        if (!Route_ParseManifest(payload, payload_len, &m)) {
            return false;
        }
        // Re-sending the manifest for the transfer already in progress is a
        // retry, not a restart. Treating it as a restart would discard every
        // chunk received so far and make a flaky link never converge.
        if (s_have_manifest && m.route_id == s_manifest.route_id &&
            m.point_count == s_manifest.point_count &&
            m.maneuver_count == s_manifest.maneuver_count) {
            return true;
        }
        return BeginTransfer(m);
    }

    if (!s_have_manifest || index >= s_manifest.chunk_count) {
        // A payload chunk with no manifest cannot be placed: its offset comes
        // from the manifest's geometry. Dropped, and the phone's retry after
        // the manifest lands will carry it.
        return false;
    }

    // Chunk 1 is the first payload chunk, so it sits at offset 0.
    const size_t offset = (size_t)(index - 1) * ROUTE_CHUNK_PAYLOAD;
    const size_t blob_size = Route_BlobSize(&s_manifest);
    if (offset >= blob_size || offset + payload_len > blob_size) {
        return false;
    }
    // Only the final chunk may be short. Anything else short means the sender
    // and this build disagree about ROUTE_CHUNK_PAYLOAD, which would silently
    // scatter the route through the buffer.
    const bool is_last = (index == (uint16_t)(s_manifest.chunk_count - 1));
    if (!is_last && payload_len != ROUTE_CHUNK_PAYLOAD) {
        return false;
    }

    memcpy(s_blob + offset, payload, payload_len);
    MarkChunk(index);

    if (s_received_count == s_manifest.chunk_count) {
        FinishTransfer();
    }
    return true;
}

bool NavRoute_IsLoaded() {
    return s_loaded;
}

void NavRoute_Progress(uint16_t *out_received, uint16_t *out_total) {
    if (out_received != nullptr) {
        *out_received = s_have_manifest ? s_received_count : 0;
    }
    if (out_total != nullptr) {
        *out_total = s_have_manifest ? s_manifest.chunk_count : 0;
    }
}

bool NavRoute_Manifest(RouteManifest_t *out) {
    if (out == nullptr || !s_loaded) {
        return false;
    }
    *out = s_manifest;
    return true;
}

const uint8_t *NavRoute_Blob() {
    return s_loaded ? s_blob : nullptr;
}

uint32_t NavRoute_LengthMetres() {
    return s_length_m;
}

void NavRoute_NoteLiveDirective() {
    s_last_live_ms = millis();
    s_ever_live = true;
}

void NavRoute_NotePhoneGone() {
    // The grace period exists to ride out a gap in a live link, and a clean
    // disconnect is not a gap -- it is the answer the grace period was waiting
    // for. Handing over at once avoids a stretch where the phone is provably
    // gone and the head unit is still deferring to it.
    s_ever_live = false;
    // Force the next tick to publish even if the cached route happens to yield
    // the same turn the phone last sent, so the source indicator flips.
    s_last_icon = 0xFF;
    s_last_distance = 0xFFFFFFFFu;
}

bool NavRoute_LastFix(RouteFix_t *out) {
    if (out == nullptr || !s_have_fix) {
        return false;
    }
    *out = s_last_fix;
    return true;
}

void NavRoute_Tick(uint32_t now_ms) {
    if (!s_loaded) {
        return;
    }

    // The phone wins while it is talking. Note this is a timeout on DATA, not
    // on the connection: a link that is up but silent is no more use to the
    // rider than one that is down.
    if (s_ever_live && (uint32_t)(now_ms - s_last_live_ms) < TBT_LIVE_GRACE_MS) {
        return;
    }

    GPS_Info_t gps;
    if (!DataCenter_Pull(TOPIC_GPS_INFO, &gps, sizeof(gps)) || !gps.fix_valid) {
        // No fix means no onboard navigation. Deliberately silent rather than
        // publishing an empty directive: the last live turn the phone sent is
        // better than nothing, and Page_Dashboard already ages it out.
        return;
    }

    RouteFix_t fix;
    if (!RouteFollow_Snap(s_blob, &s_manifest, s_cum, gps.lat, gps.lon, &fix)) {
        return;
    }
    s_last_fix = fix;
    s_have_fix = true;

    TBT_Directive_t directive;
    memset(&directive, 0, sizeof(directive));
    directive.source = TBT_SOURCE_ONBOARD;
    directive.off_route = fix.off_route;

    RouteManeuver_t maneuver;
    uint32_t to_maneuver = 0;
    if (RouteFollow_NextManeuver(s_blob, &s_manifest, fix.distance_along_m, &maneuver,
                                 &to_maneuver)) {
        directive.icon_id = maneuver.icon_id;
        directive.distance_m = to_maneuver;
        // The wire field is 32 bytes and the directive's is 32 including the
        // NUL, so this truncates by one byte in the worst case rather than
        // overrunning.
        strncpy(directive.street_name, maneuver.street_name, TBT_STREET_NAME_MAX - 1);
        directive.street_name[TBT_STREET_NAME_MAX - 1] = '\0';
    } else {
        // Past the last maneuver: the route is done.
        directive.icon_id = TBT_ICON_ARRIVE;
        directive.distance_m = 0;
    }

    // Off-route, the distance countdown is measured to a maneuver the rider is
    // no longer approaching, so showing it would be a confident lie. The arrow
    // and street name still say where the route wanted them, which is what
    // they need to get back.
    if (fix.off_route) {
        directive.distance_m = TBT_DISTANCE_UNKNOWN;
    }

    const bool unchanged =
        directive.icon_id == s_last_icon && directive.distance_m == s_last_distance;
    const bool due = (uint32_t)(now_ms - s_last_publish_ms) >= NAVROUTE_KEEPALIVE_MS;
    if (unchanged && !due) {
        return;
    }
    s_last_icon = directive.icon_id;
    s_last_distance = directive.distance_m;
    s_last_publish_ms = now_ms;
    DataCenter_Publish(TOPIC_NAV_TBT, &directive);
}
