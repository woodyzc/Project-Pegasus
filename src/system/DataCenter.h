#pragma once

#include <stdint.h>
#include <stddef.h>

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
    double lat;      // degrees
    double lon;       // degrees
    float speed;      // m/s
    float alt;        // meters
    float heading;    // degrees, 0-360
} GPS_Info_t;

typedef struct {
    uint8_t bpm;
    uint8_t battery; // percent, 0-100 (0xFF = unknown, e.g. ANT+ straps don't report battery)
} HeartRate_t;

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

// One turn-by-turn directive pushed from the phone (CLAUDE.md §5).
// Declared here beside the other topic payloads rather than in
// BLE_TBT_Receiver.h, so DataCenter's topic table can size it without the bus
// depending on a navigation module, and so any page can consume it by
// including this header alone.
typedef struct {
    uint8_t icon_id;                       // a TBT_Icon_t value
    uint32_t distance_m;                   // metres to the maneuver
    char street_name[TBT_STREET_NAME_MAX]; // NUL-terminated, may be empty
} TBT_Directive_t;

// Well-known topic names (CLAUDE.md §4 examples: "Sensor/HeartRate", "GPS_Info").
// Add new topics by extending the registration table in DataCenter.cpp.
extern const char *const TOPIC_GPS_INFO;
extern const char *const TOPIC_HEART_RATE;
extern const char *const TOPIC_IMU_DATA;
extern const char *const TOPIC_BATTERY;
extern const char *const TOPIC_NAV_TBT;

// Called by a subscriber on every DataCenter_Publish() to that topic, with a
// fresh copy of the published data (NOT a pointer into DataCenter's internal
// buffer -- safe to read after the call returns, no locking needed by the
// callback). Keep this short and non-blocking: it runs synchronously inside
// DataCenter_Publish(), on the publisher's own task/core.
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
