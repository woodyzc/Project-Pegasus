/* Host tests for src/sensors/BleHrParse.c -- the BLE Heart Rate Measurement
 * (0x2A37) parser used by BLE_HR_Client's notify callback.
 *
 * This compiles the real shipped source, not a copy, so the flags/width/length
 * handling is actually covered rather than approximated. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "BleHrParse.h"

static int failures = 0;
static int checks = 0;

static void check(bool cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); failures++; }
}

static void expect_bpm(const uint8_t *pkt, size_t len, uint16_t want, const char *what) {
    uint16_t got = 0;
    bool ok = BLE_HR_ParseMeasurement(pkt, len, &got);
    checks++;
    if (!ok || got != want) {
        printf("  FAIL: %s (ok=%d got=%u want=%u)\n", what, (int)ok, got, want);
        failures++;
    }
}

static void expect_reject(const uint8_t *pkt, size_t len, const char *what) {
    uint16_t got = 0xBEEF;
    bool ok = BLE_HR_ParseMeasurement(pkt, len, &got);
    checks++;
    if (ok) { printf("  FAIL: %s (accepted, got=%u)\n", what, got); failures++; }
}

int main(void) {
    /* --- 8-bit format (flags bit0 = 0) --- */
    for (unsigned bpm = 1; bpm <= 255; bpm++) {
        uint8_t pkt[2] = { 0x00, (uint8_t)bpm };
        expect_bpm(pkt, sizeof(pkt), (uint16_t)bpm, "8-bit round trip");
    }
    printf("- 8-bit format, bpm 1..255: done\n");

    /* Other flag bits (sensor contact, energy expended, RR intervals) must not
     * be mistaken for the width bit. */
    {
        uint8_t pkt[2] = { 0x06, 72 };   /* bits 1-2 set, bit0 clear */
        expect_bpm(pkt, sizeof(pkt), 72, "flags 0x06 still 8-bit");
        uint8_t pkt2[2] = { 0xFE, 72 };  /* every bit but bit0 */
        expect_bpm(pkt2, sizeof(pkt2), 72, "flags 0xFE still 8-bit");
    }
    printf("- unrelated flag bits ignored: done\n");

    /* --- 16-bit format (flags bit0 = 1), little-endian --- */
    {
        uint8_t pkt[3] = { 0x01, 0x48, 0x00 };  /* 72 */
        expect_bpm(pkt, sizeof(pkt), 72, "16-bit low byte only");

        uint8_t pkt2[3] = { 0x01, 0x2C, 0x01 };  /* 300 -> implausible, rejected */
        expect_reject(pkt2, sizeof(pkt2), "16-bit 300 bpm rejected as implausible");

        uint8_t pkt3[3] = { 0x01, 0xFF, 0x00 };  /* 255, the boundary */
        expect_bpm(pkt3, sizeof(pkt3), 255, "16-bit 255 accepted");

        uint8_t pkt4[3] = { 0x01, 0x00, 0x01 };  /* 256, just over */
        expect_reject(pkt4, sizeof(pkt4), "16-bit 256 rejected");

        /* Endianness really is little: 0x0001 not 0x0100. */
        uint8_t pkt5[3] = { 0x01, 0x01, 0x00 };
        expect_bpm(pkt5, sizeof(pkt5), 1, "16-bit little-endian byte order");
    }
    printf("- 16-bit format + endianness + clamping: done\n");

    /* --- Malformed input --- */
    {
        uint8_t pkt[3] = { 0x01, 0x48, 0x00 };
        expect_reject(NULL, 3, "null buffer");
        expect_reject(pkt, 0, "zero length");
        expect_reject(pkt, 1, "flags byte only");
        /* Flags claim uint16 but only 2 bytes exist. Deliberately a real
         * 2-byte allocation (not a longer buffer with a short length), so
         * ASan actually traps if the parser reads data[2] anyway. */
        uint8_t truncated[2] = { 0x01, 0x48 };
        expect_reject(truncated, sizeof(truncated), "16-bit flag with truncated packet");

        uint8_t zero8[2] = { 0x00, 0x00 };
        expect_reject(zero8, sizeof(zero8), "8-bit zero reading");
        uint8_t zero16[3] = { 0x01, 0x00, 0x00 };
        expect_reject(zero16, sizeof(zero16), "16-bit zero reading");
    }
    printf("- malformed/zero packets rejected: done\n");

    /* out_bpm must be left alone when the packet is rejected. */
    {
        uint8_t bad[2] = { 0x00, 0x00 };
        uint16_t sentinel = 0x1234;
        BLE_HR_ParseMeasurement(bad, sizeof(bad), &sentinel);
        check(sentinel == 0x1234, "out_bpm untouched on reject");

        uint8_t good[2] = { 0x00, 60 };
        check(!BLE_HR_ParseMeasurement(good, sizeof(good), NULL), "null out_bpm rejected");
    }
    printf("- output contract: done\n");

    printf("\nchecks: %d  failures: %d\nRESULT: %s\n", checks, failures,
           failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
