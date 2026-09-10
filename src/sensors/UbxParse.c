#include "UbxParse.h"

/* NAV-PVT payload offsets, from the u-blox M10 interface description.
   Note lon comes BEFORE lat -- the opposite of how they are usually written,
   and an easy way to ship a parser that looks right and reports the wrong
   hemisphere. */
#define OFF_YEAR 4
#define OFF_MONTH 6
#define OFF_DAY 7
#define OFF_HOUR 8
#define OFF_MIN 9
#define OFF_SEC 10
#define OFF_VALID 11
#define OFF_FIXTYPE 20
#define OFF_FLAGS 21
#define OFF_NUMSV 23
#define OFF_LON 24
#define OFF_LAT 28
#define OFF_HMSL 36
#define OFF_GSPEED 60
#define OFF_HEADMOT 64

/* valid bitfield (offset 11) */
#define VALID_DATE 0x01
#define VALID_TIME 0x02
#define VALID_FULLY_RESOLVED 0x04

/* flags bitfield (offset 21) */
#define FLAGS_GNSS_FIX_OK 0x01

enum {
    ST_SYNC1 = 0,
    ST_SYNC2,
    ST_CLASS,
    ST_ID,
    ST_LEN_LO,
    ST_LEN_HI,
    ST_PAYLOAD,
    ST_CK_A,
    ST_CK_B
};

static uint32_t ReadU32(const uint8_t *p) {
    /* Little-endian, assembled byte by byte: the payload buffer has no
       alignment guarantee and an unaligned 32-bit load is not free on Xtensa. */
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t ReadI32(const uint8_t *p) {
    return (int32_t)ReadU32(p);
}

static uint16_t ReadU16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void ChecksumByte(UbxParser_t *parser, uint8_t byte) {
    parser->ck_a = (uint8_t)(parser->ck_a + byte);
    parser->ck_b = (uint8_t)(parser->ck_b + parser->ck_a);
}

void Ubx_Init(UbxParser_t *parser) {
    if (parser == NULL) {
        return;
    }
    parser->state = ST_SYNC1;
    parser->msg_class = 0;
    parser->msg_id = 0;
    parser->length = 0;
    parser->index = 0;
    parser->ck_a = 0;
    parser->ck_b = 0;
    parser->oversized = false;
}

bool Ubx_DecodeNavPvt(const uint8_t *payload, size_t length, UbxNavPvt_t *out) {
    if (payload == NULL || out == NULL || length < (size_t)UBX_NAV_PVT_LEN) {
        return false;
    }

    {
        const uint8_t valid = payload[OFF_VALID];
        const uint8_t flags = payload[OFF_FLAGS];
        const uint8_t fix_type = payload[OFF_FIXTYPE];

        out->fix_type = fix_type;
        out->num_sv = payload[OFF_NUMSV];

        /* Both conditions matter: fixType says what kind of solution exists,
           gnssFixOK says whether it passed the receiver's own validity checks.
           A 3D fix with gnssFixOK clear is still not trustworthy. */
        out->fix_valid = ((flags & FLAGS_GNSS_FIX_OK) != 0) && (fix_type >= 2);

        /* 1e-7 degrees on the wire. */
        out->lon_deg = (double)ReadI32(payload + OFF_LON) * 1e-7;
        out->lat_deg = (double)ReadI32(payload + OFF_LAT) * 1e-7;

        /* hMSL is millimetres; gSpeed is mm/s; headMot is 1e-5 degrees. */
        out->alt_m = (float)ReadI32(payload + OFF_HMSL) / 1000.0f;
        out->speed_mps = (float)ReadI32(payload + OFF_GSPEED) / 1000.0f;
        out->heading_deg = (float)ReadI32(payload + OFF_HEADMOT) * 1e-5f;

        /* Normalise heading: headMot can be reported slightly outside 0-360,
           and the UI treats it as a compass bearing. */
        while (out->heading_deg < 0.0f) {
            out->heading_deg += 360.0f;
        }
        while (out->heading_deg >= 360.0f) {
            out->heading_deg -= 360.0f;
        }

        /* fullyResolved as well as validDate/validTime: without it the clock
           can be a second or two out while the receiver is still settling,
           which is worse than showing nothing on a bike computer. */
        out->time_valid = (valid & VALID_DATE) && (valid & VALID_TIME) &&
                          (valid & VALID_FULLY_RESOLVED);

        out->year = ReadU16(payload + OFF_YEAR);
        out->month = payload[OFF_MONTH];
        out->day = payload[OFF_DAY];
        out->hour = payload[OFF_HOUR];
        out->minute = payload[OFF_MIN];
        out->second = payload[OFF_SEC];
    }
    return true;
}

bool Ubx_Feed(UbxParser_t *parser, uint8_t byte, UbxNavPvt_t *out) {
    if (parser == NULL || out == NULL) {
        return false;
    }

    switch (parser->state) {
        case ST_SYNC1:
            if (byte == 0xB5) {
                parser->state = ST_SYNC2;
            }
            /* Anything else is noise between frames -- or NMEA, if the module
               has not been reconfigured yet -- and is simply skipped. */
            break;

        case ST_SYNC2:
            if (byte == 0x62) {
                parser->state = ST_CLASS;
                parser->ck_a = 0;
                parser->ck_b = 0;
            } else if (byte == 0xB5) {
                /* Stay armed: 0xB5 0xB5 0x62 is a valid start. */
                parser->state = ST_SYNC2;
            } else {
                parser->state = ST_SYNC1;
            }
            break;

        case ST_CLASS:
            parser->msg_class = byte;
            ChecksumByte(parser, byte);
            parser->state = ST_ID;
            break;

        case ST_ID:
            parser->msg_id = byte;
            ChecksumByte(parser, byte);
            parser->state = ST_LEN_LO;
            break;

        case ST_LEN_LO:
            parser->length = byte;
            ChecksumByte(parser, byte);
            parser->state = ST_LEN_HI;
            break;

        case ST_LEN_HI:
            parser->length |= (uint16_t)((uint16_t)byte << 8);
            ChecksumByte(parser, byte);
            parser->index = 0;
            /* Messages we do not handle still have to be walked to their end
               so the stream stays in sync; only their payload is discarded. */
            parser->oversized = (parser->length > UBX_MAX_PAYLOAD);
            parser->state = (parser->length == 0) ? ST_CK_A : ST_PAYLOAD;
            break;

        case ST_PAYLOAD:
            if (!parser->oversized) {
                parser->payload[parser->index] = byte;
            }
            ChecksumByte(parser, byte);
            parser->index++;
            if (parser->index >= parser->length) {
                parser->state = ST_CK_A;
            }
            break;

        case ST_CK_A:
            if (byte != parser->ck_a) {
                Ubx_Init(parser); /* corrupt frame: drop it whole */
                return false;
            }
            parser->state = ST_CK_B;
            break;

        case ST_CK_B: {
            const bool ok = (byte == parser->ck_b);
            const bool is_pvt = (parser->msg_class == UBX_NAV_PVT_CLASS) &&
                                (parser->msg_id == UBX_NAV_PVT_ID) &&
                                (parser->length == UBX_NAV_PVT_LEN) &&
                                !parser->oversized;
            uint8_t payload_copy_ok = 0;

            if (ok && is_pvt) {
                payload_copy_ok = Ubx_DecodeNavPvt(parser->payload, parser->length, out) ? 1 : 0;
            }

            Ubx_Init(parser);
            return payload_copy_ok != 0;
        }

        default:
            Ubx_Init(parser);
            break;
    }
    return false;
}
