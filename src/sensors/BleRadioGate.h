#pragma once

#include <stdint.h>

// One radio, two sensors, and the rule that only one of them may be talking to
// the controller at a time.
//
// This exists because of the failure CLAUDE.md §8 records at length: asking
// the controller to stop a scan and initiate a link while it is also busy
// makes an HCI command miss its ack deadline, NimBLE answers that by resetting
// its host, and its own uncancelled timer then fires during the re-sync and
// hits assert(0) in ble_hs_timer_exp. The chip aborts. The library defect is
// not ours to fix, so the load that provokes it is what has to go.
//
// Until cadence arrived there was exactly one client, and bracketing its
// connect with BLE_TBT_PauseAdvertising() was enough. A second client makes
// "two connect attempts at once" reachable for the first time -- and it is the
// same load, from a different direction, with no advertisement involved. Both
// supervisors run on Core 0 and both retry on their own backoff, so the
// overlap is not a rare interleaving: two sensors out of range come back at
// whatever moment they come back.
//
// Held across discovery as well as the connect. Two scans at once serve no
// purpose -- they share one radio and each would get a fraction of the duty
// cycle the other was tuned for.
//
// ⚠️ This is a lock held for SECONDS. A discovery scan is 15s and a connect
// timeout 5s. That is fine for what it guards -- two background supervisors
// that have nothing to do but wait -- and would be badly wrong for anything
// on the UI path. Nothing on Core 1 may take it.

#ifdef __cplusplus
extern "C" {
#endif

// Creates the mutex. Call once from setup(), before either client starts.
// Safe to call twice; the second call does nothing.
void BleRadioGate_Init(void);

// Blocks until the radio is free. Returns false only if the gate was never
// initialised, in which case the caller should proceed anyway -- refusing to
// connect because a lock is missing would turn a missing call into a silently
// dead sensor.
bool BleRadioGate_Acquire(void);

// Releases it. Must be paired with an Acquire that returned true; the RAII
// guard below is the only thing that should be calling either.
void BleRadioGate_Release(void);

#ifdef __cplusplus
}

// Scope guard, so no early return out of a connect path can leak the lock.
// Every exit from ConnectAndSubscribe() is one of those.
class BleRadioGateHold {
public:
    BleRadioGateHold() : held_(BleRadioGate_Acquire()) {}
    ~BleRadioGateHold() {
        if (held_) {
            BleRadioGate_Release();
        }
    }
    BleRadioGateHold(const BleRadioGateHold &) = delete;
    BleRadioGateHold &operator=(const BleRadioGateHold &) = delete;

private:
    bool held_;
};
#endif
