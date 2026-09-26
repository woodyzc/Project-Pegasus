#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Decodes a phone alert -- a call, a text, a chat message -- pushed over BLE.
//
// ---------------------------------------------------------------------------
// What a rider is allowed to be told
// ---------------------------------------------------------------------------
// WHO, and nothing else. No message body, on purpose.
//
// A rider cannot answer and should not be reading prose at 25 km/h, so the
// only thing worth their attention is whether this is worth stopping for --
// and "Mum" answers that where two lines of text do not. It also keeps the
// frame small, keeps Chinese names inside one BLE write, and means a glance
// costs a fraction of a second rather than a sentence.
//
// ---------------------------------------------------------------------------
// Wire format (little-endian, one BLE write == one alert)
// ---------------------------------------------------------------------------
//
//   off  size  field
//   0    1     magic       0x41 ('A')
//   1    1     version     0x01
//   2    1     kind        AlertKind_t
//   3    1     name_len    0..ALERT_NAME_MAX bytes of UTF-8 that follow
//   4    1     count       how many messages this one alert stands for, 1..255
//   5    1     reserved    0
//   6    n     name        UTF-8, NOT NUL-terminated on the wire
//
// Total 6..54 bytes.
//
// `count` exists because a group chat produces a notification every few
// seconds and a banner per message would be a hazard rather than a feature.
// The phone coalesces within a window and says how many it swallowed; the
// panel can then show "WeChat - 6 messages" instead of six banners.
//
// The name is bytes, not characters, and the length is in bytes -- a Chinese
// name is three bytes per character in UTF-8, so 48 bytes is about sixteen
// characters, which is roughly what fits the panel at a readable size.

#ifdef __cplusplus
extern "C" {
#endif

#define ALERT_FRAME_MAGIC 0x41
#define ALERT_FRAME_VERSION 1
#define ALERT_HEADER_LEN 6
#define ALERT_NAME_MAX 48

typedef enum {
    ALERT_KIND_CALL = 0, // incoming call, ringing now
    ALERT_KIND_SMS = 1,
    ALERT_KIND_CHAT = 2, // WeChat and anything like it
    ALERT_KIND_COUNT
} AlertKind_t;

typedef struct {
    AlertKind_t kind;
    uint8_t count;                    // messages this alert stands for, >= 1
    char name[ALERT_NAME_MAX + 1];    // NUL-terminated here, unlike the wire
} AlertFrame_t;

// Returns false and touches nothing on a frame whose magic, version, kind or
// declared length is out of range.
//
// Rejected whole rather than partially applied, like every other frame in this
// firmware: a banner naming someone who did not call is worse than no banner.
bool Alert_ParseFrame(const uint8_t *data, size_t length, AlertFrame_t *out);

// A short label for the kind, for the panel to put in front of the name.
const char *Alert_KindText(AlertKind_t kind);

#ifdef __cplusplus
}
#endif
