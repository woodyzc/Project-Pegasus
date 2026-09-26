#include "AlertFrame.h"

#include <string.h>

bool Alert_ParseFrame(const uint8_t *data, size_t length, AlertFrame_t *out) {
    if (data == NULL || out == NULL || length < ALERT_HEADER_LEN) {
        return false;
    }
    if (data[0] != ALERT_FRAME_MAGIC || data[1] != ALERT_FRAME_VERSION) {
        return false;
    }

    const uint8_t kind = data[2];
    if (kind >= ALERT_KIND_COUNT) {
        // An unknown kind is a sender this build does not understand. Showing
        // it under some default label would put a name on screen next to the
        // wrong idea of what just happened.
        return false;
    }

    const uint8_t name_len = data[3];
    if (name_len > ALERT_NAME_MAX) {
        return false;
    }
    // The declared length must match what actually arrived. A short write is
    // otherwise indistinguishable from a full one, and the missing bytes would
    // be read out of whatever follows the buffer.
    if (length != (size_t)ALERT_HEADER_LEN + name_len) {
        return false;
    }

    const uint8_t count = data[4];
    if (count == 0) {
        // Zero would mean an alert standing for no messages, which is not a
        // thing the phone can honestly send.
        return false;
    }

    // data[5] is reserved and ignored rather than rejected, so a later version
    // can use it without this build refusing the whole frame.

    out->kind = (AlertKind_t)kind;
    out->count = count;
    // Zeroed whole, not just terminated. Callers copy sizeof(name) rather
    // than strlen -- BLE_TBT_Receiver does, into a struct it publishes on the
    // data bus -- and terminating alone would leave the bytes past the NUL
    // holding whatever was on the caller's stack, which then travels into
    // shared state. Harmless while everything reads to the terminator, and
    // not worth relying on.
    memset(out->name, 0, sizeof(out->name));
    memcpy(out->name, data + ALERT_HEADER_LEN, name_len);
    return true;
}

const char *Alert_KindText(AlertKind_t kind) {
    switch (kind) {
        case ALERT_KIND_CALL: return "Call";
        case ALERT_KIND_SMS:  return "SMS";
        case ALERT_KIND_CHAT: return "WeChat";
        default:              return "";
    }
}
