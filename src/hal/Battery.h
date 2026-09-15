#pragma once

#include <stdint.h>

// Battery sensing for the Waveshare ESP32-S3-Touch-LCD-2.8: a 3.7V LiPo on
// the onboard 2P socket, with the pack voltage brought to GPIO8 through a
// 3:1 divider.
//
// The pin, the divider and the 0.990476 trim all come from the vendor's own
// driver (WS_ESP32_Touch28/src/BAT_Driver.cpp, `Volts * 3.0 / 0.990476`),
// carried here as BATTERY_ADC_PIN / BATTERY_DIVIDER_NUM / BATTERY_CAL_PERMILLE
// in platformio.ini so each board branch sets its own.
//
// ⚠️ The divider is 3:1 on this board, where the Hosyond board's was 2:1.
// Nothing about that is visible at runtime -- a wrong ratio reads as a pack
// that is simply flatter or fuller than it is, on a curve where 3.7V and 4.2V
// are only half a volt apart. Worth checking against a multimeter once.

// Configures the ADC. Call once from setup() before Battery_StartMonitor().
void Battery_Init();

// One averaged reading of the pack voltage in millivolts (already multiplied
// back up through the divider). Returns 0 if Battery_Init() hasn't run.
uint16_t Battery_ReadMillivolts();

// Maps pack millivolts to 0-100% along the LiPo curve.
uint8_t Battery_PercentFromMillivolts(uint16_t millivolts);

// Spawns the Core 0 power-monitoring task (CLAUDE.md §4, Core 0 Task 3) that
// samples periodically and publishes Battery_t to TOPIC_BATTERY.
// Preconditions: DataCenter_Init() and Battery_Init() have run.
void Battery_StartMonitor();
