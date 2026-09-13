#pragma once

#include <stdint.h>
#include <stddef.h>

// WiFi file transfer: the board becomes an access point serving the SD card
// over HTTP, so rides come off and routes and maps go on without pulling the
// card out.
//
// ---------------------------------------------------------------------------
// Why this is a mode rather than a feature
// ---------------------------------------------------------------------------
// WiFi and Bluetooth share one antenna on this chip, and this project already
// has a scar from that: advertising while the heart-rate client connected made
// an HCI command miss its acknowledgement and panicked the board (CLAUDE.md
// section 8). Rather than add WiFi traffic to that contention and hope the
// coexistence arbitration holds, starting the server shuts the BLE stack down
// and takes the radio outright.
//
// That is also why there is no way back except a restart. Re-initialising
// NimBLE after a deinit is exactly the kind of teardown-and-rebuild that
// section 8 is a list of, and a reboot costs a few seconds of a stationary
// rider's time. FileServer_StopAndRestart() is honest about what it does.
//
// The SD card is the other reason this is exclusive. FS access is not
// thread-safe here, and the card is already read by the LVGL task (routes,
// maps) and written by the ride-log task. While the server runs, the UI is
// behind a modal overlay and recording is refused, so the HTTP task is the
// only thing touching the card.
// ---------------------------------------------------------------------------
//
// What may be read and written is FilePath.h's business, and it is host
// tested. Nothing here widens it.

// Brings up the access point and starts serving. Returns false if the card is
// not mounted, a ride is being recorded, or the radio would not start; the
// reason is in FileServer_StatusText().
//
// Safe to call twice: the second call is a no-op that returns true.
bool FileServer_Start();

// Tears the access point down and reboots, which is the only supported way
// back to riding. Does not return.
void FileServer_StopAndRestart();

bool FileServer_IsRunning();

// The network name, and the password generated for this session. The password
// is fresh every time the server starts, because the access point's BSSID is
// the chip's own MAC -- anything derived from the MAC would be printed on the
// side of the box it is meant to protect.
const char *FileServer_Ssid();
const char *FileServer_Password();

// Where to point a browser, as a URL.
const char *FileServer_Url();

// How many stations are associated. Enough to tell "the phone joined" from
// "the phone is still looking", which is the question someone asks first.
int FileServer_ClientCount();

// Requests served since the access point came up, and what last happened --
// an upload, a download, a refusal. The panel is the only place this can be
// seen, since serial is unusable on this board.
uint32_t FileServer_RequestCount();
const char *FileServer_StatusText();
