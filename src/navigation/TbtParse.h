#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Pure parser for the turn-by-turn frame the phone writes over BLE,
// deliberately free of any NimBLE/Arduino dependency so it can be built and
// tested on the host (see test/host/test_tbt_parse.c). BLE_TBT_Receiver.cpp
// calls this from its GATT write callback and copies the results into a
// TBT_Directive_t for DataCenter.
//
// Wire format (little-endian, one BLE write == one directive):
//
//   off  size  field
//   0    1     magic       0x54 ('T')
//   1    1     version     0x01
//   2    1     icon_id     maneuver code, 0..TBT_ICON_MAX_ID
//   3    1     name_len    0..31, bytes of UTF-8 street name that follow
//   4    4     distance_m  uint32, metres to the maneuver, or
//                          TBT_DISTANCE_UNKNOWN when the phone has none
//   8    n     street_name UTF-8, NOT NUL-terminated on the wire
//
// Total 8..39 bytes. Binary rather than JSON: no parser dependency, a fixed
// upper bound on frame size, and no allocation inside a BLE callback.
//
// A frame is rejected whole if the magic, version, declared length or icon is
// out of range -- a partially applied directive would show the rider a turn
// that was never sent.

#define TBT_FRAME_MAGIC 0x54
#define TBT_FRAME_VERSION 0x01
#define TBT_FRAME_HEADER_LEN 8

// Longest street name a frame may carry, excluding the NUL this parser adds.
#define TBT_STREET_NAME_LEN 31

// Highest icon code currently defined (TBT_ICON_ARRIVE in DataCenter.h). Kept
// here as a plain number so this file stays free of C++ headers.
#define TBT_ICON_MAX_ID 10

// distance_m value meaning "this maneuver is real, but its distance is not
// known". Google Maps posts a step as a bare instruction -- "Turn right onto
// Richter Farm Rd", no distance anywhere in the notification -- and on one
// drive that was 38 of 109 maneuvers. The phone app previously discarded them
// whole for want of a distance, which threw away the arrow and the street name
// as well.
//
// 0xFFFFFFFF rather than 0: zero is a legitimate distance and is what an
// arrival carries. The display shows a dash for this, never a number, because
// inventing one would tell the rider to turn at a place Maps never named.
#define TBT_DISTANCE_UNKNOWN 0xFFFFFFFFu

#ifdef __cplusplus
extern "C" {
#endif

// Decodes one frame. On success writes all three outputs and returns true; on
// any malformation returns false and leaves the outputs untouched.
// `out_street_size` must be at least TBT_STREET_NAME_LEN + 1.
bool TBT_ParseFrame(const uint8_t *data,
                    size_t length,
                    uint8_t *out_icon_id,
                    uint32_t *out_distance_m,
                    char *out_street_name,
                    size_t out_street_size);

#ifdef __cplusplus
}
#endif
