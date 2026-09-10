#include "LvglTask.h"

#include <Arduino.h>
#include <lvgl.h>

static constexpr uint32_t LVGL_TASK_STACK_SIZE = 8192;
static constexpr UBaseType_t LVGL_TASK_PRIORITY = 2;
static constexpr BaseType_t LVGL_TASK_CORE = 1; // Core 1: UI & Life Cycle Core (CLAUDE.md §4)

static void lvgl_task(void *pvParameters) {
    (void)pvParameters;
    const TickType_t period = pdMS_TO_TICKS(5);
    for (;;) {
        lv_timer_handler();
        vTaskDelay(period);
    }
}

void LvglTask_Start() {
    xTaskCreatePinnedToCore(
        lvgl_task,
        "lvgl_task",
        LVGL_TASK_STACK_SIZE,
        nullptr,
        LVGL_TASK_PRIORITY,
        nullptr,
        LVGL_TASK_CORE);
}
