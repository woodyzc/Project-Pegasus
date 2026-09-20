#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Cadence, from the BLE Cycling Speed and Cadence Measurement characteristic
// (0x2A5B). Pure -- no NimBLE, no Arduino -- so every rule below is covered by
// test/host/test_csc_parse.c rather than discovered on a bike.
//
// Split in two on purpose, because the two halves fail differently:
//
//   Csc_ParseMeasurement()  decodes one notification. Wrong here means a
//                           malformed packet was read as data.
//   CadenceTracker_*        turns a series of them into an rpm. Wrong here
//                           means well-formed packets produce a wrong number,
//                           which is the half that cannot be spotted by
//                           looking at it.
//
// ---------------------------------------------------------------------------
// THE WIRE FORMAT
// ---------------------------------------------------------------------------
// Byte 0 is flags: bit 0 says wheel data is present, bit 1 says crank data is.
// Then, in that order and only if present:
//
//   wheel: uint32 cumulative revolutions, uint16 last event time
//   crank: uint16 cumulative revolutions, uint16 last event time
//
// Every event time is in 1/1024 second units and wraps at 65536, which is 64
// seconds. Both counters are free-running and neither resets -- a sensor that
// has been on the bike a while starts mid-count, so absolute values mean
// nothing and only differences do.
//
// A cadence sensor sends crank data only, and this header is named for that
// use. Wheel fields are parsed to find the crank ones behind them, and then
// ignored: speed comes from the position fix, and a wheel counter would need
// a circumference nobody has entered.

#define CSC_FLAG_WHEEL_PRESENT 0x01
#define CSC_FLAG_CRANK_PRESENT 0x02

typedef struct {
    // Free-running, wrapping at 65536. Meaningless alone.
    uint16_t crank_revs;
    // 1/1024 s ticks, also wrapping at 65536.
    uint16_t crank_event_time;
} CscMeasurement_t;

#ifdef __cplusplus
extern "C" {
#endif

// Decodes one 0x2A5B notification.
//
// Returns false -- writing nothing -- for a null or short buffer, for a packet
// whose flags claim fields it is too short to contain, and for a packet with
// no crank data at all. That last one is not an error on the wire: a combined
// speed-and-cadence sensor is entitled to send a wheel-only notification, and
// the honest answer to "what is the cadence" is then that this packet does not
// say.
bool Csc_ParseMeasurement(const uint8_t *data, size_t length, CscMeasurement_t *out);

// ---------------------------------------------------------------------------
// TURNING MEASUREMENTS INTO AN RPM
// ---------------------------------------------------------------------------

// Above this, believe the arithmetic rather than the number.
//
// A sprint on a track bike is about 150. 250 leaves room for anything real and
// still rejects the two ways this goes wrong: a garbled packet, and a wrapped
// event time misread as a very short interval.
#define CADENCE_MAX_RPM 250

// How long the crank may fail to turn before the answer is zero rather than
// the last rpm.
//
// A stopped rider is the normal case, not an error, and a sensor keeps
// notifying while stopped with its counters unchanged -- so without this the
// panel would hold the last cadence for as long as the rider coasted. Three
// seconds is a little over one crank revolution at 20rpm, which is slower than
// anyone pedals on purpose, so it cannot fire on a rider who is still turning
// the cranks.
#define CADENCE_IDLE_MS 3000

typedef struct {
    // The previous accepted sample, and when it arrived by our clock.
    uint16_t last_revs;
    uint16_t last_event_time;
    uint32_t last_sample_ms;
    bool have_last;

    // When the crank count last actually advanced, by our clock. This is what
    // the idle timeout is measured from -- not the last notification, which
    // keeps arriving while the rider coasts.
    uint32_t last_motion_ms;
    bool have_motion;

    uint16_t rpm;
} CadenceTracker_t;

// Forgets everything. Call on a fresh connection: the counters of a sensor
// that has just been reconnected have no relationship to the ones before it.
void CadenceTracker_Reset(CadenceTracker_t *t);

// Folds one decoded measurement in, with the time it arrived by the board's
// own clock.
//
// Returns true when *out_rpm has been written, which is not every call: the
// first sample of a connection establishes a baseline and says nothing, and a
// repeat of the same crank event carries no new information either. Sensors
// notify at a fixed rate regardless of pedalling, so repeats are the common
// case and not a fault.
bool CadenceTracker_Update(CadenceTracker_t *t, const CscMeasurement_t *m, uint32_t now_ms,
                           uint16_t *out_rpm);

// Applies the idle timeout without a new packet, so a rider who stops pedalling
// reaches zero even though the sensor has stopped saying anything new. Returns
// true if the rpm changed.
bool CadenceTracker_Tick(CadenceTracker_t *t, uint32_t now_ms, uint16_t *out_rpm);

#ifdef __cplusplus
}
#endif
