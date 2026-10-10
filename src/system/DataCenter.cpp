#include "DataCenter.h"

#include <Arduino.h>
#include <string.h>

const char *const TOPIC_GPS_INFO = "GPS_Info";
const char *const TOPIC_HEART_RATE = "Sensor/HeartRate";
const char *const TOPIC_CADENCE = "Sensor/Cadence";
const char *const TOPIC_IMU_DATA = "Sensor/IMU";
const char *const TOPIC_BATTERY = "Sensor/Battery";
const char *const TOPIC_NAV_TBT = "Nav/TBT";
const char *const TOPIC_PHONE_ALERT = "Phone/Alert";

Account::Account(const char *id, DataCenter_Callback_t callback, void *user_arg)
    : ID(id), Callback(callback), UserArg(user_arg) {
}

namespace {

// Subscribers one topic can have. GPS_Info is the busiest at ten; Subscribe
// refuses past this rather than growing, so a topic's list never allocates.
constexpr size_t kMaxSubscribers = 16;

struct Topic {
    const char *name;
    uint32_t size;
    uint8_t *buffer;    // heap-allocated, `size` bytes, holds the last-published value
    bool has_data;       // false until the first Publish()
    // A fixed array, not a std::vector, because Publish snapshots the list
    // before calling anyone (see there) -- and a vector's snapshot was a heap
    // allocation and a free on every publish, from the GPS, BLE and battery
    // tasks alike, into internal RAM, which is the heap this board is
    // shortest of.
    Account *subscribers[kMaxSubscribers];
    size_t subscriber_count;
};

// Well-known topics, one entry per data struct in DataCenter.h. Extend this
// table (and add a TOPIC_* constant above) to publish a new data type.
Topic s_topics[] = {
    {TOPIC_GPS_INFO, sizeof(GPS_Info_t), nullptr, false, {}, 0},
    {TOPIC_HEART_RATE, sizeof(HeartRate_t), nullptr, false, {}, 0},
    {TOPIC_CADENCE, sizeof(Cadence_t), nullptr, false, {}, 0},
    {TOPIC_IMU_DATA, sizeof(IMU_Data_t), nullptr, false, {}, 0},
    {TOPIC_BATTERY, sizeof(Battery_t), nullptr, false, {}, 0},
    {TOPIC_NAV_TBT, sizeof(TBT_Directive_t), nullptr, false, {}, 0},
    {TOPIC_PHONE_ALERT, sizeof(Alert_Info_t), nullptr, false, {}, 0},
};
constexpr size_t kTopicCount = sizeof(s_topics) / sizeof(s_topics[0]);

SemaphoreHandle_t s_mutex = nullptr;

Topic *FindTopic(const char *topic) {
    // Every caller passes one of the TOPIC_* constants, so the pointer
    // matches and the string compare is only the fallback.
    for (size_t i = 0; i < kTopicCount; i++) {
        if (s_topics[i].name == topic) {
            return &s_topics[i];
        }
    }
    for (size_t i = 0; i < kTopicCount; i++) {
        if (strcmp(s_topics[i].name, topic) == 0) {
            return &s_topics[i];
        }
    }
    return nullptr;
}

} // namespace

void DataCenter_Init() {
    // Recursive mutex: DataCenter_Publish() invokes subscriber callbacks
    // while holding the lock (see the warning in DataCenter.h), and a
    // callback that itself calls back into DataCenter (Subscribe/Publish/Pull)
    // from the same task must not deadlock against its own lock.
    s_mutex = xSemaphoreCreateRecursiveMutex();

    for (size_t i = 0; i < kTopicCount; i++) {
        s_topics[i].buffer = (uint8_t *)malloc(s_topics[i].size);
        s_topics[i].has_data = false;
    }
}

bool DataCenter_Subscribe(const char *topic, Account *account) {
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);

    Topic *t = FindTopic(topic);
    bool ok = false;
    if (t != nullptr) {
        bool already = false;
        for (size_t i = 0; i < t->subscriber_count; i++) {
            if (t->subscribers[i] == account) {
                already = true;
                break;
            }
        }
        if (!already && t->subscriber_count < kMaxSubscribers) {
            t->subscribers[t->subscriber_count++] = account;
            ok = true;
        }
    }

    xSemaphoreGiveRecursive(s_mutex);
    return ok;
}

bool DataCenter_Unsubscribe(const char *topic, Account *account) {
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);

    Topic *t = FindTopic(topic);
    bool ok = false;
    if (t != nullptr) {
        for (size_t i = 0; i < t->subscriber_count; i++) {
            if (t->subscribers[i] == account) {
                for (size_t j = i + 1; j < t->subscriber_count; j++) {
                    t->subscribers[j - 1] = t->subscribers[j];
                }
                t->subscriber_count--;
                ok = true;
                break;
            }
        }
    }

    xSemaphoreGiveRecursive(s_mutex);
    return ok;
}

bool DataCenter_Publish(const char *topic, void *data) {
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);

    Topic *t = FindTopic(topic);
    if (t == nullptr) {
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }

    memcpy(t->buffer, data, t->size);
    t->has_data = true;

    // Snapshot the subscriber list so a callback that (un)subscribes doesn't
    // shift the array we're iterating. On the stack: 64 bytes.
    Account *subscribers[kMaxSubscribers];
    const size_t count = t->subscriber_count;
    memcpy(subscribers, t->subscribers, count * sizeof(subscribers[0]));
    uint32_t size = t->size;
    uint8_t *buffer = t->buffer;

    for (size_t i = 0; i < count; i++) {
        Account *account = subscribers[i];
        if (account->Callback != nullptr) {
            account->Callback(topic, buffer, size, account->UserArg);
        }
    }

    xSemaphoreGiveRecursive(s_mutex);
    return true;
}

bool DataCenter_Pull(const char *topic, void *out_data, uint32_t out_size) {
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);

    Topic *t = FindTopic(topic);
    bool ok = false;
    if (t != nullptr && t->has_data && t->size == out_size) {
        memcpy(out_data, t->buffer, out_size);
        ok = true;
    }

    xSemaphoreGiveRecursive(s_mutex);
    return ok;
}
