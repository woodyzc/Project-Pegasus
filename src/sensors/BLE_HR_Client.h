#pragma once

#include <stdint.h>
#include <stddef.h>

class NimBLERemoteCharacteristic;

// Standard BLE heart-rate client (NimBLE): the secondary/backup HR channel
// behind software ANT+ (CLAUDE.md §3 -- "BLE Client (Secondary / Galaxy Watch
// 8)"). Connects to a peer advertising the Heart Rate Service (0x180D) and
// subscribes to Heart Rate Measurement (0x2A37).
//
// ---------------------------------------------------------------------------
// Ordering constraint when ANT+ is also running (IMPORTANT)
// ---------------------------------------------------------------------------
// deps/esp32-ant's coexist mode rides the BLE controller's passive-scan
// windows, which has two consequences documented by that library:
//
//   1. NimBLE must be initialised AND scanning before SoftANT_Start(true);
//      ant_node_stop() must come before NimBLEDevice::deinit().
//   2. BLE *scanning* is consumed while an ANT receive is open -- BLE
//      *connections* keep working, but new discovery does not. So the peer
//      must be discovered BEFORE ANT opens.
//
// That is why this module separates discovery from reconnection:
// BLE_HR_Start() scans once to learn the peer's address, and every later
// reconnect dials that stored address directly, with no scan -- which is what
// keeps the exponential-backoff reconnect working while ANT+ is live.
//
// Recommended startup order in main.cpp:
//     BLE_HR_Init();
//     BLE_HR_Start();                        // discovery + first connect
//     BLE_HR_StartCoexistScan();             // perpetual passive scan for ANT
//     SoftANT_Start(/*coexist_with_ble=*/true);
// ---------------------------------------------------------------------------

// Initialises NimBLE and configures scanning for the Heart Rate Service
// (0x180D). Call once, before BLE_HR_Start().
void BLE_HR_Init();

// Discovers and connects to the peer, then starts the Core 0 supervisor task
// that keeps the link up with exponential backoff (1s, 2s, 4s ... capped 32s).
//
// The discovery half runs SYNCHRONOUSLY and blocks for up to ~15s, matching
// how deps/esp32-ant's verified coexist example sequences it: the scan must be
// finished before anything reconfigures it for ANT, so this cannot be deferred
// into the background task without racing BLE_HR_StartCoexistScan().
//
// Preconditions: BLE_HR_Init() and DataCenter_Init() have run.
void BLE_HR_Start();

// Starts the perpetual passive scan that deps/esp32-ant's coexist mode needs
// to ride. Call only when ANT+ will run in coexist mode, and only after
// BLE_HR_Start() has had its chance to discover the peer.
void BLE_HR_StartCoexistScan();

// Parses a Heart Rate Measurement (0x2A37) notification and publishes the BPM
// to the DataCenter. Bit 0 of the flags byte selects the value format:
// 0 => uint8 BPM, 1 => uint16 little-endian BPM.
void OnNotifyCallback(NimBLERemoteCharacteristic *characteristic, uint8_t *data, size_t length,
                      bool is_notify);

// True while a BLE HR peer is connected and subscribed.
bool BLE_HR_IsConnected();

// Arbitration hook for the "ANT+ primary, BLE secondary" split in CLAUDE.md §3.
// Both sources publish to the same Sensor/HeartRate topic, so without this the
// two would fight over it and the dashboard would flip between them. When the
// hook returns true, this client stays connected but stops publishing, leaving
// the topic to the primary source.
//
// Wire it in main.cpp as:  BLE_HR_SetPrimaryActiveHook(SoftANT_IsTracking);
// Leave it unset (the default) to always publish.
void BLE_HR_SetPrimaryActiveHook(bool (*is_primary_active)());
