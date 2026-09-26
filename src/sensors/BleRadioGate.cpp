#include "BleRadioGate.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
SemaphoreHandle_t s_mutex = nullptr;
}

void BleRadioGate_Init(void) {
    if (s_mutex != nullptr) {
        return;
    }
    // A plain mutex rather than a recursive one: nothing here nests, and a
    // recursive mutex would quietly permit the nesting that this is meant to
    // make impossible.
    s_mutex = xSemaphoreCreateMutex();
}

bool BleRadioGate_Acquire(void) {
    if (s_mutex == nullptr) {
        return false;
    }
    // No timeout. The holder is always a supervisor task doing one bounded
    // thing -- a 15s scan or a 5s connect -- and a timeout here would mean
    // proceeding anyway, which is precisely the concurrency this exists to
    // prevent. Waiting is the correct behaviour for a sensor that is only
    // ever a second or two from being retried again.
    return xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE;
}

void BleRadioGate_Release(void) {
    if (s_mutex == nullptr) {
        return;
    }
    xSemaphoreGive(s_mutex);
}
