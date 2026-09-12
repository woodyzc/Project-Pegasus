#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Which clock the head unit is believing, and why.
//
// Three sources, deliberately ranked rather than merged. GNSS is an atomic
// clock relayed by satellite; the phone is a handset that was itself told the
// time by a cell network; and between updates there is only this chip's own
// oscillator counting milliseconds. Letting a lower rank overwrite a higher
// one would mean a rider passing through a tunnel had their satellite time
// replaced by a phone's.
//
// The ranking is on the TIME. The zone is a separate question and the ranking
// there is the opposite way round: GNSS knows the instant but not what a human
// standing there would call it, while the phone knows exactly, because its
// owner set it. So a fix supplies the time and the phone still supplies the
// offset.
typedef enum {
    TIME_SRC_NONE = 0, // nothing has ever set the clock
    TIME_SRC_PHONE = 1,
    TIME_SRC_GNSS = 2,
} TimeSourceKind_t;

typedef struct {
    uint32_t utc_seconds;  // now, in seconds since the Unix epoch
    int16_t offset_min;    // local time minus UTC, in minutes
    bool offset_known;     // false when nobody has said; treat as UTC
    char zone[8];          // "EDT", "CEST", ... empty when unknown
    uint8_t kind;          // a TimeSourceKind_t
} TimeReading_t;

#ifdef __cplusplus
extern "C" {
#endif

// How long a source is believed after its last update.
//
// The phone stops writing when the rider walks away from the bike, and this
// chip's oscillator is not a timekeeping part -- its drift over hours has
// never been measured on this board. Rather than show an hour that is quietly
// wrong, the clock goes back to dashes once nothing has confirmed it for this
// long. Six hours is far longer than a ride and far shorter than a weekend.
#define TIME_SOURCE_STALE_MS (6u * 60u * 60u * 1000u)

// Feeds a reading in. Both take a monotonic millisecond tick, which is what
// advances the clock between updates; the caller passes it so this module
// needs no Arduino or FreeRTOS dependency and can be reasoned about on paper.
//
// A phone reading never displaces a GNSS one while the GNSS reading is still
// fresh, but its offset and zone are always taken -- see the ranking note
// above.
void TimeSource_SetFromPhone(uint32_t utc_seconds, int16_t offset_min, const char *zone,
                             uint32_t now_ms);
void TimeSource_SetFromGnss(uint32_t utc_seconds, uint32_t now_ms);

// The current best reading. False when nothing has ever set the clock, or
// when everything that did has gone stale.
bool TimeSource_Now(uint32_t now_ms, TimeReading_t *out);

// Forgets everything. For tests, and for a settings-page reset.
void TimeSource_Reset(void);

// How many phone readings have been accepted, and how many frames the parser
// threw out. Surfaced on the settings page, because "the clock is blank" has
// three very different causes that look identical from the outside: the phone
// never wrote, it wrote something malformed, or it wrote hours ago and the
// reading has gone stale. Serial is unusable on this board, so the panel is
// where diagnostics have to live.
void TimeSource_NotePhoneRejected(void);
uint32_t TimeSource_PhoneAccepted(void);
uint32_t TimeSource_PhoneRejected(void);

#ifdef __cplusplus
}
#endif
