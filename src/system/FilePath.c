#include "FilePath.h"

#include <string.h>

// Longest single file name accepted. FAT long names go to 255, but the join
// has to fit FILEPATH_MAX alongside the longest directory, and a name past
// this is far more likely to be an attack than a file anyone meant to send.
#define LEAF_MAX 96

static bool StrEqual(const char *a, const char *b) {
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

bool FilePath_DirAllowed(const char *dir) {
    return StrEqual(dir, FILEPATH_DIR_ROOT) || StrEqual(dir, FILEPATH_DIR_RIDES) ||
           StrEqual(dir, FILEPATH_DIR_MAP);
}

bool FilePath_LeafSafe(const char *leaf) {
    if (leaf == NULL || leaf[0] == '\0') {
        return false;
    }
    // No leading dot. This covers "." and ".." without naming them, and it
    // also refuses the bare extension ".gpx", which would otherwise pass the
    // card root's suffix rule while being a dotfile rather than a route. The
    // incidental benefit is that a card used on a Mac is full of ".DS_Store"
    // and "._" companions, and none of them are anyone's ride.
    if (leaf[0] == '.') {
        return false;
    }

    size_t len = 0;
    for (const char *p = leaf; *p != '\0'; p++) {
        // Signed char on this toolchain, so a byte above 0x7F reads as
        // negative; compare as unsigned or every UTF-8 name is rejected as a
        // control character.
        const unsigned char c = (unsigned char)*p;

        // A separator in either spelling is what a traversal is made of, and
        // this is a leaf, so neither belongs here at all.
        if (c == '/' || c == '\\') {
            return false;
        }
        // Control characters and DEL. A newline in a name would let a crafted
        // upload split a header on anything that echoes the name back.
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
        // Reserved on FAT, and ':' would let a name look like a drive or a
        // stream to something reading the path later.
        if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            return false;
        }

        len++;
        if (len > LEAF_MAX) {
            return false;
        }
    }
    return true;
}

// Case-insensitive suffix match, written out rather than reached for via
// strcasecmp so this file stays free of anything platform-specific.
static bool EndsWithIgnoreCase(const char *s, const char *suffix) {
    const size_t s_len = strlen(s);
    const size_t suffix_len = strlen(suffix);
    if (s_len < suffix_len) {
        return false;
    }
    const char *tail = s + (s_len - suffix_len);
    for (size_t i = 0; i < suffix_len; i++) {
        char a = tail[i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool FilePath_LeafAllowedIn(const char *dir, const char *leaf) {
    if (!FilePath_DirAllowed(dir) || !FilePath_LeafSafe(leaf)) {
        return false;
    }
    // The card root is the owner's own folder, not a share. Routes are what
    // this server is for there, so routes are what it will touch.
    if (StrEqual(dir, FILEPATH_DIR_ROOT)) {
        return EndsWithIgnoreCase(leaf, ".gpx");
    }
    return true;
}

bool FilePath_Build(const char *dir, const char *leaf, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    if (!FilePath_LeafAllowedIn(dir, leaf)) {
        return false;
    }

    // The root already ends in its separator; the other two do not.
    const bool needs_slash = !StrEqual(dir, FILEPATH_DIR_ROOT);
    const size_t needed = strlen(dir) + (needs_slash ? 1u : 0u) + strlen(leaf) + 1u;
    if (needed > out_size || needed > FILEPATH_MAX) {
        return false;
    }

    strcpy(out, dir);
    if (needs_slash) {
        strcat(out, "/");
    }
    strcat(out, leaf);
    return true;
}

bool FilePath_Allowed(const char *path) {
    if (path == NULL || path[0] != '/') {
        return false;
    }
    if (strlen(path) + 1 > FILEPATH_MAX) {
        return false;
    }

    // Split at the last separator. Everything before it is the directory,
    // everything after is the leaf -- and because the leaf check refuses a
    // separator outright, a path with extra directories in it fails as a
    // directory that is not on the list rather than needing its own rule.
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return false;
    }

    char dir[FILEPATH_MAX];
    const size_t dir_len = (size_t)(slash - path);
    if (dir_len == 0) {
        // "/name" -- the directory is the root itself.
        strcpy(dir, FILEPATH_DIR_ROOT);
    } else {
        if (dir_len + 1 > sizeof(dir)) {
            return false;
        }
        memcpy(dir, path, dir_len);
        dir[dir_len] = '\0';
    }

    return FilePath_LeafAllowedIn(dir, slash + 1);
}
