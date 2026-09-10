#pragma once

#include <stdint.h>

// Battery sensing for the Hosyond/ES3C28P board: a 3.7V LiPo on the 1.25mm
// 2P socket, charged by an onboard TP4054, with the pack voltage brought to
// GPIO9 (ADC1_CH8) through a 2:1 divider.
//
// Pin and divider come from the board vendor's own reference
// (lcdwiki.com/2.8inch_ESP32-S3_Display: "GPIO9 ... Battery voltage ADC value
// acquisition input signal"), and the discharge curve from
// deps/Waveshare-LCD-2.8's power_manager. BATTERY_ADC_PIN is overridable from
// platformio.ini so a different board branch can point it elsewhere.

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
