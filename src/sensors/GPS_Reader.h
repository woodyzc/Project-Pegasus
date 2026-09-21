#pragma once

#include <stdint.h>

// ATGM336H (中科微电子 GPS+BD) reader: pumps UART bytes through the pure NMEA
// GGA/RMC parser and publishes GPS_Info_t to TOPIC_GPS_INFO (CLAUDE.md §4,
// Core 0 Task 1).
//
// ---------------------------------------------------------------------------
// This is a substitute module, and it is a different chipset than the spec
// ---------------------------------------------------------------------------
// CLAUDE.md §2 specs a u-blox MAX-M10S, which speaks a binary UBX protocol --
// see src/sensors/UbxParse.h and GPS_Reader.cpp's git history for that path.
// The ATGM336H that actually arrived is not a u-blox part. It has no
// documented way to switch to a binary protocol and leaves the factory
// outputting plain NMEA-0183 text at 9600 baud, so this reader speaks NMEA
// instead. If the MAX-M10S ever arrives, this file is the one to swap back,
// not GPS_Info_t or anything downstream of TOPIC_GPS_INFO -- both parsers
// fill the same struct.
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
// NOTE: the NMEA decoding is covered by test/host/test_nmea_parse.c, but the
// UART wiring and baud rate are unverified against real silicon -- no
// ATGM336H has been wired in yet, only received.

// Opens the UART for NMEA input. Call once from setup(), after
// DataCenter_Init().
void GPS_Init();

// Spawns the Core 0 reader task. Preconditions: GPS_Init() has run.
void GPS_StartReader();

// True once a GGA sentence with a non-zero fix quality has been seen. Cheap
// status for the UI to distinguish "no module" from "module present, still
// acquiring".
bool GPS_HasFix();

// Number of GGA sentences accepted so far (checksum valid, decoded
// successfully -- with or without a fix). Zero after several seconds means
// nothing is arriving: wrong pins, wrong baud, or a module that is not
// actually a GPS+BD talker at all.
uint32_t GPS_FrameCount();
