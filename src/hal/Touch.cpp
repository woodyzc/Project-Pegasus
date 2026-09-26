#include "Touch.h"

#include <Arduino.h>
#include <Wire.h>
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

// Does anything acknowledge this address? A bare address probe, so it asks
// only "is a device electrically present" without assuming a register map.
static bool AddressAcks(uint8_t addr) {
    TOUCH_BUS.beginTransmission(addr);
    return TOUCH_BUS.endTransmission(true) == 0;
}

void Touch_Init() {
    TOUCH_BUS.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL, 400000);

    pinMode(TOUCH_INT_PIN, INPUT); // active-low, open-drain; the deep-sleep wake source (see PowerManager)
    pinMode(TOUCH_RST_PIN, OUTPUT);

    // Vendor reset sequence, timings included. It starts by driving RST high
    // rather than assuming a level, so a warm restart gets a real edge.
    digitalWrite(TOUCH_RST_PIN, HIGH);
    delay(50);
    digitalWrite(TOUCH_RST_PIN, LOW);
    delay(5);
    digitalWrite(TOUCH_RST_PIN, HIGH);
    delay(50);

    // Which part is fitted, asked rather than assumed. After the reset, so the
    // controller is awake enough to acknowledge.
    if (AddressAcks(CST328_I2C_ADDR)) {
        s_addr = CST328_I2C_ADDR;
    } else if (AddressAcks(CST3530_I2C_ADDR)) {
        s_addr = CST3530_I2C_ADDR;
    } else {
        s_addr = 0;
        return; // nothing on the bus; every accessor below is a no-op
    }
    s_controller_found = true;

    // Drop into debug mode and read the info block. Bytes 10..11 of the block
    // at 0xD1F4 read back as 0xCACA on a CST328.
    //
    // This is recorded, NOT used to decide whether a controller is present --
    // that decision now belongs to the address probe above. Hynitron publishes
    // no register appendix for the CST3530, so whether it carries the same
    // signature is unknown, and a driver that refused to talk to a part that
    // had just acknowledged its own address would be choosing an undocumented
    // magic number over an ACK.
    uint8_t info[24];
    WriteReg(REG_DEBUG_MODE, nullptr, 0);
    if (ReadReg(REG_INFO_TP_NTX, info, sizeof(info))) {
        s_signature = (uint16_t)(((uint16_t)info[11] << 8) | info[10]);
    }
    WriteReg(REG_NORMAL_MODE, nullptr, 0);

    // Start from a clean latch. Otherwise a press that happened during
    // bring-up is still sitting in the count register, and the boot splash --
    // which skips on the first touch it sees -- skips itself.
    ClearTouchLatch();
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
    if (s_addr == CST3530_I2C_ADDR) {
        // The whole block in one transaction. The count is byte 5 of it, so
        // the separate read of 0xD005 the CST328 path does is not merely
        // redundant here -- it reads a frame the controller has not finished
        // publishing, which is half of why an untouched panel reported a
        // contact in three reads out of four.
        uint8_t buf[28];
        if (!ReadReg(REG_TOUCH_XY, buf, sizeof(buf))) {
            return false;
        }
        s_dbg_reads++;

        // Byte 6 is a fixed check value. The other half of the phantom
        // contacts: with no validity test, a stale or half-written frame
        // decodes as a press at whatever coordinates happen to be in it --
        // which is exactly what 1075,2563 was on a 240x320 panel.
        if (buf[6] != CST3XX_CHK_VAL) {
            AckTouchBlock();
            return false;
        }

        const uint8_t count = buf[5] & CST3XX_TOUCH_COUNT_MASK;
        AckTouchBlock();
        if (count == 0) {
            return false;
        }

        // First contact only; this UI has no multi-touch gesture. Byte 3
        // carries X's low nibble in its high half and Y's in its low half.
        *raw_x = (uint16_t)(((uint16_t)buf[1] << 4) | ((buf[3] >> 4) & 0x0F));
        *raw_y = (uint16_t)(((uint16_t)buf[2] << 4) | (buf[3] & 0x0F));
        return true;
    }

    // ---- CST328 (V1 board), as the vendor demo describes it. Untested: no
    // V1 board has ever been on this bench. ----
    uint8_t count = 0;
    if (!ReadReg(REG_TOUCH_COUNT, &count, 1)) {
        return false;
    }
    s_dbg_reads++;
    count &= 0x0F;
    if (count == 0) {
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
    uint16_t raw_x = 0;
    uint16_t raw_y = 0;
    if (!ReadContact(&raw_x, &raw_y)) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    s_dbg_presses++;
    s_dbg_last_x = raw_x;
    s_dbg_last_y = raw_y;

    uint16_t x = raw_x;
    uint16_t y = raw_y;
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
