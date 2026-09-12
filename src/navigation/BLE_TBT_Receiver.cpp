#include "BLE_TBT_Receiver.h"

#include "NavRoute.h"

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

        TBT_Directive_t directive;
        if (!TBT_ParseFrame(value.data(), value.length(), &directive.icon_id,
                            &directive.distance_m, directive.street_name,
                            sizeof(directive.street_name))) {
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
    s_server->setCallbacks(&s_server_callbacks);

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
