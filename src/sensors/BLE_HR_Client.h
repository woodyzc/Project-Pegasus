#pragma once

#include <stdint.h>
#include <stddef.h>

class NimBLERemoteCharacteristic;

// Standard BLE heart-rate client (NimBLE), and now the only heart-rate source.
// Connects to a peer advertising the Heart Rate Service (0x180D) and
// subscribes to Heart Rate Measurement (0x2A37).
//
// Discovery is deliberately separated from reconnection: BLE_HR_Start() scans
// once to learn the peer's address, and every later reconnect dials that
// stored address directly with no scan. That split was originally forced by
// ANT+ coexistence, which consumed the scan windows. ANT+ is gone and the
// split stays, because it is better behaviour on its own -- a reconnect that
// does not rescan comes back faster and does not put a scan on the controller
// beside a connection attempt, which is the load CLAUDE.md section 8 warns
// about.

// Initialises NimBLE and configures scanning for the Heart Rate Service
// (0x180D). Call once, before BLE_HR_Start().
void BLE_HR_Init();

// Discovers and connects to the peer, then starts the Core 0 supervisor task
// that keeps the link up with exponential backoff (1s, 2s, 4s ... capped 32s).
//
// The discovery half runs SYNCHRONOUSLY and blocks for up to ~15s, which is
// why main.cpp starts the UI task before calling it: the dashboard is already
// drawn and refreshing while the scan runs.
//
// Preconditions: BLE_HR_Init() and DataCenter_Init() have run.
void BLE_HR_Start();

// Disconnects cleanly so the peer stops holding the link open. Registered
// automatically as a shutdown handler by BLE_HR_Start(); exposed for any
// deliberate teardown path that does not go through esp_restart().
void BLE_HR_Shutdown();

// What the previous shutdown achieved, read from NVS at init. Distinguishes a
// goodbye that reached the peer from one that never went out -- which decides
// whether a peer refusing to advertise afterwards is our fault or its own.
const char *BLE_HR_LastShutdownText();

// Parses a Heart Rate Measurement (0x2A37) notification and publishes the BPM
// to the DataCenter. Bit 0 of the flags byte selects the value format:
// 0 => uint8 BPM, 1 => uint16 little-endian BPM.
void OnNotifyCallback(NimBLERemoteCharacteristic *characteristic, uint8_t *data, size_t length,
                      bool is_notify);

// True while a BLE HR peer is connected and subscribed.
bool BLE_HR_IsConnected();

// One word for the settings page: "connected", "reconnecting", "searching",
// or a note that only a restart will rescan. Serial is unusable on this board
// (CLAUDE.md §8), so the panel is the only place this can be seen.
const char *BLE_HR_StatusText();
