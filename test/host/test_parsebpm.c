/* Host-side check of the exact logic in SoftANT.cpp's ParseBPM(), to verify
 * the assumptions I made about the vendored decoder's contract:
 *   - antplus_hrm_decode(page, &out) returns bool
 *   - out.computed_heart_rate carries BPM
 *   - 0 means invalid
 * and that a non-HRM / garbage page is rejected rather than yielding junk. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "antplus_profiles.h"

static int failures = 0;
static int checks = 0;

static void check(int cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); failures++; }
}

/* Byte-for-byte the body of ParseBPM() in src/sensors/SoftANT.cpp. */
static uint8_t ParseBPM(uint8_t *payload) {
    if (payload == NULL) return 0;
    antplus_hrm_data_t hr;
    if (!antplus_hrm_decode(payload, &hr)) return 0;
    return hr.computed_heart_rate;
}

int main(void) {
    /* 1. Round-trip a realistic strap reading through the library's encoder. */
    for (unsigned bpm = 1; bpm <= 255; bpm++) {
        antplus_hrm_data_t in;
        memset(&in, 0, sizeof(in));
        in.page_number = 0;
        in.computed_heart_rate = (uint8_t)bpm;
        in.heart_beat_event_time = 1234;
        in.heart_beat_count = 42;

        uint8_t page[8];
        antplus_hrm_encode_page0(&in, false, page);

        uint8_t got = ParseBPM(page);
        if (got != (uint8_t)bpm) {
            printf("  FAIL: bpm %u round-tripped as %u\n", bpm, got);
            failures++;
        }
        checks++;
    }
    printf("- round-trip bpm 1..255: done\n");

    /* 2. NULL payload must not crash and must report "no reading". */
    check(ParseBPM(NULL) == 0, "NULL payload returns 0");
    printf("- null payload: done\n");

    /* 3. A zero page must not be reported as a live heart rate. */
    uint8_t zero[8];
    memset(zero, 0, sizeof(zero));
    printf("- all-zero page -> ParseBPM = %u (expect 0)\n", ParseBPM(zero));
    check(ParseBPM(zero) == 0, "all-zero page yields no BPM");

    /* 4. The toggle bit (bit7 of byte0) must not leak into the page number or
     *    corrupt the BPM -- the strap flips it every 4 messages. */
    antplus_hrm_data_t in;
    memset(&in, 0, sizeof(in));
    in.page_number = 0;
    in.computed_heart_rate = 67;   /* the README's live-strap reading */
    uint8_t page_t0[8], page_t1[8];
    antplus_hrm_encode_page0(&in, false, page_t0);
    antplus_hrm_encode_page0(&in, true, page_t1);
    check(ParseBPM(page_t0) == 67, "toggle=0 decodes to 67");
    check(ParseBPM(page_t1) == 67, "toggle=1 decodes to 67");
    printf("- toggle bit: t0=%u t1=%u\n", ParseBPM(page_t0), ParseBPM(page_t1));

    printf("\nchecks: %d  failures: %d\nRESULT: %s\n", checks, failures,
           failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
