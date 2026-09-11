#include "BLE_TBT_Receiver.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <string.h>

namespace {

NimBLEServer *s_server = nullptr;
volatile bool s_connected = false;

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *server, NimBLEConnInfo &conn_info) override {
        (void)server;
        (void)conn_info;
        s_connected = true;
    }

    void onDisconnect(NimBLEServer *server, NimBLEConnInfo &conn_info, int reason) override {
        (void)server;
        (void)conn_info;
        (void)reason;
        s_connected = false;

        // Publish an empty directive so the dashboard drops back to "NO
        // ROUTE" instead of holding the last turn forever after the phone
        // walks away.
        TBT_Directive_t cleared;
        memset(&cleared, 0, sizeof(cleared));
        cleared.icon_id = TBT_ICON_NONE;
        DataCenter_Publish(TOPIC_NAV_TBT, &cleared);

        // Nothing re-advertises on its own once a peer drops.
        NimBLEDevice::startAdvertising();
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

        // Runs on NimBLE's host task (Core 0), which is exactly the pattern
        // DataCenter exists for -- the dashboard consumes it on Core 1.
        DataCenter_Publish(TOPIC_NAV_TBT, &directive);
    }
};

// Callback objects outlive the characteristic; NimBLE keeps raw pointers.
ServerCallbacks s_server_callbacks;
TbtCallbacks s_characteristic_callbacks;

const char *s_start_result = "not started";
volatile uint32_t s_restart_count = 0;

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

// Re-asserts advertising if it is ever found stopped.
//
// The phone can only report the absence of the device, so an advertisement
// that quietly stops looks identical to a phone that never scanned. Several
// things can stop it: the controller drops it when a connection is
// established, a GATT reset takes it down on purpose, and BLE_HR_Start() runs
// a scan and a connection right after this module starts advertising.
//
// Rather than reason about which of those applies on any given boot, check.
// Five seconds is far below the time it takes a rider to notice a missing turn
// prompt, and the check is two reads when nothing is wrong.
void TbtSupervisorTask(void *pv) {
    (void)pv;

    // Only written when they change, so this costs no flash wear while the
    // steady state holds. The boot-time trace above is a snapshot taken before
    // BLE_HR_Start() runs; these are what the radio settles to afterwards,
    // which is the state the phone actually meets.
    uint8_t last_active = 0xFF;
    uint8_t last_restarts = 0xFF;
    uint8_t last_conn = 0xFF;
    uint8_t last_ncon = 0xFF;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        const uint8_t active = ble_gap_adv_active() ? 1 : 0;
        if (active != last_active) {
            last_active = active;
            NoteTbtStep("tbt_live", active);
        }

        const uint8_t restarts = (uint8_t)(s_restart_count > 254 ? 254 : s_restart_count);
        if (restarts != last_restarts) {
            last_restarts = restarts;
            NoteTbtStep("tbt_rst", restarts);
        }

        const uint8_t conn = s_connected ? 1 : 0;
        if (conn != last_conn) {
            last_conn = conn;
            NoteTbtStep("tbt_conn", conn);
        }
        const uint8_t ncon = (s_server != nullptr) ? s_server->getConnectedCount() : 0;
        if (ncon != last_ncon) {
            last_ncon = ncon;
            NoteTbtStep("tbt_ncon", ncon);
        }

        // Re-advertise whenever the controller is not advertising, connected
        // or not.
        //
        // This used to skip while a peer was connected, on the assumption that
        // a connection is a legitimate reason to go quiet. That is true for a
        // peripheral serving one peer, and wrong for this device: the phone
        // has to be able to FIND the head unit at any moment, including while
        // the watch is connected. NimBLE allows up to
        // CONFIG_BT_NIMBLE_MAX_CONNECTIONS (3) links, so going quiet after the
        // first one throws away the other two and makes the head unit
        // invisible for the rest of the ride.
        //
        // It also silently disabled this whole supervisor: advertising stopped,
        // the guard above matched, and the restart never ran.
        if (!ble_gap_adv_active()) {
            s_restart_count++;
            NimBLEDevice::startAdvertising();
        }
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

    service->start();

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

    const bool adv_ok = server_ok && NimBLEDevice::startAdvertising();
    NoteTbtStep("tbt_adv", adv_ok ? 1 : 0);

    // Ground truth from the controller, not from our own bookkeeping: whether
    // the radio is actually emitting. A true here with an empty scan on the
    // Mac would mean the payload is the problem, not the start.
    NoteTbtStep("tbt_act", ble_gap_adv_active() ? 1 : 0);

    const bool ok = adv_ok;
    s_start_result = ok ? "started" : (server_ok ? "adv REFUSED" : "GATT REFUSED");

    if (!ok) {
        // Same reasoning as the sync path: do not leave a task retrying a
        // GATT registration that may become unsafe once the HR client
        // connects.
        return;
    }

    // Kept alive for the life of the device, so it is created once here rather
    // than from a page that can be unloaded. Core 0, with the other radio
    // work (CLAUDE.md section 4); 2KB is ample for two calls and no locals.
    xTaskCreatePinnedToCore(TbtSupervisorTask, "tbt_adv", 2048, nullptr, 1, nullptr, 0);
}

bool BLE_TBT_IsConnected() {
    return s_connected;
}

bool BLE_TBT_IsAdvertising() {
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    return advertising != nullptr && advertising->isAdvertising();
}

const char *BLE_TBT_StartResultText() {
    return s_start_result;
}

uint32_t BLE_TBT_RestartCount() {
    return s_restart_count;
}
