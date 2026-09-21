#include "NmeaParse.h"

#include <stdlib.h>
#include <string.h>

static int HexNibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* Fields are comma-separated starting right after '$'; field 0 is the
   5-character talker+sentence id itself. A field's scan stops at the next
   comma, at '*' (the checksum marker), or at the end of the buffer. */
static bool GetField(const char *s, size_t len, int index, const char **out, size_t *out_len) {
    size_t start;
    size_t i;
    int field;

    if (s == NULL || len < 1 || s[0] != '$') {
        return false;
    }

    start = 1;
    field = 0;
    for (i = 1; i <= len; i++) {
        const bool is_end = (i == len) || s[i] == ',' || s[i] == '*';
        if (!is_end) {
            continue;
        }
        if (field == index) {
            *out = s + start;
            *out_len = i - start;
            return true;
        }
        field++;
        start = i + 1;
        if (i < len && s[i] == '*') {
            return false; /* ran into the checksum marker first */
        }
    }
    return false;
}

/* Copies a field into a NUL-terminated buffer. An empty field (two
   consecutive commas) is a legitimate "not reported" and copies as "". */
static bool FieldToBuf(const char *s, size_t len, int index, char *buf, size_t bufsize) {
    const char *f;
    size_t flen;

    if (!GetField(s, len, index, &f, &flen)) {
        return false;
    }
    if (flen >= bufsize) {
        return false;
    }
    memcpy(buf, f, flen);
    buf[flen] = '\0';
    return true;
}

/* ddmm.mmmm / dddmm.mmmm -> signed decimal degrees. The two digits
   immediately before the decimal point are always minutes, however many
   digits of degrees precede them -- true for both a 2-digit latitude and a
   3-digit longitude, so one routine covers both. */
static bool ParseCoord(const char *tok, char hemi, double *out_deg) {
    const char *dot;
    size_t int_len;
    size_t deg_len;
    char deg_buf[8];
    double degrees;
    double minutes;
    double value;

    if (tok == NULL || tok[0] == '\0') {
        return false;
    }
    dot = strchr(tok, '.');
    if (dot == NULL) {
        return false;
    }
    int_len = (size_t)(dot - tok);
    if (int_len < 2) {
        return false;
    }
    deg_len = int_len - 2;
    if (deg_len >= sizeof(deg_buf)) {
        return false;
    }
    if (deg_len > 0) {
        memcpy(deg_buf, tok, deg_len);
        deg_buf[deg_len] = '\0';
        degrees = atof(deg_buf);
    } else {
        degrees = 0.0;
    }
    minutes = atof(tok + deg_len);
    value = degrees + minutes / 60.0;

    if (hemi == 'S' || hemi == 'W') {
        value = -value;
    } else if (hemi != 'N' && hemi != 'E') {
        return false;
    }

    *out_deg = value;
    return true;
}

bool Nmea_ChecksumOk(const char *sentence, size_t length) {
    size_t i;
    uint8_t sum = 0;
    size_t star = 0;
    bool found_star = false;

    if (sentence == NULL || length < 4 || sentence[0] != '$') {
        return false;
    }

    for (i = 1; i < length; i++) {
        if (sentence[i] == '*') {
            star = i;
            found_star = true;
            break;
        }
        sum ^= (uint8_t)sentence[i];
    }
    if (!found_star || star + 2 >= length) {
        return false;
    }

    {
        const int hi = HexNibble(sentence[star + 1]);
        const int lo = HexNibble(sentence[star + 2]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        return sum == (uint8_t)((hi << 4) | lo);
    }
}

bool Nmea_DecodeGGA(const char *sentence, size_t length, NmeaFix_t *out) {
    char lat_buf[16];
    char lon_buf[16];
    char ns_buf[4];
    char ew_buf[4];
    char q_buf[4];
    char sv_buf[4];
    char alt_buf[16];
    double lat_deg;
    double lon_deg;

    if (sentence == NULL || out == NULL) {
        return false;
    }

    if (!FieldToBuf(sentence, length, 2, lat_buf, sizeof(lat_buf)) ||
        !FieldToBuf(sentence, length, 3, ns_buf, sizeof(ns_buf)) ||
        !FieldToBuf(sentence, length, 4, lon_buf, sizeof(lon_buf)) ||
        !FieldToBuf(sentence, length, 5, ew_buf, sizeof(ew_buf)) ||
        !FieldToBuf(sentence, length, 6, q_buf, sizeof(q_buf)) ||
        !FieldToBuf(sentence, length, 7, sv_buf, sizeof(sv_buf)) ||
        !FieldToBuf(sentence, length, 9, alt_buf, sizeof(alt_buf))) {
        return false;
    }

    if (q_buf[0] == '\0' || sv_buf[0] == '\0') {
        return false;
    }

    out->fix_quality = (uint8_t)atoi(q_buf);
    out->fix_valid = out->fix_quality != 0;
    out->num_sv = (uint8_t)atoi(sv_buf);

    /* Still acquiring: lat/lon/altitude are legitimately blank rather than
       zero, and 0,0 is a real place (the Gulf of Guinea) -- leave position
       untouched rather than reporting it. num_sv above still climbs, which
       is what tells a rider apart from a missing module. */
    if (out->fix_valid) {
        if (lat_buf[0] == '\0' || lon_buf[0] == '\0') {
            return false;
        }
        if (!ParseCoord(lat_buf, ns_buf[0], &lat_deg) ||
            !ParseCoord(lon_buf, ew_buf[0], &lon_deg)) {
            return false;
        }
        out->lat_deg = lat_deg;
        out->lon_deg = lon_deg;
        out->alt_m = (alt_buf[0] != '\0') ? (float)atof(alt_buf) : 0.0f;
    }

    return true;
}

bool Nmea_DecodeRMC(const char *sentence, size_t length, NmeaFix_t *out) {
    char time_buf[16];
    char status_buf[4];
    char speed_buf[16];
    char course_buf[16];
    char date_buf[16];

    if (sentence == NULL || out == NULL) {
        return false;
    }

    if (!FieldToBuf(sentence, length, 1, time_buf, sizeof(time_buf)) ||
        !FieldToBuf(sentence, length, 2, status_buf, sizeof(status_buf)) ||
        !FieldToBuf(sentence, length, 7, speed_buf, sizeof(speed_buf)) ||
        !FieldToBuf(sentence, length, 8, course_buf, sizeof(course_buf)) ||
        !FieldToBuf(sentence, length, 9, date_buf, sizeof(date_buf))) {
        return false;
    }

    out->time_valid = (status_buf[0] == 'A');

    /* Knots on the wire; the rest of this firmware works in m/s. */
    out->speed_mps = (speed_buf[0] != '\0') ? (float)(atof(speed_buf) * 0.514444) : 0.0f;

    /* Course over ground is blank on a stationary receiver -- a stopped
       rider must not read as "heading due north". */
    if (course_buf[0] != '\0') {
        out->heading_deg = (float)atof(course_buf);
        out->heading_valid = true;
    } else {
        out->heading_deg = 0.0f;
        out->heading_valid = false;
    }

    if (out->time_valid && strlen(time_buf) >= 6 && strlen(date_buf) >= 6) {
        out->hour = (uint8_t)((time_buf[0] - '0') * 10 + (time_buf[1] - '0'));
        out->minute = (uint8_t)((time_buf[2] - '0') * 10 + (time_buf[3] - '0'));
        out->second = (uint8_t)((time_buf[4] - '0') * 10 + (time_buf[5] - '0'));
        out->day = (uint8_t)((date_buf[0] - '0') * 10 + (date_buf[1] - '0'));
        out->month = (uint8_t)((date_buf[2] - '0') * 10 + (date_buf[3] - '0'));
        out->year = (uint16_t)(2000 + (date_buf[4] - '0') * 10 + (date_buf[5] - '0'));
    } else {
        out->time_valid = false;
    }

    return true;
}

void Nmea_Init(NmeaParser_t *parser) {
    if (parser == NULL) {
        return;
    }
    parser->len = 0;
    parser->in_line = false;
    parser->overflowed = false;
    parser->have_rmc = false;
    parser->rmc_speed_mps = 0.0f;
    parser->rmc_heading_deg = 0.0f;
    parser->rmc_heading_valid = false;
    parser->rmc_time_valid = false;
    parser->rmc_year = 0;
    parser->rmc_month = 0;
    parser->rmc_day = 0;
    parser->rmc_hour = 0;
    parser->rmc_minute = 0;
    parser->rmc_second = 0;
}

bool Nmea_Feed(NmeaParser_t *parser, uint8_t byte, NmeaFix_t *out) {
    if (parser == NULL || out == NULL) {
        return false;
    }

    if (byte == '$') {
        /* Always a resync point, even mid-line: a '$' that shows up before
           the previous sentence terminated means that one is not trustworthy
           anyway, the same tolerance UbxParse gives 0xB5. */
        parser->in_line = true;
        parser->overflowed = false;
        parser->len = 0;
        parser->line[parser->len++] = '$';
        return false;
    }

    if (byte == '\r' || byte == '\n') {
        const bool ready = parser->in_line && !parser->overflowed && parser->len > 0;
        const size_t len = parser->len;
        parser->in_line = false;
        parser->len = 0;
        parser->overflowed = false;
        if (!ready) {
            return false;
        }

        parser->line[len] = '\0';

        if (!Nmea_ChecksumOk(parser->line, len) || len < 6) {
            return false;
        }

        if (memcmp(parser->line + 3, "GGA", 3) == 0) {
            memset(out, 0, sizeof(*out));
            if (parser->have_rmc) {
                out->speed_mps = parser->rmc_speed_mps;
                out->heading_deg = parser->rmc_heading_deg;
                out->heading_valid = parser->rmc_heading_valid;
                out->time_valid = parser->rmc_time_valid;
                out->year = parser->rmc_year;
                out->month = parser->rmc_month;
                out->day = parser->rmc_day;
                out->hour = parser->rmc_hour;
                out->minute = parser->rmc_minute;
                out->second = parser->rmc_second;
            }
            return Nmea_DecodeGGA(parser->line, len, out);
        }

        if (memcmp(parser->line + 3, "RMC", 3) == 0) {
            NmeaFix_t tmp;
            memset(&tmp, 0, sizeof(tmp));
            if (Nmea_DecodeRMC(parser->line, len, &tmp)) {
                parser->rmc_speed_mps = tmp.speed_mps;
                parser->rmc_heading_deg = tmp.heading_deg;
                parser->rmc_heading_valid = tmp.heading_valid;
                parser->rmc_time_valid = tmp.time_valid;
                parser->rmc_year = tmp.year;
                parser->rmc_month = tmp.month;
                parser->rmc_day = tmp.day;
                parser->rmc_hour = tmp.hour;
                parser->rmc_minute = tmp.minute;
                parser->rmc_second = tmp.second;
                parser->have_rmc = true;
            }
            return false;
        }

        return false;
    }

    if (parser->in_line && !parser->overflowed) {
        if (parser->len < NMEA_MAX_LINE - 1) {
            parser->line[parser->len++] = (char)byte;
        } else {
            parser->overflowed = true;
        }
    }
    return false;
}
