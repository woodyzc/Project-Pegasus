#include "GPS_Reader.h"

#include <Arduino.h>
#include <stdint.h>

#include "../system/DataCenter.h"
#include "NmeaParse.h"

// Overridable per board from platformio.ini. Defaults are the ES3C28P
// reference design's IO2/IO3/IO14/IO21 expansion header -- see the header
// comment for why neither GPIO43/44 (this board's silkscreened UART header,
// reserved for USB-TTL debug) nor GPIO4/5 (wired to the onboard PCM5101 I2S
// amp) are free.
#ifndef GPS_UART_RX_PIN
#define GPS_UART_RX_PIN 2 // ESP32-S3 RX <- module TX
#endif
#ifndef GPS_UART_TX_PIN
#define GPS_UART_TX_PIN 3 // ESP32-S3 TX -> module RX
#endif
#ifndef GPS_UART_BAUD
// The ATGM336H leaves the factory at 9600, and nothing in this firmware asks
// it to change -- there is no documented binary config protocol to change it
// with, unlike the M10's UBX-CFG-VALSET. If a different ATGM336H batch turns
// out to ship at a different rate, this is the first thing to try changing.
#define GPS_UART_BAUD 9600
#endif

namespace {

HardwareSerial s_uart(1); // UART1: UART0 is reserved for debug
NmeaParser_t s_parser;

volatile bool s_has_fix = false;
volatile uint32_t s_frames = 0;

void GpsTask(void *pv) {
    (void)pv;

    for (;;) {
        while (s_uart.available() > 0) {
            const uint8_t byte = (uint8_t)s_uart.read();
            NmeaFix_t fix;

            if (!Nmea_Feed(&s_parser, byte, &fix)) {
                continue;
            }

            s_frames++;
            s_has_fix = fix.fix_valid;

            GPS_Info_t info;
            info.fix_valid = fix.fix_valid;
            info.num_sv = fix.num_sv;
            info.lat = fix.lat_deg;
            info.lon = fix.lon_deg;
            info.speed = fix.speed_mps;
            info.alt = fix.alt_m;
            // heading_valid can be false (course-over-ground blank on a
            // stopped rider) without that meaning "no module" -- num_sv and
            // fix_valid already carry that distinction, so a blank heading
            // is reported as 0 the same way a UBX headMot would be.
            info.heading = fix.heading_valid ? fix.heading_deg : 0.0f;
            info.time_valid = fix.time_valid;
            info.year = fix.year;
            info.month = fix.month;
            info.day = fix.day;
            info.hour = fix.hour;
            info.minute = fix.minute;
            info.second = fix.second;

            // Says who it came from, so the phone-fix arbitration in
            // BLE_TBT_Receiver can tell this apart from a position it
            // republished itself. Both land on the same topic.
            info.from_module = true;

            // Published even without a fix: the UI wants to distinguish "no
            // module" from "module present, still acquiring", and num_sv
            // climbing is the visible sign of the latter -- GGA reports it
            // every second regardless of fix quality.
            DataCenter_Publish(TOPIC_GPS_INFO, &info);
        }

        // GGA arrives at 1Hz and the UART buffer is far larger than one
        // sentence, so polling at 20Hz is ample and leaves Core 0 idle.
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

} // namespace

void GPS_Init() {
    Nmea_Init(&s_parser);
    s_uart.begin(GPS_UART_BAUD, SERIAL_8N1, GPS_UART_RX_PIN, GPS_UART_TX_PIN);
    // Nothing to configure: the ATGM336H streams GGA/RMC (and others we
    // ignore -- GSA, GSV, VTG) at 1Hz from power-up, and there is no
    // documented command set for this part to ask it to do otherwise.
}

void GPS_StartReader() {
    // Core 0 per CLAUDE.md §4 ("Task 1: MAX-M10S UBX parsing" -- the task
    // itself is unchanged, only the wire format it decodes). 4KB: the parser
    // keeps a 96-byte line buffer and nothing else of size.
    xTaskCreatePinnedToCore(GpsTask, "gps", 4096, nullptr, 2, nullptr, 0);
}

bool GPS_HasFix() {
    return s_has_fix;
}

uint32_t GPS_FrameCount() {
    return s_frames;
}
