#include "DataCenter.h"

#include <Arduino.h>
#include <string.h>
#include <vector>

const char *const TOPIC_GPS_INFO = "GPS_Info";
const char *const TOPIC_HEART_RATE = "Sensor/HeartRate";
const char *const TOPIC_IMU_DATA = "Sensor/IMU";
const char *const TOPIC_BATTERY = "Sensor/Battery";

Account::Account(const char *id, DataCenter_Callback_t callback, void *user_arg)
    : ID(id), Callback(callback), UserArg(user_arg) {
}

namespace {

struct Topic {
    const char *name;
    uint32_t size;
    uint8_t *buffer;    // heap-allocated, `size` bytes, holds the last-published value
    bool has_data;       // false until the first Publish()
    std::vector<Account *> subscribers;
};

// Well-known topics, one entry per data struct in DataCenter.h. Extend this
// table (and add a TOPIC_* constant above) to publish a new data type.
Topic s_topics[] = {
    {TOPIC_GPS_INFO, sizeof(GPS_Info_t), nullptr, false, {}},
    {TOPIC_HEART_RATE, sizeof(HeartRate_t), nullptr, false, {}},
    {TOPIC_IMU_DATA, sizeof(IMU_Data_t), nullptr, false, {}},
    {TOPIC_BATTERY, sizeof(Battery_t), nullptr, false, {}},
};
constexpr size_t kTopicCount = sizeof(s_topics) / sizeof(s_topics[0]);

SemaphoreHandle_t s_mutex = nullptr;

Topic *FindTopic(const char *topic) {
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
        for (Account *existing : t->subscribers) {
            if (existing == account) {
                already = true;
                break;
            }
        }
        if (!already) {
            t->subscribers.push_back(account);
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
        for (size_t i = 0; i < t->subscribers.size(); i++) {
            if (t->subscribers[i] == account) {
                t->subscribers.erase(t->subscribers.begin() + i);
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
    // invalidate the vector we're iterating.
    std::vector<Account *> subscribers = t->subscribers;
    uint32_t size = t->size;
    uint8_t *buffer = t->buffer;

    for (Account *account : subscribers) {
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
