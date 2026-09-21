#include "BLE_TBT_Receiver.h"

#include "../sensors/GpsFrame.h"
#include "NavRoute.h"

#include "../system/AlertFrame.h"
#include "../system/ClockFrame.h"
#include "../system/TimeSource.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <string.h>

namespace {

NimBLEServer *s_server = nullptr;
volatile bool s_connected = false;

// Cached, not queried.
//
// Every NimBLE GAP call in this file now happens either during BLE_TBT_Start()
// or inside a NimBLEServerCallbacks method, both of which run on NimBLE's own
// host task. Nothing else may touch the stack: the Settings page polls this
// once a second from the LVGL task, and a 5s supervisor task used to poll
// ble_gap_adv_active() and re-advertise from Core 0.
//
// That supervisor is gone, and the crash it was implicated in is why:
//
//     assert failed: ble_hs_timer_exp ble_hs.c:466 (0)
//     "The timer should not be set in this state"  (BLE_HS_SYNC_STATE_BRINGUP)
//
// The host had reset and was mid-bringup when its own timer fired. A foreign
// task calling into GAP across that window is the kind of thing that produces
// it, and the supervisor was the only such caller. Its job -- re-advertise
// after a peer connects -- belongs in onConnect anyway, where it runs on the
// right task and at the exact moment it is needed rather than up to 5s later.
volatile bool s_advertising = false;

// How many times advertising has been re-asserted after a peer connected or
// dropped. Non-zero is normal and expected -- it is the mechanism working.
volatile uint32_t s_restart_count = 0;

// True only while BLE_TBT_PauseAdvertising() is holding the radio down, so the
// resume cannot fight a phone that connected in the meantime.
volatile bool s_paused = false;

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *server, NimBLEConnInfo &conn_info) override {
        (void)server;
        (void)conn_info;
        s_connected = true;

        // Keep advertising. A connectable advertisement ends the moment a peer
        // connects, and this device has to stay findable after that: the watch
        // and the phone arrive independently and in any order, and NimBLE
        // allows three links. Going quiet after the first one hides the head
        // unit for the rest of the ride.
        //
        // This runs on NimBLE's host task, which is the whole point -- see the
        // note on s_advertising below.
        s_advertising = NimBLEDevice::startAdvertising();
        s_restart_count++;
    }

    void onDisconnect(NimBLEServer *server, NimBLEConnInfo &conn_info, int reason) override {
        (void)server;
        (void)conn_info;
        (void)reason;
        s_connected = false;

        // With a route cached, a disconnect is a handover rather than an end:
        // NavRoute navigates from its own GPS from the next tick. Clearing
        // here would blank the panel for a second and then refill it, which
        // reads as a fault rather than as a mode change.
        NavRoute_NotePhoneGone();

        if (!NavRoute_IsLoaded()) {
            // No cached route, so there is genuinely nothing left to show.
            // Publish an empty directive so the dashboard drops back to "NO
            // ROUTE" instead of holding the last turn forever after the phone
            // walks away.
            TBT_Directive_t cleared;
            memset(&cleared, 0, sizeof(cleared));
            cleared.icon_id = TBT_ICON_NONE;
            DataCenter_Publish(TOPIC_NAV_TBT, &cleared);
        }

        // Nothing re-advertises on its own once a peer drops.
        s_advertising = NimBLEDevice::startAdvertising();
    }
};

class TbtCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn_info) override {
        (void)conn_info;

        NimBLEAttValue value = characteristic->getValue();

        // Zeroed, and this is not defensive habit -- off_route lives in this
        // struct and NOTHING on the phone path ever writes it. TBT_ParseFrame
        // fills the four wire fields, source is set below, and
        // NavRoute_EnrichDirective fills its three; off_route was left as
        // whatever was on the stack. The dashboard colours the arrow red when
        // it is set, so a live phone turn flickered between red and cyan at
        // random while the countdown underneath it was perfectly correct.
        //
        // The struct's own comment in DataCenter.h says "every existing
        // publisher that zeroes this struct" -- this one did not.
        TBT_Directive_t directive;
        memset(&directive, 0, sizeof(directive));

        if (!TBT_ParseFrame(value.data(), value.length(), &directive.icon_id,
                            &directive.distance_m, directive.street_name,
                            sizeof(directive.street_name), &directive.exit_number)) {
            // Malformed frames are dropped silently rather than partially
            // applied: showing a turn the phone never sent is worse than
            // showing nothing.
            return;
        }

        // The phone is demonstrably talking. NavRoute's fallback waits on
        // this rather than on the connection state, so that a link which is up
        // but silent still hands navigation back to the head unit.
        NavRoute_NoteLiveDirective();
        directive.source = TBT_SOURCE_PHONE;

        // The phone sends one turn per frame and has no room for a second, so
        // the cached route fills in what follows this turn and how far is
        // left. Leaves them unknown when no route has been uploaded, which is
        // the case whenever the rider is navigating live without one.
        NavRoute_EnrichDirective(&directive);

        // Runs on NimBLE's host task (Core 0), which is exactly the pattern
        // DataCenter exists for -- the dashboard consumes it on Core 1.
        DataCenter_Publish(TOPIC_NAV_TBT, &directive);
    }
};

NimBLECharacteristic *s_status_characteristic = nullptr;

// Publishes the transfer progress on the status characteristic, so the phone
// can see which chunks landed without the head unit needing a reverse channel
// of its own. Four bytes: received u16, total u16, little-endian.
void NotifyRouteProgress() {
    if (s_status_characteristic == nullptr) {
        return;
    }
    uint16_t received = 0;
    uint16_t total = 0;
    NavRoute_Progress(&received, &total);

    uint8_t payload[4];
    payload[0] = (uint8_t)(received & 0xFF);
    payload[1] = (uint8_t)((received >> 8) & 0xFF);
    payload[2] = (uint8_t)(total & 0xFF);
    payload[3] = (uint8_t)((total >> 8) & 0xFF);

    s_status_characteristic->setValue(payload, sizeof(payload));
    s_status_characteristic->notify();
}

class ClockCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn_info) override {
        (void)conn_info;
        NimBLEAttValue value = characteristic->getValue();

        uint32_t utc_seconds = 0;
        int16_t offset_min = 0;
        char zone[CLOCK_ZONE_LEN + 1] = {0};

        if (!Clock_ParseFrame(value.data(), value.length(), &utc_seconds, &offset_min, zone,
                              sizeof(zone))) {
            // Dropped, like a malformed turn: a clock that is wrong is worse
            // than one that is blank, because it names the ride file and
            // stamps every trackpoint in it. Counted rather than silent,
            // because a rejected frame and an absent one look the same on the
            // panel and need opposite fixes.
            TimeSource_NotePhoneRejected();
            return;
        }

        // Ranked against whatever else has spoken, not applied outright --
        // see TimeSource.h. A fix outranks this for the instant; nothing
        // outranks it for the zone.
        TimeSource_SetFromPhone(utc_seconds, offset_min, zone, millis());
    }
};

class RouteCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn_info) override {
        (void)conn_info;
        NimBLEAttValue value = characteristic->getValue();

        // The chunk is validated and placed by NavRoute; a rejected one is
        // simply not acknowledged in the progress count, which is how the
        // phone learns to resend it.
        NavRoute_AcceptChunk(value.data(), value.length());
        NotifyRouteProgress();
    }
};

// Callback objects outlive the characteristic; NimBLE keeps raw pointers.
ServerCallbacks s_server_callbacks;
TbtCallbacks s_characteristic_callbacks;
RouteCallbacks s_route_callbacks;
ClockCallbacks s_clock_callbacks;

// ---- Has the real receiver ever produced a fix? ----
//
// GPS_Reader publishes on every NAV-PVT whether or not it has a fix, so
// "something published" does not mean "a receiver is working". What settles it
// is a fix that was actually valid, and once one has arrived the module is
// demonstrably present and the phone stands aside permanently.
//
// Permanently, rather than for a grace window like NavRoute's turn handover,
// and the asymmetry is deliberate. A turn going stale is the phone falling
// quiet, which is ordinary and reversible. A receiver that produced a fix and
// then stopped is riding into a tunnel, and there the module's own "no fix"
// is the truth -- taking the phone's position instead would paper over the
// outage with a number from a device in a pocket.
volatile bool s_module_ever_fixed = false;

void OnGpsPublished(const char *topic, const void *data, uint32_t size, void *user_arg) {
    (void)topic;
    (void)user_arg;
    if (data == nullptr || size != sizeof(GPS_Info_t)) {
        return;
    }
    const GPS_Info_t *gps = (const GPS_Info_t *)data;
    // Only a fix from the module counts. Our own republished phone fixes come
    // through this same topic, so without the source check the first phone fix
    // would convince us a module exists and silence the phone for ever.
    if (gps->fix_valid && gps->from_module) {
        s_module_ever_fixed = true;
    }
}

Account s_gps_account("TBT/GpsArb", OnGpsPublished);

class GpsCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn_info) override {
        (void)conn_info;
        NimBLEAttValue value = characteristic->getValue();

        GpsFrame_t frame;
        if (!Gps_ParseFrame(value.data(), value.length(), &frame)) {
            return;
        }

        // The module has the last word once it has ever had a fix.
        if (s_module_ever_fixed) {
            return;
        }

        GPS_Info_t info;
        memset(&info, 0, sizeof(info));
        info.from_module = false;
        info.fix_valid = frame.fix_valid;
        info.num_sv = frame.num_sv;
        info.lat = frame.lat;
        info.lon = frame.lon;
        info.speed = frame.speed;
        info.alt = frame.alt;
        info.heading = frame.heading;
        info.time_valid = frame.time_valid;
        info.year = frame.year;
        info.month = frame.month;
        info.day = frame.day;
        info.hour = frame.hour;
        info.minute = frame.minute;
        info.second = frame.second;

        // Published without a fix as well, for the same reason GPS_Reader does
        // it: the panel needs to tell "the phone is here and acquiring" from
        // "nothing is feeding us a position at all".
        DataCenter_Publish(TOPIC_GPS_INFO, &info);
    }
};

GpsCallbacks s_gps_callbacks;


// ---- Phone alerts ----
//
// No arbitration and no state: unlike position, nothing else on this device
// can produce a call. The one thing worth keeping is a counter, because
// DataCenter_Pull hands back the last published value forever and the overlay
// would otherwise have no way to tell a new alert from the one it already
// showed.
uint32_t s_alert_seq = 0;

class AlertCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn_info) override {
        (void)conn_info;
        NimBLEAttValue value = characteristic->getValue();

        AlertFrame_t frame;
        if (!Alert_ParseFrame(value.data(), value.length(), &frame)) {
            return;
        }

        Alert_Info_t info;
        memset(&info, 0, sizeof(info));
        // Pre-incremented, so the first alert of a boot is seq 1 and a zeroed
        // struct keeps meaning "nothing has arrived". Wrapping after four
        // billion alerts lands on 0 for exactly one of them, which costs that
        // single banner and nothing else.
        info.seq = ++s_alert_seq;
        info.kind = (uint8_t)frame.kind;
        info.count = frame.count;
        // Bounded by the parser, which is why this is a plain copy: name is
        // NUL-terminated within ALERT_NAME_MAX + 1 or Alert_ParseFrame refused
        // the frame above.
        memcpy(info.name, frame.name, sizeof(info.name));

        DataCenter_Publish(TOPIC_PHONE_ALERT, &info);
    }
};

AlertCallbacks s_alert_callbacks;


const char *s_start_result = "not started";

// Bring-up trace, written to NVS so it can be read back with esptool over the
// USB link. Serial cannot carry it (CLAUDE.md section 8) and the panel needs a
// human to read it out; NVS needs neither, which closes the diagnostic loop:
// flash, let it boot, read 0x9000, know exactly which step failed.
// Callers only write on change, but "on change" is unbounded if something
// makes the state flap, and this runs for the life of the device. A budget per
// boot keeps a pathological flap from writing flash forever; 40 is far more
// than a healthy boot needs and still finite.
uint32_t s_note_budget = 40;

void NoteTbtStep(const char *key, uint8_t value) {
    if (s_note_budget == 0) {
        return;
    }
    s_note_budget--;

    Preferences prefs;
    if (prefs.begin("pegasus", false)) {
        prefs.putUChar(key, value);
        prefs.end();
    }
}

} // namespace

void BLE_TBT_Start() {
    // Idempotent in NimBLE 2.x, and harmless when BLE_HR_Init() already ran.
    NimBLEDevice::init("pegasus");

    // ---- Wait for the host to sync before touching GAP or GATT ----
    // NimBLEDevice::init() brings the host up ASYNCHRONOUSLY. Until the
    // controller reports sync, NimBLEAdvertising::start() fails on its very
    // first line ("Host not synced!") and returns false without starting the
    // GATT server or advertising anything.
    //
    // This used to be masked: BLE_HR_Start() ran first and blocked ~15s
    // scanning, so the host was long since synced by the time anything
    // advertised. Moving this call before BLE_HR_Start() to fix the GATT
    // panic removed that accidental wait, and the board went quiet -- stable,
    // but invisible to the phone.
    //
    // So the wait has to be explicit rather than inherited from whatever ran
    // first. 2s is far longer than sync takes and still bounded.
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    while (!ble_hs_synced() && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    NoteTbtStep("tbt_sync", ble_hs_synced() ? 1 : 0);
    if (!ble_hs_synced()) {
        // Deliberately no supervisor task in this path. A later retry would
        // reach resetGATT() after BLE_HR_Start() has connected, and that is
        // precisely the panic this ordering exists to avoid.
        s_start_result = "host never synced";
        return;
    }

    s_server = NimBLEDevice::createServer();
    if (s_server == nullptr) {
        Serial.println("[BLE_TBT] createServer failed");
        return;
    }
    // false: do NOT let the server delete these callbacks.
    //
    // NimBLEServer::setCallbacks defaults that flag to TRUE, and ~NimBLEServer
    // then runs `delete m_pServerCallbacks` on whatever it was given. Every
    // callback object in this file is a file-scope static, so that delete
    // reaches free() with a pointer that is not in any heap, and the panic is
    // an assert deep inside heap_caps_free rather than anything naming BLE:
    //
    //   assert failed: heap_caps_free heap_caps.c:381
    //   (heap != NULL && "free() target pointer is outside heap areas")
    //
    // It only fires on the path that destroys the server, which is Restart on
    // the settings page by way of BLE_HR_Shutdown() and NimBLEDevice::deinit().
    // Nothing in normal running goes near it, which is why the board ran for
    // days before anyone saw it.
    //
    // The characteristics below need no such flag: NimBLECharacteristic takes
    // no ownership and its destructor frees only its descriptors. The two APIs
    // look alike and differ, which is the whole trap -- BLE_HR_Client.cpp got
    // this right for its client callbacks and this line did not.
    s_server->setCallbacks(&s_server_callbacks, false);

    NimBLEService *service = s_server->createService(TBT_SERVICE_UUID);
    if (service == nullptr) {
        Serial.println("[BLE_TBT] createService failed");
        return;
    }

    // WRITE_NR (write without response) as well as WRITE: turn updates arrive
    // often while riding and the phone gains nothing from an ack.
    NimBLECharacteristic *characteristic = service->createCharacteristic(
        TBT_CHARACTERISTIC_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    characteristic->setCallbacks(&s_characteristic_callbacks);

    // WRITE with an ack, unlike the turn characteristic above: a lost route
    // chunk is a permanent hole in the route, not a stale turn that the next
    // write corrects.
    NimBLECharacteristic *route = service->createCharacteristic(
        TBT_ROUTE_CHARACTERISTIC_UUID, NIMBLE_PROPERTY::WRITE);
    route->setCallbacks(&s_route_callbacks);

    s_status_characteristic = service->createCharacteristic(
        TBT_STATUS_CHARACTERISTIC_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    // WRITE_NR as well as WRITE: the phone sends this on a timer and a lost
    // one costs nothing, since the next arrives minutes later and the clock
    // keeps running from its own tick in between.
    NimBLECharacteristic *clock = service->createCharacteristic(
        TBT_CLOCK_CHARACTERISTIC_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    clock->setCallbacks(&s_clock_callbacks);

    // Position from the phone, for a head unit whose own receiver has never
    // been fitted. WRITE_NR as well as WRITE: a fix arrives once a second and
    // is worthless a second later, so an unacknowledged write that occasionally
    // drops one is a better trade than a round trip that delays them all.
    NimBLECharacteristic *gps = service->createCharacteristic(
        TBT_GPS_CHARACTERISTIC_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    gps->setCallbacks(&s_gps_callbacks);
    DataCenter_Subscribe(TOPIC_GPS_INFO, &s_gps_account);

    // Calls, texts and chat messages. WRITE_NR for the same reason as the fix
    // above, with less at stake: a dropped alert is one the rider reads on the
    // phone at the next stop, so paying a round trip per notification to
    // guarantee delivery would buy very little.
    NimBLECharacteristic *alert = service->createCharacteristic(
        TBT_ALERT_CHARACTERISTIC_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    alert->setCallbacks(&s_alert_callbacks);

    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(TBT_SERVICE_UUID);
    advertising->enableScanResponse(true);

    // The phone's ScanFilter matches on TBT_SERVICE_UUID, and a 128-bit UUID
    // costs 18 of the 31 bytes an advertisement gets. Scan response is enabled
    // above so the name has somewhere to go if the two do not fit together --
    // the UUID is the half that must stay in the advertisement, because it is
    // the half being filtered on.
    // Start the GATT server explicitly, here, rather than letting it happen as
    // a side effect inside startAdvertising(). This is the call that registers
    // the services, it is only safe while nothing is connected or scanning,
    // and doing it on purpose at the one safe moment is worth more than the
    // line it saves.
    //
    // It also makes the supervisor below safe: with the server already
    // started, a later startAdvertising() finds m_gattsStarted true and
    // returns early instead of re-entering resetGATT().
    const bool server_ok = s_server->start();
    NoteTbtStep("tbt_srv", server_ok ? 1 : 0);
    s_start_result = server_ok ? "registered" : "GATT REFUSED";
}

void BLE_TBT_StartAdvertising() {
    if (s_server == nullptr) {
        return; // BLE_TBT_Start() bailed; nothing to advertise
    }

    const bool adv_ok = NimBLEDevice::startAdvertising();
    s_advertising = adv_ok;
    NoteTbtStep("tbt_adv", adv_ok ? 1 : 0);

    // Ground truth from the controller rather than our own bookkeeping.
    NoteTbtStep("tbt_act", ble_gap_adv_active() ? 1 : 0);
    s_start_result = adv_ok ? "started" : "adv REFUSED";
}

void BLE_TBT_NoteStackReleased() {
    // NimBLEDevice::deinit(true) destroys the server this module created, and
    // nothing here would otherwise notice: s_server was never cleared, so
    // every null check in this file passed on a dangling pointer and the calls
    // behind them went into a stack that no longer existed.
    //
    // Called by BLE_HR_Client, which owns the deinit because it owns the
    // disconnect that has to precede it.
    s_server = nullptr;
    s_advertising = false;
    s_paused = false;
    s_connected = false;
}

void BLE_TBT_PauseAdvertising() {
    if (s_server == nullptr || !s_advertising) {
        return;
    }
    NimBLEDevice::stopAdvertising();
    s_advertising = false;
    s_paused = true;
}

void BLE_TBT_ResumeAdvertising() {
    // Only resumes what this paused. A phone that connected in the meantime
    // takes the advertisement down through onConnect, which restarts it
    // itself; re-entering here would fight that.
    if (s_server == nullptr || !s_paused) {
        return;
    }
    s_paused = false;
    s_advertising = NimBLEDevice::startAdvertising();
}

bool BLE_TBT_IsConnected() {
    return s_connected;
}

bool BLE_TBT_IsAdvertising() {
    // The cached flag, not ble_gap_adv_active(). This is called once a second
    // from the LVGL task, and the stack is NimBLE's host task's to touch.
    return s_advertising;
}

const char *BLE_TBT_StartResultText() {
    return s_start_result;
}

uint32_t BLE_TBT_RestartCount() {
    return s_restart_count;
}
