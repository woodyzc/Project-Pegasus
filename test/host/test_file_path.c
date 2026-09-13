// Host tests for src/system/FilePath.c -- what the WiFi file server may touch.
//
// This suite carries more weight than its size suggests. Every other host test
// here covers something that goes wrong by itself; this one covers the only
// surface in the firmware that takes input from outside the device and hands
// it to the filesystem. The interesting cases are all adversarial.

#include <stdio.h>
#include <string.h>

#include "../../src/system/FilePath.h"

static int checks;
static int failures;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("\n    FAIL: %s", what);
    }
}

// Every one of these must be refused, whichever way it arrives.
static const char *const TRAVERSALS[] = {
    "/../etc/passwd",
    "/rides/../secret.txt",
    "/rides/../../secret.txt",
    "/MAP/../../rides/x.gpx",
    "/rides/sub/../x.gpx",
    "..",
    "../x",
    "/..",
    "/rides/..",
    "/rides/.",
};

int main(void) {
    char built[FILEPATH_MAX];

    printf("- exactly three directories are allowed: ");
    check(FilePath_DirAllowed("/"), "the card root");
    check(FilePath_DirAllowed("/rides"), "the ride log folder");
    check(FilePath_DirAllowed("/MAP"), "the map folder");
    check(!FilePath_DirAllowed("/map"), "not another spelling of one of them");
    check(!FilePath_DirAllowed("/rides/"), "not with a trailing separator");
    check(!FilePath_DirAllowed("/RIDES"), "not in another case");
    check(!FilePath_DirAllowed("/rides/sub"), "not a subdirectory of one");
    check(!FilePath_DirAllowed("/system"), "not a directory nobody listed");
    check(!FilePath_DirAllowed(""), "not the empty string");
    check(!FilePath_DirAllowed(NULL), "not a null pointer");
    printf("done\n");

    printf("- a traversal is refused however it is spelled: ");
    for (size_t i = 0; i < sizeof(TRAVERSALS) / sizeof(TRAVERSALS[0]); i++) {
        check(!FilePath_Allowed(TRAVERSALS[i]), TRAVERSALS[i]);
    }
    printf("done\n");

    printf("- a leaf may not contain a separator in either spelling: ");
    check(!FilePath_LeafSafe("sub/file.gpx"), "forward slash");
    check(!FilePath_LeafSafe("sub\\file.gpx"), "backslash, which FAT also honours");
    check(!FilePath_LeafSafe("/absolute.gpx"), "a leading separator");
    check(!FilePath_LeafSafe(".."), "the parent directory");
    check(!FilePath_LeafSafe("."), "the current directory");
    check(!FilePath_LeafSafe(""), "nothing at all");
    check(!FilePath_LeafSafe(NULL), "a null pointer");
    printf("done\n");

    printf("- a name cannot smuggle a control character: ");
    check(!FilePath_LeafSafe("ride\nname.gpx"), "a newline, which could split a header");
    check(!FilePath_LeafSafe("ride\rname.gpx"), "a carriage return");
    check(!FilePath_LeafSafe("ride\tname.gpx"), "a tab");
    check(!FilePath_LeafSafe("ride\x7f.gpx"), "DEL");
    check(!FilePath_LeafSafe("C:file.gpx"), "a colon");
    check(!FilePath_LeafSafe("ride*.gpx"), "a wildcard");
    printf("done\n");

    printf("- an ordinary name is accepted, accents and all: ");
    check(FilePath_LeafSafe("2026-09-13_143005.gpx"), "a ride log name");
    check(FilePath_LeafSafe("roads.prd"), "a road extract");
    check(FilePath_LeafSafe("Custis Trail Loop.gpx"), "spaces");
    // A byte above 0x7F is negative as a signed char. Comparing it without
    // casting would reject every non-ASCII name as a control character.
    check(FilePath_LeafSafe("Ru\xc3\xa9 de la Paix.gpx"), "UTF-8 outside ASCII");
    printf("done\n");

    printf("- the card root takes routes and nothing else: ");
    check(FilePath_Allowed("/trail.gpx"), "a route at the root");
    check(FilePath_Allowed("/TRAIL.GPX"), "whatever case the extension is in");
    check(!FilePath_Allowed("/notes.txt"), "not somebody's own file");
    check(!FilePath_Allowed("/passwords.csv"), "nor anything else kept there");
    check(!FilePath_Allowed("/gpx"), "not a name that merely looks like one");
    check(!FilePath_Allowed("/.gpx"), "not the bare extension, which is a dotfile");
    printf("done\n");

    printf("- the other two take any safe name: ");
    check(FilePath_Allowed("/rides/2026-09-13_143005.gpx"), "a recorded ride");
    check(FilePath_Allowed("/MAP/roads.prd"), "a road extract");
    check(FilePath_Allowed("/MAP/arlington.prd"), "another one");
    check(!FilePath_Allowed("/rides/sub/x.gpx"), "but not one nested deeper");
    printf("done\n");

    printf("- building a path refuses what checking a path would: ");
    check(FilePath_Build("/rides", "a.gpx", built, sizeof(built)), "an allowed pair builds");
    check(strcmp(built, "/rides/a.gpx") == 0, "with exactly one separator");
    check(FilePath_Build("/", "a.gpx", built, sizeof(built)), "the root builds too");
    check(strcmp(built, "/a.gpx") == 0, "without doubling the root's own separator");
    check(!FilePath_Build("/rides", "../a.gpx", built, sizeof(built)), "a traversal does not");
    check(built[0] == '\0', "and leaves nothing behind when it refuses");
    check(!FilePath_Build("/nope", "a.gpx", built, sizeof(built)), "nor an unlisted directory");
    check(!FilePath_Build("/", "a.txt", built, sizeof(built)), "nor a non-route at the root");
    printf("done\n");

    printf("- a name too long to join is refused rather than truncated: ");
    {
        // A truncated path is a path to a different file, and silently
        // writing to a different file than the one named is worse than
        // refusing the upload.
        char huge[FILEPATH_MAX + 64];
        memset(huge, 'a', sizeof(huge) - 5);
        strcpy(huge + sizeof(huge) - 5, ".gpx");
        check(!FilePath_LeafSafe(huge), "the leaf itself is over the limit");
        check(!FilePath_Build("/rides", huge, built, sizeof(built)), "so the join refuses");
        check(built[0] == '\0', "leaving nothing behind");

        char small[8];
        check(!FilePath_Build("/rides", "a-longer-name.gpx", small, sizeof(small)),
              "a buffer too small refuses as well");
        check(small[0] == '\0', "also leaving nothing behind");
    }
    printf("done\n");

    printf("- a path that cannot be parsed is refused, not guessed at: ");
    check(!FilePath_Allowed(NULL), "a null pointer");
    check(!FilePath_Allowed(""), "the empty string");
    check(!FilePath_Allowed("rides/a.gpx"), "a relative path");
    check(!FilePath_Allowed("/"), "the root on its own, which is a directory");
    check(!FilePath_Allowed("/rides/"), "a directory with a trailing separator");
    printf("done\n");

    printf("\nchecks: %d  failures: %d\n", checks, failures);
    printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
