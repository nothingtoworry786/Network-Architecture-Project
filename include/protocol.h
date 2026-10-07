/*
 * protocol.h - the BH/1 wire protocol ("HTTP, in binary").
 *
 * Pure byte-buffer encoding and decoding. No sockets here (see net.h) and no
 * struct is ever copied to or from the wire: every field is written and read
 * one byte at a time, big-endian. SPEC.md is the normative description; the
 * section numbers in comments refer to it.
 */
#ifndef BH_PROTOCOL_H
#define BH_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- Frame header (SPEC 5): magic(2) version(1) type(1) flags(1) length(3) ---- */
#define BH_MAGIC_0        0x42u /* 'B' */
#define BH_MAGIC_1        0x48u /* 'H' */
#define BH_VERSION        1u
#define BH_HEADER_SIZE    8u
#define BH_LENGTH_OFFSET  5u        /* offset of the 24-bit payload length */
#define BH_MAX_PAYLOAD    0xFFFFFFu /* largest value the length field can hold */

/* ---- Frame types (SPEC 6). Every other value is "unknown" and is skipped. ---- */
enum { BH_TYPE_REQUEST = 0x01, BH_TYPE_RESPONSE = 0x02 };

/* ---- Methods (SPEC 8). GET is the only method in version 1. ---- */
enum { BH_METHOD_GET = 0x01 };

/* ---- Status codes (SPEC 10) ---- */
enum {
    BH_STATUS_OK           = 200,
    BH_STATUS_BAD_REQUEST  = 400,
    BH_STATUS_NOT_FOUND    = 404,
    BH_STATUS_SERVER_ERROR = 500
};
#define BH_STATUS_MIN 100u
#define BH_STATUS_MAX 599u

/* ---- Header-name identifiers (SPEC 7): exactly ten names, plus the escape ---- */
enum {
    BH_HDR_CUSTOM           = 0, /* escape: a length-prefixed name follows */
    BH_HDR_CONTENT_TYPE     = 1,
    BH_HDR_CONTENT_LENGTH   = 2,
    BH_HDR_HOST             = 3,
    BH_HDR_ACCEPT           = 4,
    BH_HDR_USER_AGENT       = 5,
    BH_HDR_SERVER           = 6,
    BH_HDR_DATE             = 7,
    BH_HDR_CACHE_CONTROL    = 8,
    BH_HDR_CONTENT_ENCODING = 9,
    BH_HDR_LAST_MODIFIED    = 10
};
#define BH_HDR_LAST_KNOWN BH_HDR_LAST_MODIFIED

/* ---- Limits (SPEC 11). Everything is checked before memory is allocated. ---- */
#define BH_MAX_HEADERS          32u
#define BH_MAX_PATH_LEN         1024u
#define BH_MAX_NAME_LEN         64u
#define BH_MAX_VALUE_LEN        1024u
#define BH_MAX_REQUEST_PAYLOAD  65535u    /* above the largest legal request (35,972) */
#define BH_MAX_BODY_LEN         0xFF0000u /* leaves 65,535 payload bytes for status + headers */
#define BH_MAX_RESPONSE_HEAD    (BH_MAX_PAYLOAD - BH_MAX_BODY_LEN)

/* A byte string: never NUL-terminated, may contain any byte value. */
typedef struct {
    const uint8_t *data;
    size_t len;
} bh_bytes;

static inline bh_bytes bh_str(const char *s)
{
    bh_bytes b = { (const uint8_t *)s, strlen(s) };
    return b;
}

typedef struct {
    uint8_t id;     /* BH_HDR_*; ids above BH_HDR_LAST_KNOWN are decoded but meaningless */
    bh_bytes name;  /* only used when id == BH_HDR_CUSTOM */
    bh_bytes value;
} bh_header;

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint32_t length; /* payload bytes following the header */
} bh_frame_header;

/* Decoded messages are views: their bh_bytes point into the frame buffer. */
typedef struct {
    uint8_t method;
    bh_bytes path;
    size_t header_count;
    bh_header headers[BH_MAX_HEADERS];
} bh_request;

typedef struct {
    uint16_t status;
    size_t header_count;
    bh_header headers[BH_MAX_HEADERS];
    bh_bytes body;
} bh_response;

/* ---- Writer: bounded big-endian output. A failure is sticky, so callers
 *      check `failed` once at the end instead of after every field. ---- */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    int failed;
} bh_writer;

void bh_put_uint(bh_writer *w, uint32_t value, size_t width); /* width 1..4 bytes */
void bh_put_bytes(bh_writer *w, const void *data, size_t n);

static inline void bh_put_u8(bh_writer *w, uint8_t v)   { bh_put_uint(w, v, 1); }
static inline void bh_put_u16(bh_writer *w, uint16_t v) { bh_put_uint(w, v, 2); }
static inline void bh_put_u24(bh_writer *w, uint32_t v) { bh_put_uint(w, v, 3); }
static inline void bh_put_u32(bh_writer *w, uint32_t v) { bh_put_uint(w, v, 4); }

/* ---- Reader: bounded big-endian input over one whole frame. If `trace` is
 *      set it is called for every field the decoder consumes; that is how the
 *      annotated hexdump is produced by the real decoder, not a second parser. ---- */
typedef void bh_trace_fn(void *ctx, size_t offset, const uint8_t *bytes, size_t n,
                         const char *label);

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
    int failed;
    bh_trace_fn *trace;
    void *trace_ctx;
} bh_reader;

/*
 * Encoders write one complete frame (header + payload) into buf and return
 * its size in bytes, or 0 if the message breaks a protocol limit or does not
 * fit in cap.
 */
size_t bh_encode_request(uint8_t *buf, size_t cap, const bh_request *req);
size_t bh_encode_response(uint8_t *buf, size_t cap, const bh_response *res);

/*
 * Decoders return 0 on success, -1 if the bytes are malformed.
 * bh_decode_frame_header reads the 8 header bytes at r->pos; it fails on a
 * wrong magic or version. The payload decoders then consume the rest of the
 * reader and fail unless every byte is accounted for.
 */
int bh_decode_frame_header(bh_reader *r, bh_frame_header *out);
int bh_decode_request(bh_reader *r, bh_request *out);
int bh_decode_response(bh_reader *r, bh_response *out);

#endif
