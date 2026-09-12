#pragma once

#include <stdint.h>
#include <stddef.h>

#include "../system/DataCenter.h"
#include "RouteFollow.h"
#include "RouteParse.h"
#include "TbtParse.h" // TBT_DISTANCE_UNKNOWN, shared with the live directive path

// Holds the route the phone downloaded, and navigates from it when the phone
// is not there.
//
// RouteParse.h decodes the wire format and RouteFollow.h does the geometry;
// both are pure. This is the part that cannot be: it owns a PSRAM buffer, it
// is written from a NimBLE callback on one core and read by the UI on another,
// and it decides which of two navigation sources the rider is actually shown.
//
// ---------------------------------------------------------------------------
// WHICH SOURCE WINS
// ---------------------------------------------------------------------------
// The phone always wins while it is talking. It has map matching, live traffic
// and rerouting; we have a polyline and a GPS fix. The onboard path exists to
// make a dropped link a non-event, not to second-guess a connected phone.
//
// So the rule is a timeout, not a connection flag: if no live directive has
// arrived for TBT_LIVE_GRACE_MS, start publishing our own. A connection that
// is up but silent is exactly as useless to the rider as one that is down, and
// on this hardware a BLE link that has stopped carrying data while still
// reporting itself connected is a thing that actually happens.
// ---------------------------------------------------------------------------

// How long to keep trusting a silent phone before navigating on our own.
//
// The phone re-sends an unchanged turn every 10 seconds (KEEPALIVE_INTERVAL_MS
// in BleLink.kt) precisely so silence is meaningful. Three missed keepalives
// is a link that has genuinely stopped, while still being short enough that
// the rider is not staring at a frozen countdown into a junction.
#define TBT_LIVE_GRACE_MS 32000

#ifdef __cplusplus
extern "C" {
#endif

// Frees any stored route and resets the assembly state. Safe to call at any
// time; the UI simply stops seeing an onboard route.
void NavRoute_Clear();

// Feeds one chunk straight off the GATT write. Returns true if the chunk was
// well-formed and accepted, false if it was rejected -- a rejected chunk is
// dropped and the transfer stays incomplete rather than being poisoned.
//
// A manifest for a route_id different from the one being assembled starts a
// fresh transfer, which is what lets the rider re-plan mid-ride without any
// explicit cancel message.
bool NavRoute_AcceptChunk(const uint8_t *data, size_t length);

// True once every chunk has arrived and the route is usable.
bool NavRoute_IsLoaded();

// Progress of the transfer in chunks, for a "loading route" readout. Both zero
// when no transfer has started.
void NavRoute_Progress(uint16_t *out_received, uint16_t *out_total);

// The assembled route, or false when none is loaded. The blob belongs to this
// module and stays valid until the next NavRoute_Clear() or a new transfer.
bool NavRoute_Manifest(RouteManifest_t *out);
const uint8_t *NavRoute_Blob();

// Route length in metres, from the cumulative table rather than the manifest,
// so it is the length we will actually navigate against.
uint32_t NavRoute_LengthMetres();

// Call whenever a live directive arrives from the phone, so the grace timer
// above can tell a talking phone from a silent one. BLE_TBT_Receiver does this.
void NavRoute_NoteLiveDirective();

// Call when the phone disconnects. Ends the grace period immediately, so the
// next tick navigates from the cached route rather than leaving the rider
// looking at a frozen turn for the full TBT_LIVE_GRACE_MS.
void NavRoute_NotePhoneGone();

// Runs the fallback. Call periodically (once a second is plenty) from the same
// task that pumps navigation.
//
// Does nothing when a route is not loaded, when the phone is still talking, or
// when there is no GPS fix. Otherwise it snaps the fix to the route, finds the
// next maneuver and publishes a TBT_Directive_t with source TBT_SOURCE_ONBOARD
// to TOPIC_NAV_TBT -- the same topic and the same struct the phone path
// publishes, so the dashboard needs to know nothing about any of this.
void NavRoute_Tick(uint32_t now_ms);

// Where the rider is along the route, from the last NavRoute_Tick that had a
// fix. False if that has never happened.
bool NavRoute_LastFix(RouteFix_t *out);

#ifdef __cplusplus
}
#endif
