#pragma once

#include <stdint.h>

// u-blox MAX-M10S reader: pumps UART bytes through the pure UBX parser and
// publishes GPS_Info_t to TOPIC_GPS_INFO (CLAUDE.md §4, Core 0 Task 1).
//
// ---------------------------------------------------------------------------
// Pin choice deviates from CLAUDE.md §2 on purpose
// ---------------------------------------------------------------------------
// The spec puts the module on GPIO43/44. Those are UART0 -- the only remaining
// way to get real logs off this board, since serial over USB-Serial-JTAG is
// unusable on the development host (see CLAUDE.md §8). The ESP32-S3 routes any
// UART to any pin through the GPIO matrix, so there is no reason to spend the
// debug port on the GNSS module. Defaults below are overridable from
// platformio.ini per board.
// ---------------------------------------------------------------------------
//
// NOTE: no MAX-M10S has ever been attached to this project. The UBX decoding
// is covered by test/host/test_ubx_parse.c, but the UART wiring, the baud rate
// and the CFG-VALSET configuration below are unverified against real silicon.

// Opens the UART and configures the receiver for UBX-only NAV-PVT output.
// Call once from setup(), after DataCenter_Init().
void GPS_Init();

// Spawns the Core 0 reader task. Preconditions: GPS_Init() has run.
void GPS_StartReader();

// True once a NAV-PVT with a valid fix has been seen. Cheap status for the UI
// to distinguish "no module" from "module present, still acquiring".
bool GPS_HasFix();

// Number of NAV-PVT frames accepted so far. Zero after several seconds means
// nothing is arriving: wrong pins, wrong baud, or the module never configured.
uint32_t GPS_FrameCount();
