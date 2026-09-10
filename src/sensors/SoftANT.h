#pragma once

#include <stdint.h>

// Pure-software ANT+ heart-rate reception on the ESP32-S3's own 2.4GHz radio,
// with no external ANT/nRF24 hardware (CLAUDE.md §3).
//
// This module is a thin binding between deps/esp32-ant and our DataCenter --
// deliberately thin, because the vendored library already does the hard part:
//
//   * The 2.4GHz PHY is NOT configured by hand here. deps/esp32-ant's
//     `ant_espphy` drives the ESP32-S3 BLE controller as a raw ANT modem (it
//     runs the controller's LE test mode and hooks its event-start / RX-IRQ
//     functions to retune to 2457MHz, 1Mbit/s GFSK, ANT sync word, CRC and
//     whitening off -- ANT's ShockBurst air format). Re-implementing that
//     here would duplicate and fight the library.
//   * The library also owns the real receive task (`ant_node_start()` spawns
//     it at priority configMAX_PRIORITIES-2 so it can hold the 32768Hz ANT
//     TDMA grid). We leave THAT task on the library's own Core 1 default
//     rather than hand-rolling a capture loop -- see the core-choice note in
//     SoftANT.cpp for why it is the one part of this module that does not sit
//     on Core 0.
//
// So SoftANT_Task() below is a supervisory task, not the capture loop: it owns
// the node's lifecycle (start + open the HRM channel) and watches for the
// strap going away, publishing a stale/zero reading so the UI can fall back to
// "--" instead of freezing on the last-seen BPM.

// FreeRTOS entry point for the Core 0 supervisory task.
// (The task spec writes this as `void SoftANT_Task()`; it takes the void*
// parameter that xTaskCreatePinnedToCore requires.)
void SoftANT_Task(void *pvParameters);

// Creates SoftANT_Task pinned to Core 0 (CLAUDE.md §4: Background Data Core).
// Preconditions: DataCenter_Init() has already run.
//
// `coexist_with_ble`: false (default) takes the BLE controller exclusively --
// correct while no BLE host is running. Set true once NimBLE is in the picture
// (CLAUDE.md §3 BLE heart-rate / §5 BLE turn-by-turn): in that mode ANT rides
// the BLE controller's passive-scan windows instead of seizing the radio, and
// NimBLE must already be initialised AND scanning before this is called.
// See the ordering rules in SoftANT.cpp.
void SoftANT_Start(bool coexist_with_ble = false);

// Extracts the real-time BPM from an 8-byte ANT+ HRM data page.
// Returns 0 if `payload` isn't a decodable HRM page (or the strap reports an
// invalid rate). Delegates to the vendored library's profile decoder rather
// than re-implementing ANT+ page parsing.
uint8_t ParseBPM(uint8_t *payload);

// True while an ANT+ HRM channel is currently tracking a strap.
bool SoftANT_IsTracking();
