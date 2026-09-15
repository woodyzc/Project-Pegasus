#include "Touch.h"

#include <Arduino.h>
#include <Wire.h>
#include "../system/PowerManager.h"

// CST328 (Hynitron) I2C address and the subset of its register map this
// driver needs. Addresses are 16-bit and go out big-endian -- the single
// biggest difference from the FT6336G this replaces, and the one that makes
// the controller look dead if you get it wrong.
static constexpr uint8_t CST328_I2C_ADDR = 0x1A;
static constexpr uint16_t REG_TOUCH_COUNT = 0xD005;  // low nibble = points, and the latch that must be cleared
static constexpr uint16_t REG_TOUCH_XY = 0xD000;     // 27 bytes: status + up to 5 packed points
static constexpr uint16_t REG_DEBUG_MODE = 0xD101;   // enter debug/info mode
static constexpr uint16_t REG_NORMAL_MODE = 0xD109;  // back to reporting touches
static constexpr uint16_t REG_INFO_TP_NTX = 0xD1F4;  // 24-byte info block; bytes 10..11 are the 0xCACA signature

// The touch panel has its own I2C bus on this board, separate from the
// sensor bus. Wire1 is it; Wire is deliberately untouched (see Touch.h).
static TwoWire &TOUCH_BUS = Wire1;

static bool s_controller_found = false;

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
    TOUCH_BUS.beginTransmission(CST328_I2C_ADDR);
    TOUCH_BUS.write((uint8_t)(reg >> 8));
    TOUCH_BUS.write((uint8_t)(reg & 0xFF));
    if (TOUCH_BUS.endTransmission(true) != 0) {
        return false;
    }
    if (TOUCH_BUS.requestFrom((int)CST328_I2C_ADDR, (int)len) != len) {
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
    TOUCH_BUS.beginTransmission(CST328_I2C_ADDR);
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

    // Drop into debug mode, read the info block, and check the signature the
    // vendor driver checks. Bytes 10..11 of the block at 0xD1F4 read back as
    // 0xCACA on a healthy controller.
    uint8_t info[24];
    WriteReg(REG_DEBUG_MODE, nullptr, 0);
    if (ReadReg(REG_INFO_TP_NTX, info, sizeof(info))) {
        const uint16_t signature = ((uint16_t)info[11] << 8) | info[10];
        s_controller_found = (signature == 0xCACA);
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

bool Touch_IsPressed() {
    uint8_t count = 0;
    if (!ReadReg(REG_TOUCH_COUNT, &count, 1)) {
        // An I2C read that failed is not a press. Reporting one would let a
        // loose connector skip the splash on every boot.
        return false;
    }
    ClearTouchLatch();
    return (count & 0x0F) != 0;
}

void Touch_Read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    uint8_t count = 0;
    if (!ReadReg(REG_TOUCH_COUNT, &count, 1)) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }
    count &= 0x0F;

    if (count == 0) {
        ClearTouchLatch();
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    // buf[0] is left for the status byte the vendor driver keeps at index 1 of
    // its own buffer, so the packed-point offsets below match theirs exactly.
    uint8_t buf[28];
    if (!ReadReg(REG_TOUCH_XY, &buf[1], 27)) {
        ClearTouchLatch();
        data->state = LV_INDEV_STATE_REL;
        return;
    }
    ClearTouchLatch();

    // Only the first contact matters -- this UI has no multi-touch gesture.
    // Point 0 is three bytes: X high 8, Y high 8, then a byte carrying X's low
    // nibble in the high half and Y's low nibble in the low half.
    const uint16_t raw_x = (uint16_t)(((uint16_t)buf[2] << 4) | ((buf[4] & 0xF0) >> 4));
    const uint16_t raw_y = (uint16_t)(((uint16_t)buf[3] << 4) | (buf[4] & 0x0F));

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
