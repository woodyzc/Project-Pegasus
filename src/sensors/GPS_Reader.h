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
// Pin choice deviates from CLAUDE.md §2, and not for the reason this file
// used to give
// ---------------------------------------------------------------------------
// The spec puts the module on GPIO43/44. Those are UART0, physically brought
// out on this board to the header silkscreened "UART" (RXD/TXD/GND/5V) -- and
// still off limits, because serial over USB-Serial-JTAG is unusable on the
// development host (see CLAUDE.md §8), which makes that header the only
// remaining way to get real logs off this board with a USB-TTL adapter.
//
// GPIO4/GPIO5 are NOT a free alternative, despite an earlier version of this
// file claiming the GPIO matrix made any pin fair game: on the ES3C28P
// reference design this board is built from, those two are wired to the
// onboard PCM5101 I2S amp (MCLK=4, BCLK=5) -- the unlabeled header next to
// the touch FPC connector, silkscreened only "SPEAKER". Unused by firmware
// today (CLAUDE.md marks audio target-board-only), but still not electrically
// free: a module driving them fights the DAC.
//
// The only header on this board actually broken out and unclaimed is the
// 4-pin one silkscreened IO2/IO3/IO14/IO21 ("Expansion" in the same reference
// design's docs). The GNSS module goes there: RX on IO2, TX on IO3, IO14/IO21
// left for whatever needs a spare GPIO next. It has no VCC/GND of its own --
// borrow those from the I2C header's 3.3V/GND pins without touching SCL/SDA.
// Defaults below are overridable from platformio.ini per board.
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
