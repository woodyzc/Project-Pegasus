#include "BLE_TBT_Receiver.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
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
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        // A connected phone is the one legitimate reason not to advertise:
        // NimBLE stops on connect and our onDisconnect restarts it.
        if (s_connected) {
            continue;
        }

        NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
        if (advertising != nullptr && !advertising->isAdvertising()) {
            s_restart_count++;
            NimBLEDevice::startAdvertising();
        }
    }
}

} // namespace

void BLE_TBT_Start() {
    // Idempotent in NimBLE 2.x, and harmless when BLE_HR_Init() already ran.
    NimBLEDevice::init("pegasus");

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
    const bool ok = NimBLEDevice::startAdvertising();
    s_start_result = ok ? "started" : "REFUSED";

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
