#include "GpxParse.h"

#include <stdlib.h>
#include <string.h>

enum {
    ST_SEEK_TAG = 0, /* outside any tag, looking for '<' */
    ST_IN_TAG        /* collecting the tag body up to '>' */
};

/* Case-sensitive by design: GPX element names are lowercase in the schema, and
   accepting <TRKPT> would mean accepting files no other GPX reader would. */
static bool TagIsTrackPoint(const char *tag) {
    /* The name ends at the first space, '/' or end of string. */
    static const char *const NAMES[] = {"trkpt", "rtept"};
    size_t n;

    for (n = 0; n < sizeof(NAMES) / sizeof(NAMES[0]); n++) {
        const size_t len = strlen(NAMES[n]);
        if (strncmp(tag, NAMES[n], len) == 0) {
            const char after = tag[len];
            if (after == ' ' || after == '\t' || after == '\r' || after == '\n' ||
                after == '/' || after == '\0') {
                return true;
            }
        }
    }
    return false;
}

/* Finds `name="value"` (or single quotes) and converts the value. Returns false
   if the attribute is absent or its value is not a number. */
static bool ReadAttribute(const char *tag, const char *name, double *out) {
    const size_t name_len = strlen(name);
    const char *p = tag;

    while ((p = strstr(p, name)) != NULL) {
        const char *cursor = p + name_len;
        char quote;
        char *end = NULL;
        double value;

        /* Must be a whole attribute name: guards against `lat` matching inside
           some other attribute, and against matching the element name itself. */
        if (p != tag) {
            const char before = p[-1];
            if (before != ' ' && before != '\t' && before != '\r' && before != '\n') {
                p += name_len;
                continue;
            }
        }

        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (*cursor != '=') {
            p += name_len;
            continue;
        }
        cursor++;
        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }

        quote = *cursor;
        if (quote != '"' && quote != '\'') {
            p += name_len;
            continue;
        }
        cursor++;

        value = strtod(cursor, &end);
        if (end == cursor) {
            return false; /* attribute present but not a number */
        }
        /* The value must actually terminate at its closing quote; anything
           else means the file is malformed in a way worth rejecting. */
        while (*end == ' ' || *end == '\t') {
            end++;
        }
        if (*end != quote) {
            return false;
        }

        *out = value;
        return true;
    }
    return false;
}

void Gpx_Init(GpxParser_t *parser) {
    if (parser == NULL) {
        return;
    }
    parser->state = ST_SEEK_TAG;
    parser->index = 0;
    parser->overflowed = false;
    parser->buffer[0] = '\0';
}

bool Gpx_ParseTagAttributes(const char *tag, double *out_lat, double *out_lon) {
    double lat;
    double lon;

    if (tag == NULL || out_lat == NULL || out_lon == NULL) {
        return false;
    }
    if (!ReadAttribute(tag, "lat", &lat) || !ReadAttribute(tag, "lon", &lon)) {
        return false;
    }
    /* Out-of-range values are corruption, not data. Drawing a trail to them
       would put a line across the map to nowhere. */
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        return false;
    }

    *out_lat = lat;
    *out_lon = lon;
    return true;
}

bool Gpx_Feed(GpxParser_t *parser, char c, double *out_lat, double *out_lon) {
    if (parser == NULL || out_lat == NULL || out_lon == NULL) {
        return false;
    }

    switch (parser->state) {
        case ST_SEEK_TAG:
            if (c == '<') {
                parser->state = ST_IN_TAG;
                parser->index = 0;
                parser->overflowed = false;
            }
            break;

        case ST_IN_TAG:
            if (c == '>') {
                bool emitted = false;

                parser->buffer[parser->index] = '\0';
                if (!parser->overflowed && TagIsTrackPoint(parser->buffer)) {
                    emitted = Gpx_ParseTagAttributes(parser->buffer, out_lat, out_lon);
                }

                parser->state = ST_SEEK_TAG;
                parser->index = 0;
                parser->overflowed = false;
                return emitted;
            }

            if (c == '<') {
                /* A '<' inside a tag means the previous one was never closed.
                   Restart here rather than carrying the damage forward. */
                parser->index = 0;
                parser->overflowed = false;
                break;
            }

            if (parser->index < GPX_TAG_BUFFER - 1) {
                parser->buffer[parser->index++] = c;
            } else {
                /* Long tags are almost always <metadata> or a styling element,
                   not a track point. Mark and skip to the closing '>' rather
                   than parsing a truncated attribute list. */
                parser->overflowed = true;
            }
            break;

        default:
            Gpx_Init(parser);
            break;
    }
    return false;
}
