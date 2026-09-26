#pragma once

#include <stdint.h>

// BLE client for a crank cadence sensor: the Cycling Speed and Cadence service
// (0x1816), subscribing to CSC Measurement (0x2A5B), publishing revolutions
// per minute to TOPIC_CADENCE.
//
// Deliberately a near-twin of BLE_HR_Client rather than a generalisation of
// it. That file carries a lot of behaviour that was learned from one specific
// peer -- a Galaxy Watch that stops advertising after an unclean disconnect,
// and needs NVS bookkeeping on the settings page to diagnose -- and folding
// two sensors into one supervisor would have meant rewriting the only radio
// path on this board that is known to work. A cadence sensor is a far simpler
// peer: a coin cell, a supervision timeout of seconds, and no operating system
// behind it.
//
// ---------------------------------------------------------------------------
// WHAT IS SHARED, AND WHY IT HAS TO BE
// ---------------------------------------------------------------------------
// The radio. There are now two clients and a server on one controller, and
// NimBLE is configured for three connections -- so this sensor is the third
// and last, with nothing spare.
//
// Two things follow, and neither is optional:
//
//   * Connects are serialised through BleRadioGate. Two supervisors retrying
//     on their own backoffs will otherwise eventually initiate two links at
//     once, which is the load CLAUDE.md §8 says aborts the chip.
//   * This task must be parked before anything calls NimBLEDevice::deinit().
//     BLE_HR_Shutdown() owns that teardown and calls BLE_CSC_Park() first.
//     Skipping it deletes the client out from under a task that is holding a
//     pointer to it.
//
// ---------------------------------------------------------------------------
// WHAT THE PANEL SHOWS, AND WHAT THIS DECIDES
// ---------------------------------------------------------------------------
// Zero rpm and no reading are different things and both come from here. A
// connected sensor with a still crank publishes 0 after CADENCE_IDLE_MS, which
// the dashboard draws as "0". A sensor that has gone away publishes nothing,
// and the dashboard ages the cell out to "--" five seconds later. A flat
// battery must not look like a rider coasting.

#ifdef __cplusplus
extern "C" {
#endif

// Configures scan parameters. Must precede BLE_TBT_Start(), like
// BLE_HR_Init(), because it does not scan and therefore does not close the
// GATT table.
void BLE_CSC_Init(void);

// Finds a sensor and starts the supervisor. Blocking, up to one discovery
// scan. Call AFTER BLE_TBT_Start() and after BLE_HR_Start(), so that GATT
// registration is done and the two clients' first connects do not race for
// the gate at boot.
void BLE_CSC_Start(void);

// Stops the supervisor entering NimBLE, and disconnects if connected. Bounded.
// Called by BLE_HR_Shutdown() before it deinitialises the stack.
void BLE_CSC_Park(void);

bool BLE_CSC_IsConnected(void);

// One line for the settings page: whether a sensor was found, whether it is
// connected, and whether it is actually sending.
const char *BLE_CSC_StatusText(void);

#ifdef __cplusplus
}
#endif
