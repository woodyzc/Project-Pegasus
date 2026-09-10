#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Pure parser for the BLE Heart Rate Measurement characteristic (0x2A37),
// deliberately free of any NimBLE/Arduino dependency so it can be built and
// tested on the host (see test/host/test_ble_hr_parse.c). BLE_HR_Client.cpp
// calls this from its notify callback.
//
// Layout: byte 0 is the flags field; bit 0 selects the heart-rate value width
// (0 => uint8 at byte 1, 1 => uint16 little-endian at bytes 1..2).
//
// Returns true and writes *out_bpm only for a well-formed packet carrying a
// plausible rate. Returns false for a null/short buffer, a packet whose flags
// claim a uint16 it is too short to contain, a zero reading ("no value"), or a
// value above 255 (not a real heart rate, and it would wrap HeartRate_t::bpm).
#ifdef __cplusplus
extern "C" {
#endif

bool BLE_HR_ParseMeasurement(const uint8_t *data, size_t length, uint16_t *out_bpm);

#ifdef __cplusplus
}
#endif
