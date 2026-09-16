#include "LvglTask.h"

#include <Arduino.h>
#include <lvgl.h>

static constexpr uint32_t LVGL_TASK_STACK_SIZE = 8192;
static constexpr UBaseType_t LVGL_TASK_PRIORITY = 2;
static constexpr BaseType_t LVGL_TASK_CORE = 1; // Core 1: UI & Life Cycle Core (CLAUDE.md §4)

// ---- Why this is not a fixed 5ms period any more ----
//
// It used to be, which meant 200 wake-ups a second forever: while a page
// animated, while the dashboard sat showing the same numbers, and -- worst --
// while the backlight was off and nobody could see anything at all. LVGL's own
// clocks are LV_DISP_DEF_REFR_PERIOD and LV_INDEV_DEF_READ_PERIOD, both 30ms,
// so roughly five in every six of those passes had nothing to do.
//
// lv_timer_handler() already knows how long until its next timer is due, and
// returns it. Sleeping for exactly that turns a 200Hz spin into something that
// tracks the work: ~30ms between wake-ups on a still screen, and back down to
// the floor the moment a 300ms page transition starts. Adaptive rather than
// slower -- the distinction matters, because a plainly longer fixed period
// would have bought the same idle saving by making every animation coarser.
static constexpr uint32_t LVGL_MIN_DELAY_MS = 5;
// The floor is the old period exactly, so nothing that was smooth before can
// become less smooth now: this change can only ever remove wake-ups that had
// no work behind them.

static constexpr uint32_t LVGL_MAX_DELAY_MS = 50;
// The ceiling exists for the case lv_timer_handler() reports LV_NO_TIMER_READY
// (UINT32_MAX) -- no timer is pending at all. Sleeping on that literally would
// park the task forever, and the next touch would land on a UI that had
// stopped running. 50ms is below what a finger notices and far above the
// 30ms indev period that caps this in practice.

static void lvgl_task(void *pvParameters) {
    (void)pvParameters;
    for (;;) {
        uint32_t next_ms = lv_timer_handler();
        if (next_ms < LVGL_MIN_DELAY_MS) {
            next_ms = LVGL_MIN_DELAY_MS;
        } else if (next_ms > LVGL_MAX_DELAY_MS) {
            next_ms = LVGL_MAX_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(next_ms));
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
