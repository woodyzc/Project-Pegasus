#pragma once

#include <stdint.h>
#include <stddef.h>

#include "../system/DataCenter.h"
#include "TbtParse.h"

// Turn-by-turn navigation receiver (CLAUDE.md §5: "Accept turn arrows and
// distance metrics pushed over BLE from mobile app").
//
// TBT_Directive_t and TBT_Icon_t are declared in DataCenter.h alongside the
// other topic payloads -- see the note there for why they live with the bus
// rather than in this header.
//
// ---------------------------------------------------------------------------
// Role note: this makes the device a BLE *peripheral*
// ---------------------------------------------------------------------------
// Everything else BLE in this firmware is a central (BLE_HR_Client dials out
// to a heart-rate peer). This module runs the other role: it advertises a
// service the phone connects to and writes into. NimBLE handles both roles at
// once, but both need the BLE controller, so this cannot run when the
// heart-rate source is set to ANT+ -- SoftANT_Start(false) takes the
// controller exclusively and no NimBLE host exists in that mode.
// ---------------------------------------------------------------------------
//
// The frame format and its pure decoder live in TbtParse.h/.c, which carry no
// NimBLE or Arduino dependency so they can be covered by test/host --
// the same split BleHrParse.c uses for the heart-rate characteristic.

// Custom 128-bit UUIDs (project-defined; not registered with the Bluetooth SIG).
#define TBT_SERVICE_UUID "a3c87500-8ed3-4bdf-8a39-a01bebede295"
#define TBT_CHARACTERISTIC_UUID "a3c87501-8ed3-4bdf-8a39-a01bebede295"

// Brings up the GATT server and starts advertising, so the phone can find and
// connect to the device. Preconditions: DataCenter_Init() has run, and NimBLE
// is initialised (BLE_HR_Init() does this; call that first).
void BLE_TBT_Start();

// True while a phone is connected to the TBT service.
bool BLE_TBT_IsConnected();
