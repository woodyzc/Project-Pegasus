#include "TimeZone.h"

#include <stdio.h>

/* Regions are rectangles in (lat, lon), tested in order, so a smaller region
   nested inside a larger one must come first -- Arizona before Mountain,
   Tibet-side China before India. Rectangles cannot express a real timezone
   border; see the accuracy note in the header. */
typedef struct {
    double lat_min;
    double lat_max;
    double lon_min;
    double lon_max;
    const char *posix_tz;
} Zone;

static const Zone ZONES[] = {
    /* ---- United States ------------------------------------------------- */
    /* Arizona keeps standard time all year, so it must precede Mountain.
       (The Navajo Nation in its northeast corner does observe DST; that is
       exactly the kind of enclave a rectangle cannot express.) */
    {31.0, 37.1, -115.0, -109.0, "MST7"},
    {24.0, 50.0, -85.0, -66.0, "EST5EDT,M3.2.0,M11.1.0"},
    /* The Mountain/Central border follows Colorado's eastern edge near -102,
       not the -105 a longitude-band guess suggests: Denver is at -104.99 and
       is Mountain. Getting this wrong is a whole hour, in a state the border
       runs right past. */
    {24.0, 50.0, -102.0, -85.0, "CST6CDT,M3.2.0,M11.1.0"},
    {24.0, 50.0, -115.0, -102.0, "MST7MDT,M3.2.0,M11.1.0"},
    {30.0, 50.0, -125.0, -115.0, "PST8PDT,M3.2.0,M11.1.0"},
    {51.0, 72.0, -170.0, -129.0, "AKST9AKDT,M3.2.0,M11.1.0"},
    {18.0, 23.5, -161.0, -154.0, "HST10"},

    /* ---- Canada (broad bands; the Prairies are messier than this) ------- */
    {42.0, 84.0, -142.0, -120.0, "PST8PDT,M3.2.0,M11.1.0"},
    {42.0, 84.0, -120.0, -102.0, "MST7MDT,M3.2.0,M11.1.0"},
    {42.0, 84.0, -102.0, -85.0, "CST6CDT,M3.2.0,M11.1.0"},
    {42.0, 84.0, -85.0, -60.0, "EST5EDT,M3.2.0,M11.1.0"},

    /* ---- Europe -------------------------------------------------------- */
    {35.0, 61.0, -11.0, 2.0, "GMT0BST,M3.5.0/1,M10.5.0/2"},
    {35.0, 62.0, 2.0, 17.0, "CET-1CEST,M3.5.0,M10.5.0/3"},
    {34.0, 62.0, 17.0, 32.0, "EET-2EEST,M3.5.0/3,M10.5.0/4"},

    /* ---- Asia-Pacific -------------------------------------------------- */
    /* China before India: their rectangles overlap across Tibet, and the
       whole country runs on a single offset with no DST. */
    {18.0, 54.0, 73.0, 135.0, "CST-8"},
    {30.0, 46.0, 129.0, 146.0, "JST-9"},
    {33.0, 39.0, 124.0, 132.0, "KST-9"},
    {6.0, 36.0, 68.0, 73.0, "IST-5:30"},
    {1.0, 8.0, 100.0, 105.0, "SGT-8"},
    {-44.0, -10.0, 141.0, 154.0, "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {-44.0, -10.0, 129.0, 141.0, "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {-36.0, -13.0, 112.0, 129.0, "AWST-8"},
    {-48.0, -34.0, 166.0, 179.0, "NZST-12NZDT,M9.5.0,M4.1.0/3"},

    /* ---- South America ------------------------------------------------- */
    {-34.0, 5.0, -74.0, -34.0, "BRT3"},
    {-56.0, -21.0, -74.0, -53.0, "ART3"},
};

static const size_t ZONE_COUNT = sizeof(ZONES) / sizeof(ZONES[0]);

/* Buffer for the longitude fallback. Single-threaded by contract: only the
   Core 1 UI asks for a timezone, once per position update. */
static char s_fallback[16];

const char *TimeZone_PosixFor(double lat_deg, double lon_deg, bool *out_approximate) {
    size_t i;

    if (out_approximate != NULL) {
        *out_approximate = false;
    }

    for (i = 0; i < ZONE_COUNT; i++) {
        if (lat_deg >= ZONES[i].lat_min && lat_deg <= ZONES[i].lat_max &&
            lon_deg >= ZONES[i].lon_min && lon_deg <= ZONES[i].lon_max) {
            return ZONES[i].posix_tz;
        }
    }

    if (out_approximate != NULL) {
        *out_approximate = true;
    }

    {
        /* Solar time: one hour per 15 degrees, nearest hour. No DST, and no
           attempt to guess one -- a wrong DST offset is worse than a known
           approximation. Clamped to the real -12..+14 range so a bad fix
           cannot produce a nonsense TZ string. */
        int offset = (int)((lon_deg / 15.0) + (lon_deg >= 0 ? 0.5 : -0.5));
        if (offset > 14) {
            offset = 14;
        }
        if (offset < -12) {
            offset = -12;
        }
        /* POSIX signs are inverted: UTC+2 is written "UTC-2". */
        snprintf(s_fallback, sizeof(s_fallback), "UTC%+d", -offset);
        return s_fallback;
    }
}

int64_t TimeZone_UtcToEpoch(uint16_t year, uint8_t month, uint8_t day,
                            uint8_t hour, uint8_t minute, uint8_t second) {
    /* days_from_civil (Howard Hinnant): shifts the year to start in March so
       the leap day lands at the end of the cycle, which removes every special
       case for February. Valid across the whole proleptic Gregorian range. */
    int64_t y = (int64_t)year;
    const int64_t m = (int64_t)month;
    const int64_t d = (int64_t)day;

    int64_t era;
    int64_t yoe;
    int64_t doy;
    int64_t doe;
    int64_t days;

    y -= (m <= 2) ? 1 : 0;
    era = ((y >= 0) ? y : y - 399) / 400;
    yoe = y - era * 400;                                        /* [0, 399] */
    doy = (153 * (m + ((m > 2) ? -3 : 9)) + 2) / 5 + d - 1;     /* [0, 365] */
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                /* [0, 146096] */
    days = era * 146097 + doe - 719468;                         /* since 1970-01-01 */

    return days * 86400 + (int64_t)hour * 3600 + (int64_t)minute * 60 + (int64_t)second;
}
