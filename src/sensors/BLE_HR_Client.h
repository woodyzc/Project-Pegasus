#pragma once

#include <stdint.h>
#include <stddef.h>

class NimBLERemoteCharacteristic;

// Standard BLE heart-rate client (NimBLE), and now the only heart-rate source.
// Connects to a peer advertising the Heart Rate Service (0x180D) and
// subscribes to Heart Rate Measurement (0x2A37).
//
// Discovery is deliberately separated from reconnection: BLE_HR_Start() scans
// to learn the peer's address, and every later reconnect dials that stored
// address directly with no scan. That split was originally forced by ANT+
// coexistence, which consumed the scan windows. ANT+ is gone and the split
// stays, because it is better behaviour on its own -- a reconnect that does
// not rescan comes back faster and does not put a scan on the controller
// beside a connection attempt, which is the load CLAUDE.md section 8 warns
// about.
//
// It is NOT one-shot, whatever older comments here used to say. The supervisor
// rescans every couple of seconds for as long as it has no peer, and after a
// few failed direct connects it throws the stored address away and goes back
// to discovery -- because a watch's resolvable private address rotates, so the
// address that worked an hour ago can be dead while the peer is broadcasting
// happily under a new one. Nothing here needs a restart to look again.
//
// ---------------------------------------------------------------------------
// The peer can still make itself unfindable, and so far only a watch has
// ---------------------------------------------------------------------------
// If the head unit resets without saying goodbye, the peer goes on believing
// the link is up -- and a peripheral that thinks it is connected stops
// advertising. The board then scans for something deliberately not there.
// BLE_HR_Shutdown() exists to prevent exactly this; see its comment.
//
// Observed on the bench with a Galaxy Watch 8 (2026-09-15): after a restart it
// never came back, and the only cure was switching broadcasting off on the
// watch and restarting again. That is a watch-shaped failure, and there is
// good reason to expect a plain strap not to share it:
//
//   * a strap's supervision timeout is seconds, so it notices the dead link
//     and resumes advertising on its own almost immediately;
//   * a watch is a whole operating system with its own connection manager and
//     app-level state, which can hold a phantom link far longer and may not
//     re-advertise until something prods it.
//
// That is reasoning, not a measurement -- no strap has been tested here yet.
// When one is, the test is: connect, restart, then touch nothing and watch how
// long the link status on the settings page takes to come back by itself.

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

// One line for the settings page: "connected (N.Ns)", "connected, no data",
// "reconnecting (N fails)", or "searching (N seen, M HR)". Serial is unusable
// on this board (CLAUDE.md §8), so the panel is the only place this can be
// seen.
//
// The M in that last one is the whole diagnostic: M == 0 means no peer is
// advertising the heart-rate service at all, which is the peer holding a
// phantom link and is not something this end can fix. M > 0 with no
// connection is ours.
const char *BLE_HR_StatusText();
