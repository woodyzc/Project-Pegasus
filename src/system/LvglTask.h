#pragma once

// Starts lvgl_task() pinned to Core 1, which owns LVGL's lv_timer_handler()
// loop for the lifetime of the firmware (per CLAUDE.md §4: Core 1 = UI &
// Life Cycle Core). Call once from setup(), after Display_Init()/Touch_Init()
// and lv_init()/indev registration.
void LvglTask_Start();
