#pragma once

#include <stddef.h>
#include <stdint.h>

// For ALERT_NAME_MAX, which bounds Alert_Info_t below.
#include "AlertFrame.h"

// Cross-core Pub/Sub data bus, adapted from deps/X-TRACK's DataCenter/Account
// design (see deps/X-TRACK/.../Utils/DataCenter/). X-TRACK's original runs on
// a single Cortex-M4 core and gets its thread-safety for free from LVGL's
// cooperative, single-threaded lv_timer_handler() loop -- it has no locking
// of its own (see Account.cpp: no mutex anywhere). Project Pegasus is genuinely
// dual-core (CLAUDE.md §4: Core 0 sensor tasks publish, Core 1's LVGL task
// consumes), so this port adds a real FreeRTOS mutex around every access.
//
// Unlike X-TRACK's Account (one heavyweight object per publisher, owning its
// own ping-pong buffer + LVGL timer), topics here are pre-registered with a
// known struct size and live in one shared table -- simpler, and matches the
// exact DataCenter_Subscribe/DataCenter_Publish(topic, data) shape the task
// calls for. Account is kept as a lightweight subscriber handle (id + an
// optional push callback), not a publisher-owned object.

// ---- Shared data structures (published under the topic names below) ----

typedef struct {
    // Set from UBX NAV-PVT's gnssFixOK flag AND its fixType. Consumers must
    // check this rather than inferring a fix from the coordinates: before a
    // fix, lat/lon are legitimately 0,0, which is a real place in the Gulf of
    // Guinea and indistinguishable from "no data" by value alone.
    bool fix_valid;
    uint8_t num_sv;   // satellites used in the solution

    // True when this came from the MAX-M10S, false when the phone supplied it
    // over BLE (src/sensors/GpsFrame.h).
    //
    // Not cosmetic: the arbitration between the two reads it. Both publish to
    // this topic, so without it the head unit cannot tell its own receiver's
    // first fix from a fix it republished on the phone's behalf -- and would
    // conclude a module exists the moment the phone sent one.
    //
    // Consumers that only want a position can ignore it. It matters to whoever
    // decides WHICH position to believe.
    bool from_module;

    double lat;       // degrees
    double lon;       // degrees
    float speed;      // m/s
    float alt;        // meters above mean sea level
    float heading;    // degrees, 0-360

    // UTC, straight off the receiver. time_valid additionally requires the
    // receiver to report the time fully resolved, not merely present.
    bool time_valid;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} GPS_Info_t;

typedef struct {
    uint8_t bpm;
    uint8_t battery; // percent, 0-100 (0xFF = unknown; a 0x180D peer reports battery elsewhere)
} HeartRate_t;

// Crank cadence, already reduced to revolutions per minute.
//
// The wire carries two free-running 16-bit counters rather than a rate, and
// turning those into an rpm needs history, a wrap-safe subtraction and an idle
// timeout -- all of which live in src/sensors/BleCscParse.h, host-tested,
// beside the decoder. What reaches this bus is the answer, so every consumer
// gets the same one.
//
// Zero is a real reading and means the crank is not turning. "No sensor" and
// "sensor gone quiet" are absences instead, and the dashboard ages the reading
// out the way it does heart rate.
typedef struct {
    uint16_t rpm;
} Cadence_t;

typedef struct {
    float pitch;           // degrees
    bool motion_detected;  // Any-Motion wake trigger state
} IMU_Data_t;

typedef struct {
    uint16_t millivolts; // at the battery, i.e. already past the 2:1 divider
    uint8_t percent;     // 0-100, from the LiPo discharge curve
    bool on_usb;         // true when the rail reads above any real LiPo, i.e. USB-powered
} Battery_t;

// Turn-by-turn maneuver icons. Values are wire constants -- the phone app
// sends these numerically, so append new ones rather than renumbering.
typedef enum {
    TBT_ICON_NONE = 0, // no active route
    TBT_ICON_STRAIGHT = 1,
    TBT_ICON_TURN_LEFT = 2,
    TBT_ICON_TURN_RIGHT = 3,
    TBT_ICON_SLIGHT_LEFT = 4,
    TBT_ICON_SLIGHT_RIGHT = 5,
    TBT_ICON_SHARP_LEFT = 6,
    TBT_ICON_SHARP_RIGHT = 7,
    TBT_ICON_UTURN = 8,
    TBT_ICON_ROUNDABOUT = 9,
    TBT_ICON_ARRIVE = 10,
    _TBT_ICON_LAST = TBT_ICON_ARRIVE,
} TBT_Icon_t;

#define TBT_STREET_NAME_MAX 32 // 31 UTF-8 bytes + NUL

// Where a directive came from. The rider needs to know, because the two are
// not equally trustworthy: the phone has map matching and live rerouting, the
// head unit has a cached polyline and its own GPS. A countdown that is being
// computed on board rather than received should say so.
typedef enum {
    TBT_SOURCE_PHONE = 0,    // live over BLE; also the value a zeroed struct has
    TBT_SOURCE_ONBOARD = 1,  // computed from the cached route and our own fix
} TBT_Source_t;

// One turn-by-turn directive pushed from the phone (CLAUDE.md §5).
// Declared here beside the other topic payloads rather than in
// BLE_TBT_Receiver.h, so DataCenter's topic table can size it without the bus
// depending on a navigation module, and so any page can consume it by
// including this header alone.
typedef struct {
    uint8_t icon_id;                       // a TBT_Icon_t value
    uint32_t distance_m;                   // metres to the maneuver
    char street_name[TBT_STREET_NAME_MAX]; // NUL-terminated, may be empty

    // Appended, not inserted: TBT_SOURCE_PHONE is 0, so every existing
    // publisher that zeroes this struct keeps its old meaning untouched.
    uint8_t source;      // a TBT_Source_t value
    bool off_route;      // onboard only: the fix is not near the cached route

    // Which exit to take, 1..9, or 0 when the maneuver is not a roundabout or
    // nobody said. Every roundabout shares one arrow, so this is the only
    // thing that distinguishes them.
    uint8_t exit_number;

    // The maneuver AFTER the one above, and the gap between the two. Closely
    // spaced junctions are where a rider goes wrong, and an arrow that only
    // ever shows the next one gives no warning that another follows it
    // immediately.
    //
    // TBT_ICON_NONE means "not known", which is also what a zeroed struct
    // says, so a publisher that does not fill these is reporting honestly.
    // Only the cached route can supply them: the phone sends one turn at a
    // time and the wire format has no room for two.
    uint8_t then_icon_id;
    uint32_t then_distance_m;

    // Metres still to ride to the destination, or TBT_DISTANCE_UNKNOWN.
    //
    // Explicitly unknown rather than zero, because zero is a legitimate value
    // meaning "arrived" and a zeroed struct must not claim that.
    uint32_t remaining_m;
} TBT_Directive_t;

// ---- One interruption from the phone: a call, a text, a chat message ----
//
// Carries WHO and nothing else -- no message body. The reasoning is in
// src/system/AlertFrame.h, which also defines the wire format this is filled
// from. Included rather than re-declared so the name length has exactly one
// definition: this struct is the thing the panel draws, and a bound that
// disagreed with the parser's would be a truncation nobody sees until a long
// name arrives.
typedef struct {
    // Increments once per alert delivered, starting at 1. A zeroed struct
    // therefore says "nothing has ever arrived", and the overlay can tell a
    // fresh alert from the same one still sitting in the topic buffer --
    // DataCenter_Pull always succeeds once a topic has data, so without this
    // a poller has no way to ask "is this new?".
    uint32_t seq;

    uint8_t kind;  // an AlertKind_t value
    uint8_t count; // messages coalesced into this one alert, >= 1
    char name[ALERT_NAME_MAX + 1]; // NUL-terminated; may be empty
} Alert_Info_t;

// Well-known topic names (CLAUDE.md §4 examples: "Sensor/HeartRate", "GPS_Info").
// Add new topics by extending the registration table in DataCenter.cpp.
extern const char *const TOPIC_GPS_INFO;
extern const char *const TOPIC_HEART_RATE;
extern const char *const TOPIC_CADENCE;
extern const char *const TOPIC_IMU_DATA;
extern const char *const TOPIC_BATTERY;
extern const char *const TOPIC_NAV_TBT;
extern const char *const TOPIC_PHONE_ALERT;

// Called by a subscriber on every DataCenter_Publish() to that topic.
//
// `data` points AT the topic's own buffer -- it is not a copy, whatever this
// comment used to claim. It is valid only for the duration of the call, which
// is safe because DataCenter_Publish() holds the bus mutex across every
// callback, and it is emphatically not safe to store. A callback that wants
// the value afterwards must copy it out, or call DataCenter_Pull() later.
//
// Keep this short and non-blocking: it runs synchronously inside
// DataCenter_Publish(), on the publisher's own task/core, with that mutex
// held. Anything slow belongs behind a flag the owning task picks up.
typedef void (*DataCenter_Callback_t)(const char *topic, const void *data, uint32_t size, void *user_arg);

// A subscriber's handle on the bus. One Account per subscriber (not per
// topic) -- Subscribe() to as many topics as needed with the same Account.
class Account {
public:
    explicit Account(const char *id, DataCenter_Callback_t callback = nullptr, void *user_arg = nullptr);

    const char *const ID;
    const DataCenter_Callback_t Callback; // nullptr => polling-only subscriber (use DataCenter_Pull)
    void *const UserArg;
};

// Must be called once (e.g. from setup()) before any Subscribe/Publish call.
void DataCenter_Init();

// Subscribes `account` to `topic`. `topic` must be one of the well-known
// TOPIC_* names above (or a topic added to the registration table).
// Returns false if the topic is unknown or `account` is already subscribed.
bool DataCenter_Subscribe(const char *topic, Account *account);

// Removes `account` from `topic`'s subscriber list. Returns false if either
// the topic is unknown or `account` wasn't subscribed.
bool DataCenter_Unsubscribe(const char *topic, Account *account);

// Copies sizeof(<topic's registered type>) bytes from `data` into the topic's
// cache and synchronously notifies every subscribed Account's callback.
// `data` must point to a live instance of that topic's registered struct
// (e.g. GPS_Info_t for TOPIC_GPS_INFO). Returns false if the topic is unknown.
bool DataCenter_Publish(const char *topic, void *data);

// Polling read of the topic's last-published value (for late subscribers, or
// consumers that don't want a push callback, e.g. UI redraw on a timer).
// `out_size` must equal the topic's registered struct size. Returns false if
// the topic is unknown, out_size mismatches, or nothing has been published
// to it yet.
bool DataCenter_Pull(const char *topic, void *out_data, uint32_t out_size);
