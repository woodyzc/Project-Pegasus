#include "BLE_HR_Client.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "../system/DataCenter.h"
#include "BleHrParse.h"

namespace {

// Standard GATT assigned numbers.
const NimBLEUUID kHrService((uint16_t)0x180D);      // Heart Rate Service
const NimBLEUUID kHrMeasurement((uint16_t)0x2A37);  // Heart Rate Measurement

constexpr uint32_t kDiscoveryScanMs = 15000;
constexpr uint32_t kBackoffStartMs = 1000;   // 1s, then 2s, 4s, 8s ...
constexpr uint32_t kBackoffMaxMs = 32000;    // ... capped, so a strap left at home
                                             // doesn't retry forever at 1Hz
constexpr uint32_t kSuperviseIdleMs = 1000;

constexpr uint32_t kTaskStackSize = 4096;
constexpr UBaseType_t kTaskPriority = 3;
constexpr BaseType_t kTaskCore = 0; // Core 0: Background Data Core (CLAUDE.md §4)

NimBLEClient *s_client = nullptr;
NimBLEAddress s_peer_address;
bool s_have_peer = false;
volatile bool s_connected = false;

bool (*s_primary_active)() = nullptr;

class ClientCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient *client, int reason) override {
        (void)client;
        s_connected = false;
        Serial.printf("[BLE_HR] disconnected (reason %d)\n", reason);
    }
};
ClientCallbacks s_client_callbacks;

// Scans for a peer advertising 0x180D and remembers its address. Only needed
// once: later reconnects dial s_peer_address directly (see the header's note
// on ANT+ coexistence consuming the scan).
bool DiscoverPeer() {
    Serial.println("[BLE_HR] scanning for a 0x180D peripheral...");
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    NimBLEScanResults results = scan->getResults(kDiscoveryScanMs, false);

    for (int i = 0; i < results.getCount(); i++) {
        const NimBLEAdvertisedDevice *device = results.getDevice(i);
        if (device != nullptr && device->isAdvertisingService(kHrService)) {
            s_peer_address = device->getAddress();
            s_have_peer = true;
            Serial.printf("[BLE_HR] found %s '%s' rssi %d\n", s_peer_address.toString().c_str(),
                          device->getName().c_str(), device->getRSSI());
            return true;
        }
    }
    Serial.println("[BLE_HR] no 0x180D peripheral found");
    return false;
}

// Connects to the remembered peer and subscribes to 0x2A37. No scan involved,
// so this still works while ANT+ owns the scan windows.
bool ConnectAndSubscribe() {
    if (!s_have_peer) {
        return false;
    }

    if (s_client == nullptr) {
        s_client = NimBLEDevice::createClient();
        if (s_client == nullptr) {
            Serial.println("[BLE_HR] createClient failed");
            return false;
        }
        s_client->setClientCallbacks(&s_client_callbacks, false);
    }

    // Dial the stored address rather than an advertised-device object, so no
    // scan results are required.
    if (!s_client->connect(s_peer_address)) {
        return false;
    }

    NimBLERemoteService *service = s_client->getService(kHrService);
    NimBLERemoteCharacteristic *measurement =
        (service != nullptr) ? service->getCharacteristic(kHrMeasurement) : nullptr;

    if (measurement == nullptr || !measurement->canNotify() ||
        !measurement->subscribe(true, OnNotifyCallback)) {
        Serial.println("[BLE_HR] 0x2A37 subscribe failed");
        s_client->disconnect();
        return false;
    }

    s_connected = true;
    Serial.printf("[BLE_HR] connected to %s, subscribed to 0x2A37\n",
                  s_peer_address.toString().c_str());
    return true;
}

// Core 0 supervisor: keeps the link up with exponential backoff after the
// initial connect. The notifications themselves arrive on NimBLE's own host
// task, not here.
//
// Note this never starts a scan: initial discovery is done synchronously in
// BLE_HR_Start() before ANT can claim the scan windows, and every reconnect
// from here dials the stored address instead.
void BleHrTask(void *pvParameters) {
    (void)pvParameters;

    uint32_t backoff_ms = kBackoffStartMs;

    for (;;) {
        if (s_connected) {
            backoff_ms = kBackoffStartMs; // healthy link resets the backoff
            vTaskDelay(pdMS_TO_TICKS(kSuperviseIdleMs));
            continue;
        }

        if (!s_have_peer) {
            // Discovery never succeeded and we cannot rescan safely once ANT
            // may be riding the scan windows. Idle rather than spin; a reboot
            // (or a future explicit re-pair entry point) is what recovers this.
            vTaskDelay(pdMS_TO_TICKS(kBackoffMaxMs));
            continue;
        }

        if (ConnectAndSubscribe()) {
            continue;
        }

        Serial.printf("[BLE_HR] connect failed, retrying in %lums\n", (unsigned long)backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms = (backoff_ms >= kBackoffMaxMs) ? kBackoffMaxMs : (backoff_ms * 2);
    }
}

} // namespace

void OnNotifyCallback(NimBLERemoteCharacteristic *characteristic, uint8_t *data, size_t length,
                      bool is_notify) {
    (void)characteristic;
    (void)is_notify;

    // Flags/width/length handling lives in BleHrParse.c so it can be covered
    // by the host tests (test/host/test_ble_hr_parse.c).
    uint16_t bpm16;
    if (!BLE_HR_ParseMeasurement(data, length, &bpm16)) {
        return;
    }

    // ANT+ is the primary source (CLAUDE.md §3). While it is tracking, stay
    // connected but leave the topic alone -- see BLE_HR_SetPrimaryActiveHook.
    if (s_primary_active != nullptr && s_primary_active()) {
        return;
    }

    HeartRate_t hr;
    hr.bpm = (uint8_t)bpm16;
    // The strap's own battery level lives in the separate Battery Service
    // (0x180F / 0x2A19), not in this characteristic, so it stays "unknown"
    // per DataCenter.h's convention. Reading 0x180F on connect would be a
    // small, self-contained follow-up.
    hr.battery = 0xFF;
    DataCenter_Publish(TOPIC_HEART_RATE, &hr);
}

void BLE_HR_Init() {
    NimBLEDevice::init("pegasus");

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
}

void BLE_HR_Start() {
    // Discovery runs synchronously (blocking for up to kDiscoveryScanMs), the
    // way deps/esp32-ant's verified coexist example sequences it. It has to
    // finish before BLE_HR_StartCoexistScan()/SoftANT_Start() touch the scan,
    // otherwise the perpetual passive scan would be reconfigured underneath an
    // in-flight active scan.
    if (DiscoverPeer()) {
        ConnectAndSubscribe();
    }

    xTaskCreatePinnedToCore(BleHrTask, "BLE_HR", kTaskStackSize, nullptr, kTaskPriority, nullptr,
                            kTaskCore);
}

void BLE_HR_StartCoexistScan() {
    // Passive, perpetual: deps/esp32-ant's coexist mode retunes these scan
    // windows to receive ANT frames, so one must be running the whole time ANT
    // is open.
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(false);
    scan->start(0, false, true);
}

bool BLE_HR_IsConnected() {
    return s_connected;
}

void BLE_HR_SetPrimaryActiveHook(bool (*is_primary_active)()) {
    s_primary_active = is_primary_active;
}
