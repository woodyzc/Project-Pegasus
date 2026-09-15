#pragma once

#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// What a ride amounted to, and how to say it.
//
// A ride used to end with nothing: the numbers were on the dashboard while it
// happened and gone the moment the next one started. Every figure here already
// existed somewhere -- Trip, RideStats, Ascent, RideLog -- and none of them
// were ever shown together.
//
// The struct is a snapshot rather than a live view, because the one moment it
// is most wanted is the instant the rider starts a new ride, by which time the
// live values have been reset.
//
// Kept free of Arduino and LVGL so the formatting can be host-tested. The
// duration is the reason: hours, minutes and seconds have three cases and two
// of them only appear after a long day out.

#ifdef __cplusplus
extern "C" {
#endif

// Longest string RideSummary_FormatDuration writes, including the NUL.
#define RIDE_SUMMARY_TIME_MAX 12

typedef struct {
    double distance_km;    // from the trip odometer, which has its own floor
    double moving_seconds; // stopped time excluded, as the average is
    float avg_kmh;
    float max_kmh;
    uint8_t avg_bpm; // 0 when no strap was heard
    uint8_t max_bpm;
    float ascent_m;
    uint32_t points;  // trackpoints written to the card
    char file[48];    // the .gpx it went to, or "" if nothing was recorded
} RideSummary_t;

// "h:mm:ss" past an hour, "m:ss" below one, and "0:00" for nothing.
//
// Not "00:12:34" for a twelve-minute ride: the leading zeros are noise on a
// number a rider reads at a glance, and the colon count already says which
// unit is which. Writes nothing and returns false if the buffer is too small.
bool RideSummary_FormatDuration(uint32_t seconds, char *out, size_t out_size);

// The same duration, in five glyphs: "m:ss" below an hour, "h:mm" from one.
//
// For a cell too narrow to hold "6:26:14", which at any large size is most of
// them. Dropping the seconds past an hour costs nothing a rider reads -- after
// six hours nobody is counting them -- while below an hour they are the half
// that is still changing.
//
// Both forms are at most five glyphs, which is what makes the cell sizeable at
// all: a format that was five glyphs sometimes and seven others would have to
// be laid out for the seven.
#define RIDE_SUMMARY_SHORT_TIME_MAX 8
bool RideSummary_FormatDurationShort(uint32_t seconds, char *out, size_t out_size);

// True when there is nothing worth showing -- no distance and no moving time.
// A rider who presses "start new ride" twice in a row should not be handed an
// empty report the second time.
bool RideSummary_IsEmpty(const RideSummary_t *s);

#ifdef __cplusplus
}
#endif
