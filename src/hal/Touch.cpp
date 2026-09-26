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
    // false = repeated START, not STOP.
    //
    // A STOP here ends the transaction, and this controller then treats the
    // following read as unrelated to the address just written: it acknowledges
    // its own address and clocks out nothing, so the master samples an
    // undriven bus. That is every symptom this bring-up produced -- 28 zero
    // bytes, then FF FF FF EF repeating, and before that "varied" bytes that
    // were noise being decoded as contacts at 1075,2563. The part never
    // returned register data at all; it only ever looked like it had.
    //
    // A bare address probe still works through a STOP, which is why
    // Touch_Init()'s detection found the part at 0x58 correctly while every
    // read after it failed.
    if (TOUCH_BUS.endTransmission(false) != 0) {
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
    // 100kHz, not the vendor's 400kHz.
    //
    // At 400kHz this part acknowledged its address on every boot -- detection
    // never once failed -- and then returned bytes that changed at random
    // from read to read: FF FF FF EF for a while, then noise. A slave that
    // ACKs reliably but cannot hold a multi-byte read together is the
    // signature of marginal signal integrity rather than a protocol fault.
    // The address byte is short enough to survive; a 28-byte read is not.
    //
    // The sensor bus runs 400kHz happily, which is not a counter-argument:
    // its two devices sit on the board while this one is across the panel's
    // flex, with whatever pull-ups the FPC provides.
    TOUCH_BUS.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL, 100000);

    // INPUT_PULLUP, not INPUT. The line is active-low **open-drain**: the
    // controller can pull it down and nothing can pull it up, so without a
    // pull-up it floats -- and it floated low, which read as "a contact is
    // waiting" on every single poll. The read counter and the interrupt
    // counter came back exactly equal, 190/190 and 315/315, which is what
    // that looks like from outside.
    //
    // The comment on this line has said "open-drain" since the port was
    // written, next to a pinMode that could not honour it.
    pinMode(TOUCH_INT_PIN, INPUT_PULLUP); // active-low, open-drain; also the deep-sleep wake source (see PowerManager)
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

    {
        uint8_t w2[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        uint8_t w1[4] = {0xFF, 0xFF, 0xFF, 0xFF};

        // 16-bit register address, D0 00 -- what the code does today.
        TOUCH_BUS.beginTransmission(s_addr);
        TOUCH_BUS.write((uint8_t)0xD0);
        TOUCH_BUS.write((uint8_t)0x00);
        const int e2 = TOUCH_BUS.endTransmission(false);
        if (TOUCH_BUS.requestFrom((int)s_addr, 4) == 4) {
            for (int i = 0; i < 4; i++) {
                w2[i] = (uint8_t)TOUCH_BUS.read();
            }
        }

        // 8-bit register address, 0x00 -- the CST816-family shape.
        TOUCH_BUS.beginTransmission(s_addr);
        TOUCH_BUS.write((uint8_t)0x00);
        const int e1 = TOUCH_BUS.endTransmission(false);
        if (TOUCH_BUS.requestFrom((int)s_addr, 4) == 4) {
            for (int i = 0; i < 4; i++) {
                w1[i] = (uint8_t)TOUCH_BUS.read();
            }
        }

        snprintf(s_dbg_probe, sizeof(s_dbg_probe),
                 "16b e%d %02X%02X%02X%02X | 8b e%d %02X%02X%02X%02X", e2, w2[0],
                 w2[1], w2[2], w2[3], e1, w1[0], w1[1], w1[2], w1[3]);
    }

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
        // Only while the controller says it has something.
        //
        // This is the difference between the init probe and every live read.
        // The probe runs once, straight after reset, with the part idle, and
        // has returned byte-identical data on every boot and at both clock
        // speeds. The live reads poll asynchronously about thirty times a
        // second and come back different every time -- because they sample
        // the registers while the controller is rewriting them.
        //
        // Linux's driver is interrupt-driven: it reads in response to INT,
        // never on a timer. Polling a part designed that way is reading a
        // frame that is being written underneath you, and no register map
        // would have made those bytes decode.
        if (digitalRead(TOUCH_INT_PIN) != LOW) {
            return false;
        }
        s_dbg_int_low++;

        // Seven bytes, not the kernel's twenty-eight.
        //
        // Two photos ten seconds apart settled this. The one-shot probe's
        // 4-byte reads came back identical in both, and identical again to
        // the run before at 400kHz -- stable, reproducible data. Meanwhile
        // the read counter advanced by 19 in ten seconds against an LVGL
        // poll rate of about 33 a second, so roughly nineteen of every twenty
        // 28-byte reads were failing outright and the survivors returned
        // zeros.
        //
        // Short reads work and long ones do not, which is a length limit
        // rather than the signal-integrity problem the changing bytes looked
        // like. Everything this driver needs is in the first seven bytes:
        // coordinates at 1..3, count at 5, check value at 6. The rest of the
        // kernel's 28 carry contacts two through five, which this UI has no
        // use for.
        uint8_t buf[7];
        if (!ReadReg(REG_TOUCH_XY, buf, sizeof(buf))) {
            return false;
        }
        s_dbg_reads++;
        memcpy(s_dbg_frame, buf, sizeof(buf));
        // Latched separately so a tap is still readable afterwards -- holding
        // a finger on the glass and photographing the panel at the same time
        // is not a thing one pair of hands does well.
        if ((buf[5] & CST3XX_TOUCH_COUNT_MASK) != 0 || (buf[0] & 0x0F) != 0) {
            memcpy(s_dbg_hit, buf, sizeof(buf));
        }

        // Byte 6 is a fixed check value. The other half of the phantom
        // contacts: with no validity test, a stale or half-written frame
        // decodes as a press at whatever coordinates happen to be in it --
        // which is exactly what 1075,2563 was on a 240x320 panel.
        if (buf[6] != CST3XX_CHK_VAL) {
            return false;
        }

        const uint8_t count = buf[5] & CST3XX_TOUCH_COUNT_MASK;
        if (count == 0) {
            return false;
        }

        // First contact only; this UI has no multi-touch gesture. Byte 3
        // carries X's low nibble in its high half and Y's in its low half.
        *raw_x = (uint16_t)(((uint16_t)buf[1] << 4) | ((buf[3] >> 4) & 0x0F));
        *raw_y = (uint16_t)(((uint16_t)buf[2] << 4) | (buf[3] & 0x0F));

        // Acknowledged only now, having actually consumed a frame.
        //
        // It used to be sent on every poll, rejected frames included, and that
        // silenced the part completely: reads kept succeeding and returned 28
        // zero bytes for as long as the board was up, while the one frame
        // latched before the first ack had real content in it. The command is
        // named STOP in the kernel's header for a reason -- sending it thirty
        // times a second is telling the controller to stop reporting, over and
        // over, faster than it can publish anything.
        AckTouchBlock();
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
