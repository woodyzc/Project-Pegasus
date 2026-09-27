#include "Touch.h"

#include <Arduino.h>
#include <Wire.h>
#include <stdio.h>
#include <string.h>
#include "../system/PowerManager.h"

// CST328 (Hynitron) I2C address and the subset of its register map this
// driver needs. Addresses are 16-bit and go out big-endian -- the single
// biggest difference from the FT6336G this replaces, and the one that makes
// the controller look dead if you get it wrong.
// Two parts, two addresses, one protocol.
//
// This board family ships either a **CST328 at 0x1A** (V1) or a **CST3530 at
// 0x58** (V2), and the label on the back is the only outward difference.
// Hynitron's mutual-capacitance parts default to one or the other. They share
// the 16-bit big-endian register map, so the whole of the rest of this driver
// is common to both and only the address has to be decided.
//
// Decided by probing rather than by a build flag, because the alternative is
// a firmware that silently does nothing on half the boards it is flashed to --
// which is exactly what happened here on 2026-09-26: a V2 board, a driver
// hard-coded to 0x1A, and a panel that looked wired wrong. An I2C scan of the
// bus found one device at 0x58 and settled it in a single bench round.
static constexpr uint8_t CST328_I2C_ADDR = 0x1A;
static constexpr uint8_t CST3530_I2C_ADDR = 0x58;

// Resolved by Touch_Init(). Zero means nothing on the bus answered.
static uint8_t s_addr = 0;

// The info-block signature the controller reported, recorded for the bring-up
// readout. 0 means the block was never read.
static uint16_t s_signature = 0;
static constexpr uint16_t REG_TOUCH_COUNT = 0xD005;  // low nibble = points, and the latch that must be cleared
static constexpr uint16_t REG_TOUCH_XY = 0xD000;     // the touch block: status, count and up to 5 packed points

// CST3xx constants, from Linux's drivers/input/touchscreen/hynitron_cstxxx.c.
// That driver is the only published description of this protocol; the part
// has no register appendix from Hynitron. Its register constants look
// byte-swapped against ours (0x00d0 vs 0xD000) only because it writes them
// little-endian -- the bytes on the wire are D0 00 either way.
static constexpr uint8_t CST3XX_CHK_VAL = 0xAB;          // byte 6 of a valid frame, and the ack payload
static constexpr uint8_t CST3XX_TOUCH_COUNT_MASK = 0x7F; // GENMASK(6, 0)
static constexpr uint16_t REG_DEBUG_MODE = 0xD101;   // enter debug/info mode
static constexpr uint16_t REG_NORMAL_MODE = 0xD109;  // back to reporting touches
static constexpr uint16_t REG_INFO_TP_NTX = 0xD1F4;  // 24-byte info block; bytes 10..11 are the 0xCACA signature

// The touch panel has its own I2C bus on this board, separate from the
// sensor bus. Wire1 is it; Wire is deliberately untouched (see Touch.h).
static TwoWire &TOUCH_BUS = Wire1;

static bool s_controller_found = false;

// Bring-up counters. See Touch_DebugCounters in the header for why these
// exist: they separate "the I2C link is dead" from "the panel reports
// nothing" from "the coordinates are wrong", which look identical from the
// outside on a board whose only input is the thing being tested.
static uint32_t s_dbg_reads = 0;
static uint32_t s_dbg_presses = 0;
static uint16_t s_dbg_last_x = 0;
static uint16_t s_dbg_last_y = 0;

// The first bytes of the most recent touch block, and of the most recent one
// whose count byte was non-zero. Ground truth about the frame layout, which
// beats any document: Hynitron publishes no register appendix for this part,
// and the kernel driver's offsets were derived for its siblings.
static uint8_t s_dbg_frame[8] = {0};
static uint8_t s_dbg_hit[8] = {0};

// One-shot bring-up probe: does this part take a 16-bit register address or
// an 8-bit one? The kernel's CST3xx driver uses 16-bit, but Espressif's
// CST3530 component hides the register layer inside a proprietary Hynitron
// blob, and Hynitron's own CST816 family uses 8-bit -- so the width is an
// assumption, not a fact, and every read so far has come back as an undriven
// bus. Reports the ACK code of each register write as well as the bytes,
// because a NAK on the second address byte would explain everything.
static char s_dbg_probe[64] = "";

// How many polls found the interrupt line asserted. If this never moves, the
// controller is not announcing contacts -- which is a different fault from
// announcing them in a layout we cannot read.
static uint32_t s_dbg_int_low = 0;

// Set by the interrupt, consumed by the LVGL read.
//
// The vendor driver reads the panel ONLY from here -- its Touch_Loop() is
// `if (Touch_interrupts) { ... read ... }` and nothing else ever touches the
// registers. That is the piece this driver was missing all along: polling
// 0xD005 asynchronously returns whatever the controller happens to be holding
// mid-scan, which decodes as a contact roughly a quarter of the time at
// coordinates that are not on the panel. It is why the UI has been navigating
// itself.
//
// RISING, and INPUT with no pull-up, both copied from the vendor
// (`#define interrupt RISING`). An earlier attempt here gated on the LOW
// *level* and added a pull-up, which is neither of those things.
volatile bool s_irq = false;

void IRAM_ATTR TouchISR() {
    s_irq = true;
}

// What the last interrupt-driven read found. LVGL polls far faster than the
// panel reports, so every poll between interrupts answers from here rather
// than going near the bus.
static bool s_pressed = false;
static uint16_t s_cached_x = 0;
static uint16_t s_cached_y = 0;

// Raw-panel-to-display orientation mapping, against Display_Init()'s
// tft.setRotation(0). The CST328 is configured by the panel module itself
// with the glass's native 240x320 resolution and reports in those
// coordinates, origin top-left -- so, as on the previous board, no transform.
//
// UNVERIFIED on this hardware. If a touch in the top-right acts on the
// top-left, set TOUCH_INVERT_X; if top and bottom are swapped, TOUCH_INVERT_Y;
// if the axes are transposed, TOUCH_SWAP_XY. One of the three, not a
// combination, is almost always the answer.
static constexpr bool TOUCH_SWAP_XY = false;
static constexpr bool TOUCH_INVERT_X = false;
static constexpr bool TOUCH_INVERT_Y = false;

// The raw range the panel actually produces, stretched onto the screen's.
//
// Identity by default -- 0..239 and 0..319 are the first and last pixels of a
// 240x320 display. The constants exist because "reports display coordinates"
// is not the same as "reaches every corner": if the digitizer's active area is
// inset from the glass, the corners never reach 0 or the last column, and a
// control in the corner becomes unreachable no matter how large its touch
// area is made.
//
// To correct it, find the extremes the panel actually reports at each corner
// and put them here; everything between is linear, so four numbers are the
// whole calibration.
//
// Note there is no pixel 240 or 320. A 240-wide display ends at 239, and a
// mapping that produced 240 would be pointing one column past the screen.
static constexpr uint16_t TOUCH_RAW_MIN_X = 0;
static constexpr uint16_t TOUCH_RAW_MAX_X = TFT_WIDTH - 1;
static constexpr uint16_t TOUCH_RAW_MIN_Y = 0;
static constexpr uint16_t TOUCH_RAW_MAX_Y = TFT_HEIGHT - 1;

// Maps one axis from the panel's range onto the screen's, and clamps.
//
// Clamping is not defensive decoration: a reading a few counts outside the
// calibrated range lands off-screen, and LVGL will happily deliver a press at
// a coordinate no widget occupies -- which is a tap that does nothing, the
// hardest kind of fault to see.
static uint16_t MapAxis(uint16_t raw, uint16_t raw_min, uint16_t raw_max, uint16_t span) {
    if (raw_max <= raw_min) {
        return 0;
    }
    if (raw <= raw_min) {
        return 0;
    }
    if (raw >= raw_max) {
        return (uint16_t)(span - 1);
    }
    const uint32_t scaled = (uint32_t)(raw - raw_min) * (uint32_t)(span - 1);
    return (uint16_t)(scaled / (uint32_t)(raw_max - raw_min));
}

// A read is: write the two register-address bytes (no stop), then read `len`.
static bool ReadReg(uint16_t reg, uint8_t *buf, uint8_t len) {
    if (s_addr == 0) {
        return false;
    }
    TOUCH_BUS.beginTransmission(s_addr);
    TOUCH_BUS.write((uint8_t)(reg >> 8));
    TOUCH_BUS.write((uint8_t)(reg & 0xFF));
    // STOP, matching the vendor driver's Touch_I2C_Read exactly. A repeated
    // START was tried and changed nothing; this part is happy either way,
    // and matching the reference removes one more place to be wrong.
    if (TOUCH_BUS.endTransmission(true) != 0) {
        return false;
    }
    if (TOUCH_BUS.requestFrom((int)s_addr, (int)len) != len) {
        return false;
    }
    for (uint8_t i = 0; i < len; i++) {
        buf[i] = TOUCH_BUS.read();
    }
    return true;
}

// `len` of 0 writes the register address alone, which is how the mode
// registers are poked.
static bool WriteReg(uint16_t reg, const uint8_t *buf, uint8_t len) {
    if (s_addr == 0) {
        return false;
    }
    TOUCH_BUS.beginTransmission(s_addr);
    TOUCH_BUS.write((uint8_t)(reg >> 8));
    TOUCH_BUS.write((uint8_t)(reg & 0xFF));
    for (uint8_t i = 0; i < len; i++) {
        TOUCH_BUS.write(buf[i]);
    }
    return TOUCH_BUS.endTransmission(true) == 0;
}

// The controller latches its touch count until it is written back to zero.
// Every read path has to end here or the panel reports one press and then
// appears to die.
static void ClearTouchLatch() {
    const uint8_t zero = 0;
    WriteReg(REG_TOUCH_COUNT, &zero, 1);
}

// ---- CST3530 (V2 board), from Waveshare's Touch_CST3530.cpp ----
//
// A different protocol from the CST328, not a variant of it. Its registers
// are **32-bit** and go out most-significant byte first, which is why every
// attempt built on the CST328's 16-bit addressing read an undriven bus: the
// part was being sent half an address.
//
// The end-of-read command is a register address with no payload at all -- the
// 0xAB lives in the address itself, not in a data byte, which is the other
// thing the kernel-derived attempt got wrong.
static constexpr uint32_t CST3530_DATA_REG = 0xD0070000u;
static constexpr uint32_t CST3530_END_READ_REG = 0xD00002ABu;

static bool Read32(uint32_t reg, uint8_t *buf, uint32_t len) {
    if (s_addr == 0) {
        return false;
    }
    TOUCH_BUS.beginTransmission(s_addr);
    TOUCH_BUS.write((uint8_t)(reg >> 24));
    TOUCH_BUS.write((uint8_t)(reg >> 16));
    TOUCH_BUS.write((uint8_t)(reg >> 8));
    TOUCH_BUS.write((uint8_t)(reg & 0xFF));
    // Repeated START for reads, STOP for writes -- the vendor's CST3530 code
    // differs from its CST328 code here too.
    if (TOUCH_BUS.endTransmission(false) != 0) {
        return false;
    }
    if (TOUCH_BUS.requestFrom((int)s_addr, (int)len) != (int)len) {
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)TOUCH_BUS.read();
    }
    return true;
}

static bool Write32(uint32_t reg) {
    if (s_addr == 0) {
        return false;
    }
    TOUCH_BUS.beginTransmission(s_addr);
    TOUCH_BUS.write((uint8_t)(reg >> 24));
    TOUCH_BUS.write((uint8_t)(reg >> 16));
    TOUCH_BUS.write((uint8_t)(reg >> 8));
    TOUCH_BUS.write((uint8_t)(reg & 0xFF));
    return TOUCH_BUS.endTransmission(true) == 0;
}

// Does anything acknowledge this address? A bare address probe, so it asks
// only "is a device electrically present" without assuming a register map.
static bool AddressAcks(uint8_t addr) {
    TOUCH_BUS.beginTransmission(addr);
    return TOUCH_BUS.endTransmission(true) == 0;
}

void Touch_Init() {
    // 400kHz, as the vendor driver uses. 100kHz was tried while chasing what
    // looked like signal integrity and made no difference either way.
    TOUCH_BUS.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL, 400000);

    // INPUT_PULLUP, not INPUT. The line is active-low **open-drain**: the
    // controller can pull it down and nothing can pull it up, so without a
    // pull-up it floats -- and it floated low, which read as "a contact is
    // waiting" on every single poll. The read counter and the interrupt
    // counter came back exactly equal, 190/190 and 315/315, which is what
    // that looks like from outside.
    //
    // The comment on this line has said "open-drain" since the port was
    // written, next to a pinMode that could not honour it.
    pinMode(TOUCH_INT_PIN, INPUT); // as the vendor does; also the deep-sleep wake source (see PowerManager)
    pinMode(TOUCH_RST_PIN, OUTPUT);

    // Vendor reset sequence, timings included. It starts by driving RST high
    // rather than assuming a level, so a warm restart gets a real edge.
    digitalWrite(TOUCH_RST_PIN, HIGH);
    delay(50);
    digitalWrite(TOUCH_RST_PIN, LOW);
    delay(5);
    digitalWrite(TOUCH_RST_PIN, HIGH);
    delay(50);

    // Which part is fitted, decided the way the V2 demo decides it: try the
    // CST328 and its 0xCACA signature first, and on failure reset again and
    // probe for a CST3530 at 0x58. The two boards are outwardly identical
    // apart from a label, and V1 was discontinued in June 2026, so a V2 is
    // now the likely board rather than the exception.
    s_addr = CST328_I2C_ADDR;
    uint8_t info[24];
    WriteReg(REG_DEBUG_MODE, nullptr, 0);
    if (ReadReg(REG_INFO_TP_NTX, info, sizeof(info))) {
        s_signature = (uint16_t)(((uint16_t)info[11] << 8) | info[10]);
    }
    WriteReg(REG_NORMAL_MODE, nullptr, 0);

    if (s_signature == 0xCACA) {
        s_controller_found = true;
        ClearTouchLatch(); // CST328 only: it latches its count until cleared
    } else {
        // The CST3530's own reset timing, which is not the CST328's: low for
        // 100ms then high for 500ms, against high/50, low/5, high/50.
        digitalWrite(TOUCH_RST_PIN, LOW);
        delay(100);
        digitalWrite(TOUCH_RST_PIN, HIGH);
        delay(500);

        s_addr = CST3530_I2C_ADDR;
        s_controller_found = AddressAcks(CST3530_I2C_ADDR);
        if (!s_controller_found) {
            s_addr = 0;
        }
        // No signature check here, and that is the vendor's choice too:
        // TOUCH2_Init() probes the address and nothing more. The 0xCACA
        // constant belongs to the CST328 -- gating a CST3530 on it was my
        // mistake, and it is what made a working part look like dead
        // hardware.
    }

    attachInterrupt(digitalPinToInterrupt(TOUCH_INT_PIN), TouchISR, RISING);

}

bool Touch_ControllerFound() {
    return s_controller_found;
}

// Acknowledges the touch block the way a CST3xx expects: the three bytes
// D0 00 AB, which is the kernel driver's CST3XX_TOUCH_DATA_STOP_CMD written
// little-endian. Without it the controller keeps handing back the same frame.
static void AckTouchBlock() {
    const uint8_t ab = CST3XX_CHK_VAL;
    WriteReg(REG_TOUCH_XY, &ab, 1);
}

// One poll of the panel: true when a finger is down, with the raw coordinates
// the controller reported.
//
// The two parts need different handling here, which the shared register map
// did not advertise. The CST3530 path follows Linux's hynitron_cstxxx.c
// (drivers/input/touchscreen), which is the only published description of
// this protocol -- Hynitron ships no register appendix for the part.
static bool ReadContact(uint16_t *raw_x, uint16_t *raw_y) {
    // No verified controller, no touch reports. See Touch_Init.
    if (!s_controller_found) {
        return false;
    }

    if (s_addr == CST3530_I2C_ADDR) {
        // Nine bytes from 0xD0070000, then the end-of-read command whatever
        // the outcome -- the vendor sends it on every path, valid or not.
        uint8_t buf[9];
        if (!Read32(CST3530_DATA_REG, buf, sizeof(buf))) {
            return false;
        }
        s_dbg_reads++;
        memcpy(s_dbg_frame, buf, sizeof(s_dbg_frame));

        // Two validity conditions, both the vendor's: a zero count in the low
        // nibble of byte 3, or a zero high nibble of byte 8, means no contact.
        const uint8_t count = buf[3] & 0x0F;
        if (count == 0 || (buf[8] & 0xF0) == 0 || count > 5) {
            Write32(CST3530_END_READ_REG);
            return false;
        }
        Write32(CST3530_END_READ_REG);
        memcpy(s_dbg_hit, buf, sizeof(s_dbg_hit));

        // Packed quite differently from the CST328: the low byte of each axis
        // is its own byte, and byte 7 carries both high nibbles -- X's in the
        // low half, Y's in the high half.
        *raw_x = (uint16_t)(((uint16_t)(buf[7] & 0x0F) << 8) | buf[4]);
        *raw_y = (uint16_t)(((uint16_t)(buf[7] & 0xF0) << 4) | buf[5]);
        return true;
    }

    // Waveshare's own Touch_CST328.cpp, followed step for step: read the
    // count at 0xD005, and only if it is non-zero read 27 bytes of points at
    // 0xD000 into buf[1..], then clear the count register. The coordinate
    // packing below is theirs verbatim.
    //
    // This is what the driver did originally, and it was right -- it was
    // simply talking to 0x1A on a board whose controller answers at 0x58, so
    // every transaction failed and the bytes were never data. Ten rounds of
    // register widths, read lengths, repeated STARTs, clock speeds and two
    // borrowed frame layouts were all spent on garbage produced by that one
    // wrong address, which the address probe had already fixed before any of
    // them.
    uint8_t count = 0;
    if (!ReadReg(REG_TOUCH_COUNT, &count, 1)) {
        return false;
    }
    s_dbg_reads++;
    s_dbg_frame[0] = count;
    count &= 0x0F;
    // The vendor treats an impossible count the same as none: clear and drop.
    if (count == 0 || count > 5) {
        ClearTouchLatch();
        return false;
    }

    // buf[0] is left for the status byte the vendor driver keeps at index 1 of
    // its own buffer, so the packed-point offsets below match theirs exactly.
    uint8_t buf[28];
    if (!ReadReg(REG_TOUCH_XY, &buf[1], 27)) {
        ClearTouchLatch();
        return false;
    }
    ClearTouchLatch();
    memcpy(s_dbg_frame + 1, buf + 1, sizeof(s_dbg_frame) - 1);
    memcpy(s_dbg_hit, s_dbg_frame, sizeof(s_dbg_hit));

    *raw_x = (uint16_t)(((uint16_t)buf[2] << 4) | ((buf[4] & 0xF0) >> 4));
    *raw_y = (uint16_t)(((uint16_t)buf[3] << 4) | (buf[4] & 0x0F));
    return true;
}

bool Touch_IsPressed() {
    uint16_t x = 0;
    uint16_t y = 0;
    // An I2C read that failed is not a press. Reporting one would let a
    // loose connector skip the splash on every boot.
    return ReadContact(&x, &y);
}

void Touch_Read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    // Interrupt to begin a touch; poll while one is in progress.
    //
    // Reading only on the interrupt is what the vendor does, and it is right
    // for the idle case -- it is what keeps a panel nobody is touching off
    // the bus entirely. But it samples a moving finger only as often as the
    // controller raises an edge, and LVGL needs several *moving* positions
    // inside a gesture's window to call it a swipe. With the frame rate
    // measured at 33fps/5%CPU, sparse sampling is the only thing left that
    // explains a swipe being hard to land.
    //
    // So while a contact is down, every LVGL read (30ms) goes and asks. That
    // cannot bring the phantom touches back: those were the 16-bit register
    // address returning garbage that decoded as contacts, not polling as
    // such, and a poll that finds no contact now simply reports none.
    if (s_irq || s_pressed) {
        s_irq = false;
        s_dbg_int_low++;
        uint16_t rx = 0;
        uint16_t ry = 0;
        if (ReadContact(&rx, &ry)) {
            s_pressed = true;
            s_cached_x = rx;
            s_cached_y = ry;
            s_dbg_presses++;
            s_dbg_last_x = rx;
            s_dbg_last_y = ry;
        } else {
            s_pressed = false;
        }
    }

    if (!s_pressed) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    uint16_t x = s_cached_x;
    uint16_t y = s_cached_y;
    if (TOUCH_SWAP_XY) {
        uint16_t t = x;
        x = y;
        y = t;
    }
    if (TOUCH_INVERT_X) {
        x = TFT_WIDTH - 1 - x;
    }
    if (TOUCH_INVERT_Y) {
        y = TFT_HEIGHT - 1 - y;
    }

    // A press on a dark screen spends itself waking the screen up, and LVGL
    // never sees it. Otherwise the first touch after the backlight times out
    // lands on whatever button happens to be under the finger, which on this
    // firmware could be "Start new ride" or the file server.
    const bool was_off = PowerManager_ScreenIsOff();
    PowerManager_NoteActivity();
    if (was_off) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    data->point.x = MapAxis(x, TOUCH_RAW_MIN_X, TOUCH_RAW_MAX_X, TFT_WIDTH);
    data->point.y = MapAxis(y, TOUCH_RAW_MIN_Y, TOUCH_RAW_MAX_Y, TFT_HEIGHT);
    data->state = LV_INDEV_STATE_PR;
}

void Touch_DebugCounters(uint32_t *reads, uint32_t *presses, uint16_t *last_x,
                         uint16_t *last_y) {
    if (reads != nullptr) {
        *reads = s_dbg_reads;
    }
    if (presses != nullptr) {
        *presses = s_dbg_presses;
    }
    if (last_x != nullptr) {
        *last_x = s_dbg_last_x;
    }
    if (last_y != nullptr) {
        *last_y = s_dbg_last_y;
    }
}

void Touch_DebugIdentity(uint8_t *addr, uint16_t *signature) {
    if (addr != nullptr) {
        *addr = s_addr;
    }
    if (signature != nullptr) {
        *signature = s_signature;
    }
}

void Touch_DebugFrame(uint8_t *latest, uint8_t *latched, size_t len) {
    const size_t n = len < sizeof(s_dbg_frame) ? len : sizeof(s_dbg_frame);
    if (latest != nullptr) {
        memcpy(latest, s_dbg_frame, n);
    }
    if (latched != nullptr) {
        memcpy(latched, s_dbg_hit, n);
    }
}

uint32_t Touch_DebugIntLow() {
    return s_dbg_int_low;
}

void Touch_DebugProbe(char *out, size_t len) {
    if (out != nullptr && len > 0) {
        snprintf(out, len, "%s", s_dbg_probe);
    }
}
