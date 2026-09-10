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
    NimBLEDevice::startAdvertising();
}

bool BLE_TBT_IsConnected() {
    return s_connected;
}
