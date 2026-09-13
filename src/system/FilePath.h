#pragma once

#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Decides which paths on the SD card the file server may touch.
//
// Split out from FileServer.cpp and kept free of Arduino, SD_MMC and the HTTP
// server so it can be host-tested, because this is the part where a mistake
// matters: the server hands whatever path it is given straight to the
// filesystem, and the request comes from outside the device. Everything else
// in that module is plumbing, this is the boundary.
//
// The rule is deliberately narrow. Three directories, no nesting, no
// traversal, and at the card root only .gpx files -- the root holds whatever
// the owner keeps on the card, so it is listed by extension rather than
// opened up. Anything not explicitly permitted is refused.

#ifdef __cplusplus
extern "C" {
#endif

// Longest path this will build, including the NUL. Generous against SD_MMC's
// own limits rather than tight against them.
#define FILEPATH_MAX 160

// The three directories, exactly. "/" is the card root, where GpxTrack looks
// for routes; "/rides" is where RideLog writes; "/MAP" holds the .prd road
// extracts.
#define FILEPATH_DIR_ROOT "/"
#define FILEPATH_DIR_RIDES "/rides"
#define FILEPATH_DIR_MAP "/MAP"

// True for exactly those three spellings. Case-sensitive, because SD_MMC's
// FAT driver is not, and accepting "/map" here while the card holds "/MAP"
// would let two spellings of one directory disagree about what is allowed.
bool FilePath_DirAllowed(const char *dir);

// True if `leaf` is a plain file name: non-empty, no leading dot, no
// separator in either spelling, no control or FAT-reserved characters, and
// short enough to join.
//
// The leading-dot rule covers "." and ".." without naming them, and it is not
// what stops a traversal: no separator survives this check, so one cannot be
// assembled from a name it accepted.
bool FilePath_LeafSafe(const char *leaf);

// True if this directory accepts this file. Every allowed directory takes any
// safe leaf except the card root, which takes only *.gpx (case-insensitive).
bool FilePath_LeafAllowedIn(const char *dir, const char *leaf);

// Joins an allowed directory and a safe leaf into `out`. Returns false, and
// writes nothing, if either part is refused or the result would not fit.
bool FilePath_Build(const char *dir, const char *leaf, char *out, size_t out_size);

// The whole check in one call, for a path that arrived from a request:
// splits it, then applies every rule above. Returns false for anything it
// cannot prove safe, including a path it cannot parse.
bool FilePath_Allowed(const char *path);

#ifdef __cplusplus
}
#endif
