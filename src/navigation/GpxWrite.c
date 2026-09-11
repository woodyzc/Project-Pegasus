#include "GpxWrite.h"

#include <stdio.h>
#include <string.h>

// snprintf reports the length it *would* have written, so a return value at or
// beyond the buffer size means the output was truncated. Every writer below
// funnels through this so truncation can never reach the card.
static size_t Finish(int written, size_t out_size, char *out) {
    if (written < 0 || (size_t)written >= out_size) {
        if (out_size > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    return (size_t)written;
}

size_t GpxWrite_XmlEscape(char *out, size_t out_size, const char *in) {
    if (out == NULL || out_size == 0) {
        return 0;
    }
    if (in == NULL) {
        out[0] = '\0';
        return 0;
    }

    size_t used = 0;
    for (const char *p = in; *p != '\0'; p++) {
        const char *replacement = NULL;
        switch (*p) {
            case '&':  replacement = "&amp;";  break;
            case '<':  replacement = "&lt;";   break;
            case '>':  replacement = "&gt;";   break;
            case '"':  replacement = "&quot;"; break;
            case '\'': replacement = "&apos;"; break;
            default:   break;
        }

        if (replacement != NULL) {
            const size_t len = strlen(replacement);
            if (used + len >= out_size) {
                out[0] = '\0';
                return 0;
            }
            memcpy(out + used, replacement, len);
            used += len;
        } else {
            if (used + 1 >= out_size) {
                out[0] = '\0';
                return 0;
            }
            out[used++] = *p;
        }
    }

    out[used] = '\0';
    return used;
}

size_t GpxWrite_Header(char *out, size_t out_size, const char *track_name) {
    if (out == NULL || out_size == 0) {
        return 0;
    }

    // Truncate before escaping, not after: cutting escaped text could split
    // "&amp;" and put a bare "&am" in the file, which is not well-formed XML.
    char trimmed[GPX_WRITE_MAX_NAME + 1];
    const char *source = (track_name != NULL) ? track_name : "Pegasus ride";
    snprintf(trimmed, sizeof(trimmed), "%s", source);

    char escaped[(GPX_WRITE_MAX_NAME * 6) + 1];
    if (GpxWrite_XmlEscape(escaped, sizeof(escaped), trimmed) == 0 && trimmed[0] != '\0') {
        return 0;
    }

    const int written = snprintf(
        out, out_size,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<gpx version=\"1.1\" creator=\"Project Pegasus\"\n"
        "     xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
        "  <trk>\n"
        "    <name>%s</name>\n"
        "    <trkseg>\n",
        escaped);

    return Finish(written, out_size, out);
}

size_t GpxWrite_Point(char *out,
                      size_t out_size,
                      double lat,
                      double lon,
                      float alt_m,
                      bool has_time,
                      uint16_t year,
                      uint8_t month,
                      uint8_t day,
                      uint8_t hour,
                      uint8_t minute,
                      uint8_t second) {
    if (out == NULL || out_size == 0) {
        return 0;
    }

    // 7 decimal places is about 11mm at the equator -- past what the receiver
    // resolves, and the convention every GPX tool expects.
    int written;
    if (has_time) {
        written = snprintf(out, out_size,
                           "      <trkpt lat=\"%.7f\" lon=\"%.7f\">"
                           "<ele>%.1f</ele>"
                           "<time>%04u-%02u-%02uT%02u:%02u:%02uZ</time>"
                           "</trkpt>\n",
                           lat, lon, (double)alt_m,
                           (unsigned)year, (unsigned)month, (unsigned)day,
                           (unsigned)hour, (unsigned)minute, (unsigned)second);
    } else {
        written = snprintf(out, out_size,
                           "      <trkpt lat=\"%.7f\" lon=\"%.7f\">"
                           "<ele>%.1f</ele>"
                           "</trkpt>\n",
                           lat, lon, (double)alt_m);
    }

    return Finish(written, out_size, out);
}

size_t GpxWrite_Footer(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return 0;
    }
    const int written = snprintf(out, out_size,
                                 "    </trkseg>\n"
                                 "  </trk>\n"
                                 "</gpx>\n");
    return Finish(written, out_size, out);
}

size_t GpxWrite_FileName(char *out,
                         size_t out_size,
                         bool has_time,
                         uint16_t year,
                         uint8_t month,
                         uint8_t day,
                         uint8_t hour,
                         uint8_t minute,
                         uint8_t second,
                         unsigned sequence) {
    if (out == NULL || out_size == 0) {
        return 0;
    }

    int written;
    if (has_time) {
        written = snprintf(out, out_size, "/rides/%04u-%02u-%02u_%02u%02u%02u.gpx",
                           (unsigned)year, (unsigned)month, (unsigned)day,
                           (unsigned)hour, (unsigned)minute, (unsigned)second);
    } else {
        // %04u caps at four digits by convention, not by force -- a sequence
        // beyond 9999 simply produces a longer name, which is still unique.
        written = snprintf(out, out_size, "/rides/ride-%04u.gpx", sequence);
    }

    return Finish(written, out_size, out);
}
