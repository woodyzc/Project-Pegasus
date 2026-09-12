#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* LVGL's tick, supplied by the simulator instead of Arduino's millis(). */
unsigned int sim_millis(void);
#ifdef __cplusplus
}
#endif
