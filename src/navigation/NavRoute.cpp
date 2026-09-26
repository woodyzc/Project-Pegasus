#include "NavRoute.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

namespace {

// Guards the route store against its two tasks.
//
// This module is written from NimBLE's host task -- every characteristic
// callback lands there -- and read from the Core 1 task that pumps
// NavRoute_Tick(). The header has said so since the file was written, and for
// just as long nothing enforced it: finishing a transfer heap_caps_free()s
// the live blob to swap the new one in, while Tick() may be part-way through
// snapping a fix to that exact allocation. It is not a rare interleaving
// either. The
// phone re-sends the whole route on a reroute, which is a supported and
// ordinary thing for it to do mid-ride, and it is precisely when the rider is
// relying on the display.
//
// Recursive because the accessors call each other -- Tick() ends by calling
// NavRoute_EnrichDirective(), which locks again on the same task.
//
// LOCK ORDER: this lock may be taken while DataCenter's is NOT held, never
// the other way round. OnGpsPublished() below runs inside DataCenter_Publish()
// with DataCenter's mutex held, so it must not touch this one -- it bumps a
// volatile counter and nothing else, which is the whole reason that counter
// exists. Tick() releases this lock before it publishes, for the same reason.
SemaphoreHandle_t s_lock = nullptr;

// Scoped hold. Safe before NavRoute_Init() has run and safe if the semaphore
// could not be created: a board that cannot allocate one has worse problems
// than this race, and failing to navigate would be the wrong answer to it.
struct RouteLock {
    RouteLock() {
        if (s_lock != nullptr) {
            xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
        }
    }
    ~RouteLock() {
        if (s_lock != nullptr) {
            xSemaphoreGiveRecursive(s_lock);
        }
    }
    RouteLock(const RouteLock &) = delete;
    RouteLock &operator=(const RouteLock &) = delete;
};

// ---- The live route: what the rider is navigating right now ----
//
// The assembled blob and the cumulative distance table both live in PSRAM:
// a 4,000-point route is 32KB of polyline and another 16KB of table, which is
// a large fraction of internal RAM and none of it is touched from an ISR.
uint8_t *s_blob = nullptr;
uint32_t *s_cum = nullptr;

RouteManifest_t s_manifest;
bool s_loaded = false;
uint32_t s_length_m = 0;

// ---- The transfer in flight, staged apart from the live route ----
//
// These were the same variables until an incoming route was found to destroy
// the one being ridden before anything had judged it. BeginTransfer() freed
// the store the moment a MANIFEST arrived, so the rider lost their navigation
// to a transfer that had not delivered a single point yet -- and if that
// transfer then failed to arrive, or was refused by FinishTransfer()'s
// ordering check, there was nothing left to fall back to and no indication
// that anything had gone.
//
// The failure that matters is not the refusal, which needs a broken router.
// It is the phone walking out of range mid-upload, which is ordinary on a
// bike and leaves the manifest received and the chunks not. Validating before
// freeing would not have helped there; only building somewhere else does.
//
// So a transfer now assembles here and the live route is replaced by an
// atomic swap in FinishTransfer(), after the route has been measured and its
// maneuvers checked. A failed or abandoned transfer costs the staging memory
// and nothing else. The peak cost is both routes resident at once, which is
// what PSRAM is for.
uint8_t *s_rx_blob = nullptr;
uint32_t *s_rx_cum = nullptr;

// Which chunks have landed. One bit each, in internal RAM because it is read
// and written on every single chunk and it is tiny.
uint8_t *s_rx_received = nullptr;
size_t s_rx_received_bytes = 0;
uint16_t s_rx_received_count = 0;

RouteManifest_t s_rx_manifest;
bool s_rx_have_manifest = false;

RouteFix_t s_last_fix;
bool s_have_fix = false;

// Last time a live directive arrived from the phone. The fallback waits for
// this to go stale rather than watching the connection state -- see the header.
uint32_t s_last_live_ms = 0;
bool s_ever_live = false;

// Bumped on every live directive from the phone. NavRoute_Tick decides to
// publish under the lock and then publishes with it released, so the phone's
// NimBLE host task can slip a fresh turn in between the two and lose the race
// to the onboard turn it just superseded -- both write TOPIC_NAV_TBT and the
// last writer wins. A stamp in milliseconds cannot settle that: two events in
// one millisecond read as one. A counter can.
uint32_t s_live_seq = 0;

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

// How long a position stays believable.
//
// The same five seconds Page_Dashboard blanks SPEED after, and deliberately
// so: both publishers send at 1Hz (the receiver, and the phone's
// requestLocationUpdates at 1000ms), so five is five missed fixes, and the
// panel should not be navigating from a position it has already stopped
// showing a speed for.
#define NAVROUTE_FIX_STALE_MS 5000

// Bumped by the DataCenter callback below on every VALID fix, and read by
// NavRoute_Tick to notice that one arrived.
//
// This exists because DataCenter_Pull has no concept of freshness: once a
// topic has ever been published it keeps handing out that same last value for
// ever. So `fix_valid` on a pulled struct does not mean "there is a fix", it
// means "there was one, once" -- and the whole onboard path was built on that
// reading. See NavRoute_Tick.
//
// A counter rather than a timestamp because the callback runs on the
// publisher's core and Tick runs on the UI's: this way the only thing crossing
// between them is one aligned 32-bit word that nothing compares against a
// clock, and every deadline is measured against the now_ms Tick is handed.
volatile uint32_t s_fix_seq = 0;
uint32_t s_seen_fix_seq = 0;
uint32_t s_last_fix_ms = 0;
bool s_have_fix_time = false;

void OnGpsPublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)user_arg;
    if (data == nullptr || size < sizeof(GPS_Info_t)) {
        return;
    }
    // Stamped on a valid fix, never on a publish. GPS_Reader publishes at 1Hz
    // with fix_valid false while it acquires -- a climbing num_sv is how
    // "module present, still searching" is told from "no module" -- so
    // counting publishes would read a receiver in a tunnel as a live position
    // for as long as the tunnel.
    const GPS_Info_t *info = (const GPS_Info_t *)data;
    if (!info->fix_valid) {
        return;
    }
    s_fix_seq++;
}

Account s_gps_account("NavRoute/GPS", OnGpsPublished);

void FreeLive() {
    if (s_blob != nullptr) {
        heap_caps_free(s_blob);
        s_blob = nullptr;
    }
    if (s_cum != nullptr) {
        heap_caps_free(s_cum);
        s_cum = nullptr;
    }
    s_loaded = false;
    s_length_m = 0;
}

void FreeRx() {
    if (s_rx_blob != nullptr) {
        heap_caps_free(s_rx_blob);
        s_rx_blob = nullptr;
    }
    if (s_rx_cum != nullptr) {
        heap_caps_free(s_rx_cum);
        s_rx_cum = nullptr;
    }
    if (s_rx_received != nullptr) {
        heap_caps_free(s_rx_received);
        s_rx_received = nullptr;
    }
    s_rx_received_bytes = 0;
    s_rx_received_count = 0;
    s_rx_have_manifest = false;
}

// The snap and the published directive both describe a route. Reset them
// whenever the route underneath them changes or goes away, or the first tick
// afterwards measures a new polyline from the old route's position.
void ResetFollowState() {
    s_have_fix = false;
    s_last_icon = 0xFF;
    s_last_distance = 0xFFFFFFFFu;
}

bool ChunkSeen(uint16_t index) {
    if (s_rx_received == nullptr) {
        return false;
    }
    return (s_rx_received[index >> 3] & (uint8_t)(1u << (index & 7))) != 0;
}

void MarkChunk(uint16_t index) {
    if (s_rx_received == nullptr || ChunkSeen(index)) {
        return;
    }
    s_rx_received[index >> 3] |= (uint8_t)(1u << (index & 7));
    s_rx_received_count++;
}

// A manifest starts a transfer. Allocating here rather than on the first
// payload chunk means an out-of-order arrival -- which BLE permits and Android
// does in practice under load -- is stored rather than dropped.
bool BeginTransfer(const RouteManifest_t &m) {
    // Only the staging area. The live route is not touched here -- see the
    // note on s_rx_blob for what happened when it was.
    FreeRx();

    const size_t blob_size = Route_BlobSize(&m);
    s_rx_blob = (uint8_t *)heap_caps_calloc(blob_size, 1, MALLOC_CAP_SPIRAM);
    if (s_rx_blob == nullptr) {
        return false;
    }
    s_rx_cum = (uint32_t *)heap_caps_calloc(m.point_count, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (s_rx_cum == nullptr) {
        FreeRx();
        return false;
    }

    s_rx_received_bytes = ((size_t)m.chunk_count + 7) / 8;
    s_rx_received = (uint8_t *)heap_caps_calloc(s_rx_received_bytes, 1, MALLOC_CAP_INTERNAL);
    if (s_rx_received == nullptr) {
        FreeRx();
        return false;
    }

    s_rx_manifest = m;
    s_rx_have_manifest = true;
    MarkChunk(0); // the manifest is chunk 0 and has now arrived
    return true;
}

// Called once the last chunk lands. Builds the cumulative table so every
// subsequent fix is a snap rather than a re-measure of the whole polyline.
void FinishTransfer() {
    const uint32_t length_m = RouteFollow_BuildCumulative(s_rx_blob, &s_rx_manifest, s_rx_cum);

    // A route whose points are all identical measures zero and cannot be
    // navigated -- treat it as a failed transfer rather than dividing by it
    // later.
    //
    // Maneuver order is checked here for the same reason and with the same
    // answer. RouteFollow_NextManeuver scans linearly and takes the first
    // entry at or beyond the rider, so an inverted array does not fail: it
    // hands back a turn already ridden past, every second, for the whole
    // route. Refusing the route shows the rider nothing, which is honest;
    // accepting it shows them a wrong turn, which is not. Once per transfer,
    // 256 entries at most.
    const bool usable = length_m > 0 && RouteFollow_ManeuversOrdered(s_rx_blob, &s_rx_manifest);

    if (!usable) {
        // The rider keeps whatever they were already navigating. Refusing a
        // route is not a reason to take away a good one.
        FreeRx();
        return;
    }

    // ---- The swap ----
    // Under the same lock every reader takes, so Tick() cannot be part-way
    // through snapping to the old blob while it is freed.
    FreeLive();
    s_blob = s_rx_blob;
    s_rx_blob = nullptr;
    s_cum = s_rx_cum;
    s_rx_cum = nullptr;
    s_manifest = s_rx_manifest;
    s_length_m = length_m;
    s_loaded = true;

    // Releases the chunk bitmap and clears the transfer's bookkeeping. The
    // blob and table are already moved out, so this does not free them.
    FreeRx();

    ResetFollowState();
}

} // namespace

void NavRoute_Init() {
    // Before the subscription, and before anything else can reach the store.
    // main.cpp calls this well ahead of the radios, so no callback can arrive
    // first, but the accessors tolerate a null lock anyway rather than
    // depending on that ordering staying true.
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }

    // Subscribed even with no route loaded. The subscription is what makes
    // position freshness knowable, and a route can arrive from the phone at
    // any moment afterwards -- subscribing only once one had would leave the
    // first seconds of every route navigating on an unstamped fix.
    DataCenter_Subscribe(TOPIC_GPS_INFO, &s_gps_account);
}

void NavRoute_Clear() {
    RouteLock lock;
    FreeLive();
    FreeRx();
    ResetFollowState();
}

bool NavRoute_AcceptChunk(const uint8_t *data, size_t length) {
    RouteLock lock;

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
        if (s_rx_have_manifest && m.route_id == s_rx_manifest.route_id &&
            m.point_count == s_rx_manifest.point_count &&
            m.maneuver_count == s_rx_manifest.maneuver_count) {
            return true;
        }
        return BeginTransfer(m);
    }

    if (!s_rx_have_manifest || index >= s_rx_manifest.chunk_count) {
        // A payload chunk with no manifest cannot be placed: its offset comes
        // from the manifest's geometry. Dropped, and the phone's retry after
        // the manifest lands will carry it.
        return false;
    }

    // Chunk 1 is the first payload chunk, so it sits at offset 0.
    const size_t offset = (size_t)(index - 1) * ROUTE_CHUNK_PAYLOAD;
    const size_t blob_size = Route_BlobSize(&s_rx_manifest);
    if (offset >= blob_size || offset + payload_len > blob_size) {
        return false;
    }
    // Only the final chunk may be short. Anything else short means the sender
    // and this build disagree about ROUTE_CHUNK_PAYLOAD, which would silently
    // scatter the route through the buffer.
    const bool is_last = (index == (uint16_t)(s_rx_manifest.chunk_count - 1));
    if (!is_last && payload_len != ROUTE_CHUNK_PAYLOAD) {
        return false;
    }

    memcpy(s_rx_blob + offset, payload, payload_len);
    MarkChunk(index);

    if (s_rx_received_count == s_rx_manifest.chunk_count) {
        FinishTransfer();
    }
    return true;
}

bool NavRoute_IsLoaded() {
    RouteLock lock;
    return s_loaded;
}

void NavRoute_Progress(uint16_t *out_received, uint16_t *out_total) {
    RouteLock lock;
    // The transfer in flight, not the route in use: this is what the upload
    // progress bar is watching, and it ticks while the live route -- if there
    // is one -- carries on being navigated underneath it.
    if (out_received != nullptr) {
        *out_received = s_rx_have_manifest ? s_rx_received_count : 0;
    }
    if (out_total != nullptr) {
        *out_total = s_rx_have_manifest ? s_rx_manifest.chunk_count : 0;
    }
}

bool NavRoute_Manifest(RouteManifest_t *out) {
    RouteLock lock;
    if (out == nullptr || !s_loaded) {
        return false;
    }
    *out = s_manifest;
    return true;
}

uint32_t NavRoute_LengthMetres() {
    RouteLock lock;
    return s_length_m;
}

void NavRoute_NoteLiveDirective() {
    RouteLock lock;
    s_last_live_ms = millis();
    s_ever_live = true;
    s_live_seq++;
}

void NavRoute_NotePhoneGone() {
    // The grace period exists to ride out a gap in a live link, and a clean
    // disconnect is not a gap -- it is the answer the grace period was waiting
    // for. Handing over at once avoids a stretch where the phone is provably
    // gone and the head unit is still deferring to it.
    RouteLock lock;
    s_ever_live = false;
    // Force the next tick to publish even if the cached route happens to yield
    // the same turn the phone last sent, so the source indicator flips.
    s_last_icon = 0xFF;
    s_last_distance = 0xFFFFFFFFu;
}

bool NavRoute_LastFix(RouteFix_t *out) {
    RouteLock lock;
    if (out == nullptr || !s_have_fix) {
        return false;
    }
    *out = s_last_fix;
    return true;
}

bool NavRoute_EnrichDirective(TBT_Directive_t *directive) {
    RouteLock lock;
    if (directive == nullptr) {
        return false;
    }
    // Say "unknown" up front, so every early return below leaves the
    // directive honest rather than carrying whatever the caller had.
    directive->then_icon_id = TBT_ICON_NONE;
    directive->then_distance_m = 0;
    directive->remaining_m = TBT_DISTANCE_UNKNOWN;

    if (!s_loaded || !s_have_fix || s_blob == nullptr) {
        return false;
    }

    // How much route is left. Clamped rather than allowed to wrap: a fix that
    // snapped just past the end would otherwise read as four thousand
    // kilometres to go.
    if (s_last_fix.distance_along_m < s_manifest.total_length_m) {
        directive->remaining_m = s_manifest.total_length_m - s_last_fix.distance_along_m;
    } else {
        directive->remaining_m = 0;
    }

    // The maneuver after next. Found by asking the same question twice: once
    // from where the rider is, then again from just past whatever that
    // returned.
    {
        RouteManeuver_t first;
        RouteManeuver_t second;
        uint32_t ignored = 0;

        if (!RouteFollow_NextManeuver(s_blob, &s_manifest, s_last_fix.distance_along_m, &first,
                                      &ignored)) {
            return true; // Past the last maneuver; remaining_m still stands.
        }
        if (!RouteFollow_NextManeuver(s_blob, &s_manifest, first.distance_along_route_m + 1,
                                      &second, &ignored)) {
            return true; // The next one is the last; there is no "then".
        }

        directive->then_icon_id = second.icon_id;
        // The gap BETWEEN the two maneuvers, not the distance from the rider.
        // "Then in 40 m" means 40 metres after the turn being made, which is
        // what a rider needs to know to take the first one in the right lane.
        directive->then_distance_m =
            second.distance_along_route_m - first.distance_along_route_m;
    }
    return true;
}

// Everything Tick does that touches the store, under the lock. Returns true
// when *out carries a directive the caller should publish.
//
// Split out so DataCenter_Publish() happens with this module's lock released:
// publishing runs every subscriber's callback synchronously, on this task, and
// holding two buses' locks at once is how a deadlock gets built by accident.
// See the LOCK ORDER note at the top of this file.
static bool TickLocked(uint32_t now_ms, TBT_Directive_t *out, uint32_t *out_live_seq) {
    *out_live_seq = s_live_seq;
    if (!s_loaded) {
        return false;
    }

    // Has a valid fix arrived since the last time through here?
    const uint32_t seq = s_fix_seq;
    if (seq != s_seen_fix_seq) {
        s_seen_fix_seq = seq;
        s_last_fix_ms = now_ms;
        s_have_fix_time = true;
    }

    // A position that has stopped arriving is not a position.
    //
    // Without this the fallback outlived the phone entirely: stop the link and
    // the last fix sits in the topic buffer for ever with fix_valid still set,
    // so this function snapped that frozen point to the route, produced a
    // turn, and republished it every keepalive -- which reset the dashboard's
    // own 30s expiry on every pass. The panel showed an amber arrow and a
    // street name indefinitely, next to a SPEED cell that had gone to dashes
    // within five seconds, and the one told the truth while the other did not.
    const bool fix_fresh =
        s_have_fix_time && (uint32_t)(now_ms - s_last_fix_ms) <= NAVROUTE_FIX_STALE_MS;

    // Aged out here rather than after the handover below, so it happens even
    // while the phone is the one navigating. s_last_fix feeds two things that
    // both go wrong quietly when it is stale: the distances
    // NavRoute_EnrichDirective() hangs off a live phone directive, and the
    // hint the snap below is about to be given. A hint is only worth having
    // because it says where the rider was a second ago; five seconds of
    // silence is exactly when it stops saying that.
    if (!fix_fresh) {
        s_have_fix = false;
    }

    // The phone wins while it is talking. Note this is a timeout on DATA, not
    // on the connection: a link that is up but silent is no more use to the
    // rider than one that is down.
    if (s_ever_live && (uint32_t)(now_ms - s_last_live_ms) < TBT_LIVE_GRACE_MS) {
        return false;
    }

    if (!fix_fresh) {
        return false;
    }

    GPS_Info_t gps;
    if (!DataCenter_Pull(TOPIC_GPS_INFO, &gps, sizeof(gps)) || !gps.fix_valid) {
        // No fix means no onboard navigation. Deliberately silent rather than
        // publishing an empty directive: the last live turn the phone sent is
        // better than nothing, and Page_Dashboard already ages it out.
        return false;
    }

    // Told where the rider was on the previous fix, which is the only thing
    // that can settle an out-and-back -- both legs of one are the same points
    // in the same order, so they snap identically and the geometry has no
    // opinion. See RouteFollow_SnapFrom. The hint is bounded there, so a fix
    // that genuinely belongs elsewhere still re-acquires immediately.
    RouteFix_t fix;
    if (!RouteFollow_SnapFrom(s_blob, &s_manifest, s_cum, gps.lat, gps.lon, s_have_fix,
                              s_have_fix ? s_last_fix.distance_along_m : 0, &fix)) {
        return false;
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
        directive.exit_number = maneuver.exit_number;
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
        return false;
    }
    s_last_icon = directive.icon_id;
    s_last_distance = directive.distance_m;
    s_last_publish_ms = now_ms;
    NavRoute_EnrichDirective(&directive);
    *out = directive;
    return true;
}

void NavRoute_Tick(uint32_t now_ms) {
    TBT_Directive_t directive;
    bool publish = false;
    uint32_t live_seq = 0;
    {
        RouteLock lock;
        publish = TickLocked(now_ms, &directive, &live_seq);
    }
    if (!publish) {
        return;
    }

    // Did the phone speak while the lock was down? TickLocked found it silent
    // past the grace period, decided the head unit should navigate, and then
    // released the lock so that DataCenter_Publish below does not run every
    // subscriber's callback with this module's lock held. That gap is short
    // and it is not empty: a live turn arriving in it is published by NimBLE's
    // host task, and if this publish lands afterwards the rider is shown the
    // onboard turn that the phone's arrival had just superseded.
    //
    // It self-corrects on the phone's next frame, so this is a flicker at the
    // handover rather than a lasting wrong turn -- but the handover is exactly
    // when the rider looks down to see who is navigating.
    //
    // Dropping the publish rather than retrying, and deliberately not rolling
    // back the keepalive bookkeeping TickLocked just wrote: the phone now owns
    // the panel for the next TBT_LIVE_GRACE_MS anyway, and if it falls silent
    // again the keepalive is long past due by then, so the onboard path
    // resumes on the very next tick rather than waiting.
    {
        RouteLock lock;
        if (s_live_seq != live_seq) {
            return;
        }
    }

    DataCenter_Publish(TOPIC_NAV_TBT, &directive);
}
