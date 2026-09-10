#include "GPS_Reader.h"

#include <Arduino.h>
#include <stdint.h>

#include "../system/DataCenter.h"
#include "UbxParse.h"

// Overridable per board from platformio.ini. Defaults avoid GPIO43/44 so
// UART0 stays free for a USB-TTL debug adapter -- see the header.
#ifndef GPS_UART_RX_PIN
#define GPS_UART_RX_PIN 4 // ESP32-S3 RX <- module TX
#endif
#ifndef GPS_UART_TX_PIN
#define GPS_UART_TX_PIN 5 // ESP32-S3 TX -> module RX
#endif
#ifndef GPS_UART_BAUD
// M10 modules leave the factory at 38400. Older M8 parts use 9600; if nothing
// parses, this is the first thing to try changing.
#define GPS_UART_BAUD 38400
#endif

namespace {

HardwareSerial s_uart(1); // UART1: UART0 is reserved for debug
UbxParser_t s_parser;

volatile bool s_has_fix = false;
volatile uint32_t s_frames = 0;

// ---- UBX-CFG-VALSET (class 0x06, id 0x8A) ----
// The M10 configuration interface is key/value rather than the older
// CFG-PRT/CFG-MSG messages. Keys below are from the M10 interface
// description; their storage size is encoded in the key itself (all three
// here are one byte).
//
// UNVERIFIED against hardware: no module has been connected. If NAV-PVT never
// arrives, these keys and the layer byte are the place to look.
constexpr uint32_t KEY_UART1_OUTPROT_UBX = 0x10740001;  // L: emit UBX
constexpr uint32_t KEY_UART1_OUTPROT_NMEA = 0x10740002; // L: emit NMEA
constexpr uint32_t KEY_MSGOUT_NAV_PVT_UART1 = 0x20910007; // U1: NAV-PVT rate

void AppendKeyValue(uint8_t *buf, size_t &n, uint32_t key, uint8_t value) {
    buf[n++] = (uint8_t)(key & 0xFF);
    buf[n++] = (uint8_t)((key >> 8) & 0xFF);
    buf[n++] = (uint8_t)((key >> 16) & 0xFF);
    buf[n++] = (uint8_t)((key >> 24) & 0xFF);
    buf[n++] = value;
}

// Wraps a payload in sync bytes and the 8-bit Fletcher checksum, then writes
// it out. Same checksum the parser verifies on the way in.
void SendUbx(uint8_t msg_class, uint8_t msg_id, const uint8_t *payload, uint16_t length) {
    uint8_t header[6];
    header[0] = 0xB5;
    header[1] = 0x62;
    header[2] = msg_class;
    header[3] = msg_id;
    header[4] = (uint8_t)(length & 0xFF);
    header[5] = (uint8_t)(length >> 8);

    uint8_t ck_a = 0;
    uint8_t ck_b = 0;
    for (size_t i = 2; i < 6; i++) {
        ck_a = (uint8_t)(ck_a + header[i]);
        ck_b = (uint8_t)(ck_b + ck_a);
    }
    for (uint16_t i = 0; i < length; i++) {
        ck_a = (uint8_t)(ck_a + payload[i]);
        ck_b = (uint8_t)(ck_b + ck_a);
    }

    s_uart.write(header, sizeof(header));
    if (length > 0) {
        s_uart.write(payload, length);
    }
    s_uart.write(ck_a);
    s_uart.write(ck_b);
    s_uart.flush();
}

void ConfigureReceiver() {
    uint8_t payload[4 + 3 * 5];
    size_t n = 0;

    payload[n++] = 0x00; // version
    payload[n++] = 0x01; // layers: RAM only -- deliberately not flash, so a
                         // bad configuration never outlives a power cycle
    payload[n++] = 0x00; // reserved
    payload[n++] = 0x00;

    AppendKeyValue(payload, n, KEY_UART1_OUTPROT_UBX, 1);
    AppendKeyValue(payload, n, KEY_UART1_OUTPROT_NMEA, 0); // CLAUDE.md §2: UBX only
    AppendKeyValue(payload, n, KEY_MSGOUT_NAV_PVT_UART1, 1); // one per solution

    SendUbx(0x06, 0x8A, payload, (uint16_t)n);
}

void GpsTask(void *pv) {
    (void)pv;

    for (;;) {
        while (s_uart.available() > 0) {
            const uint8_t byte = (uint8_t)s_uart.read();
            UbxNavPvt_t pvt;

            if (!Ubx_Feed(&s_parser, byte, &pvt)) {
                continue;
            }

            s_frames++;
            s_has_fix = pvt.fix_valid;

            GPS_Info_t info;
            info.fix_valid = pvt.fix_valid;
            info.num_sv = pvt.num_sv;
            info.lat = pvt.lat_deg;
            info.lon = pvt.lon_deg;
            info.speed = pvt.speed_mps;
            info.alt = pvt.alt_m;
            info.heading = pvt.heading_deg;
            info.time_valid = pvt.time_valid;
            info.year = pvt.year;
            info.month = pvt.month;
            info.day = pvt.day;
            info.hour = pvt.hour;
            info.minute = pvt.minute;
            info.second = pvt.second;

            // Published even without a fix: the UI wants to distinguish "no
            // module" from "module present, still acquiring", and num_sv
            // climbing is the visible sign of the latter.
            DataCenter_Publish(TOPIC_GPS_INFO, &info);
        }

        // NAV-PVT arrives at 1Hz and the UART buffer is far larger than one
        // frame, so polling at 20Hz is ample and leaves Core 0 idle.
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

} // namespace

void GPS_Init() {
    Ubx_Init(&s_parser);
    s_uart.begin(GPS_UART_BAUD, SERIAL_8N1, GPS_UART_RX_PIN, GPS_UART_TX_PIN);
    delay(100); // let the module's UART settle before configuring it
    ConfigureReceiver();
}

void GPS_StartReader() {
    // Core 0 per CLAUDE.md §4 ("Task 1: MAX-M10S UBX parsing"). 4KB: the
    // parser keeps a 92-byte frame buffer and nothing else of size.
    xTaskCreatePinnedToCore(GpsTask, "gps", 4096, nullptr, 2, nullptr, 0);
}

bool GPS_HasFix() {
    return s_has_fix;
}

uint32_t GPS_FrameCount() {
    return s_frames;
}
