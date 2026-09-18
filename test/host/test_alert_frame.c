/* Host-side tests for the phone alert frame (src/system/AlertFrame.c).
 *
 * The name in this frame is the only field in the firmware that arrives from
 * off-device and is then printed verbatim on the panel, so the length handling
 * is the part under test rather than an afterthought: the declared length and
 * the delivered length are two different numbers under an attacker's or a
 * buggy encoder's control, and trusting the first one reads past the buffer.
 *
 * Everything here runs under ASan in `make asan`, which is where an
 * over-read would actually be caught -- the parser would otherwise return a
 * plausible-looking name and pass. */
#include <stdio.h>
#include <string.h>

#include "AlertFrame.h"

static int checks = 0;
static int failures = 0;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* Builds a frame into `b` and returns its total length. */
static size_t build(uint8_t *b, uint8_t kind, uint8_t count, const char *name) {
    const size_t n = strlen(name);
    b[0] = ALERT_FRAME_MAGIC;
    b[1] = ALERT_FRAME_VERSION;
    b[2] = kind;
    b[3] = (uint8_t)n;
    b[4] = count;
    b[5] = 0;
    memcpy(b + ALERT_HEADER_LEN, name, n);
    return ALERT_HEADER_LEN + n;
}

static void test_well_formed(void) {
    printf("-- a call from someone in the address book --\n");
    uint8_t b[64];
    const size_t len = build(b, ALERT_KIND_CALL, 1, "Mum");
    check(len == 9, "three-byte name makes a nine-byte frame");

    AlertFrame_t a;
    memset(&a, 0xAA, sizeof(a));
    check(Alert_ParseFrame(b, len, &a), "accepted");
    check(a.kind == ALERT_KIND_CALL, "kind is call");
    check(a.count == 1, "stands for one event");
    check(strcmp(a.name, "Mum") == 0, "name came through");
    check(strlen(a.name) == 3, "and is NUL-terminated at the right place");
}

static void test_kinds(void) {
    printf("-- the three things a phone can interrupt a ride for --\n");
    uint8_t b[64];
    AlertFrame_t a;

    check(Alert_ParseFrame(b, build(b, ALERT_KIND_SMS, 1, "Alex"), &a), "sms accepted");
    check(a.kind == ALERT_KIND_SMS, "kind is sms");

    check(Alert_ParseFrame(b, build(b, ALERT_KIND_CHAT, 1, "Group"), &a), "chat accepted");
    check(a.kind == ALERT_KIND_CHAT, "kind is chat");

    /* An unknown kind is refused whole rather than shown under a default
     * label, because the label is what tells the rider whether to stop. */
    check(!Alert_ParseFrame(b, build(b, ALERT_KIND_COUNT, 1, "X"), &a),
          "first undefined kind refused");
    check(!Alert_ParseFrame(b, build(b, 200, 1, "X"), &a), "far-out kind refused");
}

static void test_kind_text(void) {
    printf("-- the label the panel puts in front of the name --\n");
    check(strcmp(Alert_KindText(ALERT_KIND_CALL), "Call") == 0, "call label");
    check(strcmp(Alert_KindText(ALERT_KIND_SMS), "SMS") == 0, "sms label");
    check(strcmp(Alert_KindText(ALERT_KIND_CHAT), "WeChat") == 0, "chat label");
    /* Empty rather than "?" so a future kind that slips through prints the
     * name alone instead of a label that means nothing. */
    check(Alert_KindText((AlertKind_t)99)[0] == '\0', "unknown label is empty");
}

static void test_header_rejections(void) {
    printf("-- frames that are not ours --\n");
    uint8_t b[64];
    AlertFrame_t a;
    const size_t len = build(b, ALERT_KIND_SMS, 1, "Alex");

    check(!Alert_ParseFrame(NULL, len, &a), "null data refused");
    check(!Alert_ParseFrame(b, len, NULL), "null out refused");

    for (size_t short_len = 0; short_len < ALERT_HEADER_LEN; short_len++) {
        check(!Alert_ParseFrame(b, short_len, &a), "shorter than the header refused");
    }

    build(b, ALERT_KIND_SMS, 1, "Alex");
    b[0] = 'B';
    check(!Alert_ParseFrame(b, len, &a), "wrong magic refused");

    build(b, ALERT_KIND_SMS, 1, "Alex");
    b[1] = ALERT_FRAME_VERSION + 1;
    check(!Alert_ParseFrame(b, len, &a), "later version refused");
    b[1] = 0;
    check(!Alert_ParseFrame(b, len, &a), "version zero refused");
}

static void test_count(void) {
    printf("-- how many messages one banner stands for --\n");
    uint8_t b[64];
    AlertFrame_t a;

    check(Alert_ParseFrame(b, build(b, ALERT_KIND_CHAT, 255, "Family"), &a),
          "255 coalesced messages accepted");
    check(a.count == 255, "count came through");

    /* Zero would be a banner standing for nothing, which the phone has no
     * honest way to produce -- and which would print "0 messages". */
    check(!Alert_ParseFrame(b, build(b, ALERT_KIND_CHAT, 0, "Family"), &a),
          "zero count refused");
}

static void test_name_length(void) {
    printf("-- the name, which is the whole payload --\n");
    uint8_t b[128];
    AlertFrame_t a;

    /* An alert with no name at all is legitimate: a call from a number that
     * is not in the address book has nobody to name. The panel still needs to
     * ring. */
    memset(&a, 0xAA, sizeof(a));
    check(Alert_ParseFrame(b, build(b, ALERT_KIND_CALL, 1, ""), &a), "empty name accepted");
    check(a.name[0] == '\0', "and reads as an empty string");

    char longest[ALERT_NAME_MAX + 1];
    memset(longest, 'A', ALERT_NAME_MAX);
    longest[ALERT_NAME_MAX] = '\0';
    size_t len = build(b, ALERT_KIND_CHAT, 1, longest);
    check(Alert_ParseFrame(b, len, &a), "the longest legal name accepted");
    check(strlen(a.name) == ALERT_NAME_MAX, "all of it kept");
    check(strcmp(a.name, longest) == 0, "unchanged");

    /* One over. Both numbers have to move together for this to be the length
     * test and not the mismatch test below. */
    b[3] = ALERT_NAME_MAX + 1;
    b[ALERT_HEADER_LEN + ALERT_NAME_MAX] = 'A';
    check(!Alert_ParseFrame(b, len + 1, &a), "one byte over the maximum refused");

    b[3] = 255;
    check(!Alert_ParseFrame(b, ALERT_HEADER_LEN + 255, &a), "a 255-byte name refused");
}

static void test_length_mismatch(void) {
    printf("-- when the declared length and the delivered length disagree --\n");
    uint8_t b[64];
    AlertFrame_t a;
    const size_t len = build(b, ALERT_KIND_SMS, 1, "Alex");

    /* This is the case that reads off the end of the BLE buffer if the
     * declared length is taken on trust. A truncated write is otherwise
     * indistinguishable from a complete one. */
    check(!Alert_ParseFrame(b, len - 1, &a), "one byte short refused");
    check(!Alert_ParseFrame(b, ALERT_HEADER_LEN, &a), "name missing entirely refused");

    /* And the other direction: trailing bytes mean this is not the frame it
     * says it is, so it is not decoded as one. */
    b[ALERT_HEADER_LEN + 4] = 'X';
    check(!Alert_ParseFrame(b, len + 1, &a), "one byte long refused");
}

static void test_non_ascii_passes_through(void) {
    printf("-- a Chinese name is bytes to this layer --\n");
    uint8_t b[64];
    AlertFrame_t a;

    /* The parser must not care. It counts bytes; the font is what decides
     * whether this draws, and that is a separate problem from this one. */
    const char *name = "\xE5\xBC\xA0\xE4\xB8\x89"; /* 张三 */
    const size_t len = build(b, ALERT_KIND_CHAT, 3, name);
    check(len == ALERT_HEADER_LEN + 6, "two characters are six UTF-8 bytes");
    check(Alert_ParseFrame(b, len, &a), "accepted");
    check(strcmp(a.name, name) == 0, "bytes came through untouched");

    /* No validation of UTF-8 sequences, deliberately: a truncated multi-byte
     * character costs a wrong glyph, while rejecting the frame costs the
     * rider the alert. Nothing downstream indexes into this string. */
    char truncated[4] = {'\xE5', '\xBC', '\0', '\0'};
    const size_t tlen = build(b, ALERT_KIND_CHAT, 1, truncated);
    check(Alert_ParseFrame(b, tlen, &a), "a half character is still an alert");
}

static void test_reserved_byte(void) {
    printf("-- the reserved byte is ignored, not policed --\n");
    uint8_t b[64];
    AlertFrame_t a;
    const size_t len = build(b, ALERT_KIND_SMS, 1, "Alex");

    /* Opposite of GpsFrame's reserved flag bits, which are rejected. There a
     * wrong reading is a wrong position on the map; here it is at worst a
     * banner with no extra decoration, and a build that refuses the frame
     * outright would go silent the moment the app learns a new trick. */
    b[5] = 0xFF;
    check(Alert_ParseFrame(b, len, &a), "an unknown reserved value still parses");
    check(strcmp(a.name, "Alex") == 0, "and the rest is unaffected");
}

/* The exact bytes the phone's encoder produces.
 *
 * This array is duplicated, character for character, in the companion app's
 * AlertFrameTest.kt. That duplication is the test: the two sides are separate
 * codebases in separate languages built by separate toolchains, and the only
 * thing keeping them agreeing about this frame is that both are pinned to the
 * same twelve bytes. Move a field on either side and one of the two suites
 * goes red.
 *
 * A WeChat group that said six things, named in Chinese. Chinese on purpose:
 * a multi-byte name is where a length in characters rather than bytes would
 * pass every ASCII test and fail here. */
static void test_reference_frame_from_the_phone(void) {
    printf("-- the frame the companion app actually sends --\n");
    const uint8_t wire[] = {
        0x41,                   /* magic 'A' */
        0x01,                   /* version */
        0x02,                   /* ALERT_KIND_CHAT */
        0x06,                   /* name length, in BYTES */
        0x06,                   /* count */
        0x00,                   /* reserved */
        0xE5, 0xBC, 0xA0,       /* U+5F20 */
        0xE4, 0xB8, 0x89,       /* U+4E09 */
    };

    AlertFrame_t a;
    check(sizeof(wire) == 12, "twelve bytes on the wire");
    check(Alert_ParseFrame(wire, sizeof(wire), &a), "the firmware accepts it");
    check(a.kind == ALERT_KIND_CHAT, "decoded as a chat message");
    check(a.count == 6, "standing for six messages");
    check(strlen(a.name) == 6, "six bytes of name");
    check(memcmp(a.name, wire + ALERT_HEADER_LEN, 6) == 0, "name byte-for-byte");
}

int main(void) {
    test_well_formed();
    test_kinds();
    test_kind_text();
    test_header_rejections();
    test_count();
    test_name_length();
    test_length_mismatch();
    test_non_ascii_passes_through();
    test_reserved_byte();
    test_reference_frame_from_the_phone();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
