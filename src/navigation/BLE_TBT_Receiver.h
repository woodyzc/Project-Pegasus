#pragma once

#include <stdint.h>
#include <stddef.h>

#include "../system/DataCenter.h"
#include "TbtParse.h"

// Turn-by-turn navigation receiver (CLAUDE.md §5: "Accept turn arrows and
// distance metrics pushed over BLE from mobile app").
//
// TBT_Directive_t and TBT_Icon_t are declared in DataCenter.h alongside the
// other topic payloads -- see the note there for why they live with the bus
// rather than in this header.
//
// ---------------------------------------------------------------------------
// Role note: this makes the device a BLE *peripheral*
// ---------------------------------------------------------------------------
// Everything else BLE in this firmware is a central (BLE_HR_Client dials out
// to a heart-rate peer). This module runs the other role: it advertises a
// service the phone connects to and writes into. NimBLE handles both roles at
// once, but both need the BLE controller, so this cannot run when the
// heart-rate source is set to ANT+ -- SoftANT_Start(false) takes the
// controller exclusively and no NimBLE host exists in that mode.
// ---------------------------------------------------------------------------
//
// The frame format and its pure decoder live in TbtParse.h/.c, which carry no
// NimBLE or Arduino dependency so they can be covered by test/host --
// the same split BleHrParse.c uses for the heart-rate characteristic.

// Custom 128-bit UUIDs (project-defined; not registered with the Bluetooth SIG).
#define TBT_SERVICE_UUID "a3c87500-8ed3-4bdf-8a39-a01bebede295"
#define TBT_CHARACTERISTIC_UUID "a3c87501-8ed3-4bdf-8a39-a01bebede295"

// The route download, written once at ride start. Same service, because a
// phone that can send turns can send the route behind them and a second
// service would cost another 16 bytes of advertisement for nothing.
//
// WRITE only, no WRITE_NR: a live turn is disposable and the next one corrects
// it, but a dropped route chunk leaves a permanent hole. The ack is what lets
// the phone know a chunk landed, and RouteParse's chunk indices are what let
// it resend just the one that did not.
#define TBT_ROUTE_CHARACTERISTIC_UUID "a3c87502-8ed3-4bdf-8a39-a01bebede295"

// Transfer progress, for the phone to poll and for the rider to see. Reads
// back four bytes: received chunks u16, total chunks u16, both little-endian.
// Notifies on change so the phone need not poll during the download.
#define TBT_STATUS_CHARACTERISTIC_UUID "a3c87503-8ed3-4bdf-8a39-a01bebede295"

// Brings up the GATT server and starts advertising, so the phone can find and
// connect to the device. Preconditions: DataCenter_Init() has run, and NimBLE
// is initialised (BLE_HR_Init() does this; call that first).
//
// MUST be called before BLE_HR_Start(), and this is a crash rather than a
// preference. Registering a GATT service needs NimBLE's table to be mutable,
// and ble_gatts_mutable() says no while a scan, a connection attempt, an
// advertisement or an established connection exists -- BLE_HR_Start() leaves
// several of those true. The refusal is not returned to the caller: it lands
// on SYSINIT_PANIC_ASSERT inside ble_svc_gap_init() and panics the chip.
//
// The failure only shows when a heart-rate peer is genuinely in range, since
// otherwise nothing connects and the registration succeeds, so getting this
// order wrong looks like flaky hardware rather than a bug.
//
// This registers the server but does NOT advertise -- see below.
void BLE_TBT_Start();

// Starts advertising. Call AFTER BLE_HR_Start(), so the two halves of this
// module straddle it:
//
//     BLE_HR_Init();
//     BLE_TBT_Start();            // register while the GATT table is mutable
//     BLE_HR_Start();             // discovery scan + connect
//     BLE_TBT_StartAdvertising(); // advertise once that has settled
//
// Registration and advertising have opposite constraints, which is why they
// are separate calls. Registration must happen before anything connects or the
// GATT table is locked and NimBLE panics. Advertising must NOT overlap the
// heart-rate discovery scan: doing both at once puts a scan, an advertisement
// and a connection attempt on the controller together, and an HCI command that
// then misses its ack deadline makes NimBLE reset its host -- during which its
// own stale timer fires into an assert and takes the chip down.
void BLE_TBT_StartAdvertising();

// ---- Radio arbitration with the heart-rate client ----
// These two modules share one BLE controller, and this is where that is
// negotiated. BLE_HR_Client brackets each connect attempt with these so the
// controller is not asked to advertise, initiate a connection and stop a scan
// all at once.
//
// That combination is what kills the board: an HCI command that misses its ack
// deadline makes NimBLE reset its host, and its own timer -- which the reset
// does not cancel -- then fires during the re-sync and hits assert(0) in
// ble_hs_timer_exp. We are on the newest NimBLE-Arduino and cannot patch it,
// so the load that provokes the timeout is the only thing left to remove.
//
// Both are safe no-ops when turn-by-turn was never started (GPX mode), and the
// pause is deliberately short: a connect attempt times out in 5s, and the
// phone only loses the chance to discover the head unit for that long.
void BLE_TBT_PauseAdvertising();
void BLE_TBT_ResumeAdvertising();

// True while a phone is connected to the TBT service.
bool BLE_TBT_IsConnected();

// ---- Diagnostics, because "the app cannot find it" has many causes ----
// The phone filters on TBT_SERVICE_UUID being in the advertisement, so the
// first question is always whether this device is advertising at all. Serial
// cannot answer it (CLAUDE.md section 8) and neither can the phone, which only
// ever reports the absence. These put the answer on the panel.

// True if the radio is advertising right now.
bool BLE_TBT_IsAdvertising();

// What the initial NimBLEDevice::startAdvertising() returned, as text. The
// return value used to be discarded, which meant a refusal to advertise was
// indistinguishable from a phone that simply never connected.
const char *BLE_TBT_StartResultText();

// How many times the supervisor has had to restart advertising. Non-zero means
// something is stopping it behind our back, which is worth knowing even though
// the supervisor papers over it.
uint32_t BLE_TBT_RestartCount();
