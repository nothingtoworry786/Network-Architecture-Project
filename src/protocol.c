/*
 * protocol.c - BH/1 encoding and decoding on plain byte buffers.
 */
#include "protocol.h"

#include <stdio.h>

static const uint8_t MAGIC[] = { BH_MAGIC_0, BH_MAGIC_1 };

/* ------------------------------------------------------------------ writer */

void bh_put_uint(bh_writer *w, uint32_t value, size_t width)
{
    if (w->failed || w->cap - w->len < width) {
        w->failed = 1;
        return;
    }
    /* Big-endian: most significant byte first, whatever the host order is. */
    for (size_t i = 0; i < width; i++)
        w->buf[w->len++] = (uint8_t)(value >> (8 * (width - 1 - i)));
}

void bh_put_bytes(bh_writer *w, const void *data, size_t n)
{
    if (w->failed || w->cap - w->len < n) {
        w->failed = 1;
        return;
    }
    if (n > 0)
        memcpy(w->buf + w->len, data, n);
    w->len += n;
}

/* ------------------------------------------------------------------ reader */

static bh_bytes get_bytes(bh_reader *r, size_t n, const char *label)
{
    bh_bytes out = { NULL, 0 };

    if (r->failed || r->len - r->pos < n) {
        r->failed = 1;
        return out;
    }
    out.data = r->buf + r->pos;
    out.len = n;
    if (r->trace)
        r->trace(r->trace_ctx, r->pos, out.data, n, label);
    r->pos += n;
    return out;
}

static uint32_t get_uint(bh_reader *r, size_t width, const char *label)
{
    uint32_t value = 0;

    if (r->failed || r->len - r->pos < width) {
        r->failed = 1;
        return 0;
    }
    for (size_t i = 0; i < width; i++)
        value = (value << 8) | r->buf[r->pos + i];
    if (r->trace) {
        char text[64];
        snprintf(text, sizeof text, "%s = %lu", label, (unsigned long)value);
        r->trace(r->trace_ctx, r->pos, r->buf + r->pos, width, text);
    }
    r->pos += width;
    return value;
}

/* ------------------------------------------------------------ frame header */

static void begin_frame(bh_writer *w, uint8_t type)
{
    bh_put_bytes(w, MAGIC, sizeof MAGIC);
    bh_put_u8(w, BH_VERSION);
    bh_put_u8(w, type);
    bh_put_u8(w, 0);  /* flags: none are defined in version 1 */
    bh_put_u24(w, 0); /* payload length, patched in by finish_frame */
}

static size_t finish_frame(bh_writer *w)
{
    if (w->failed || w->len - BH_HEADER_SIZE > BH_MAX_PAYLOAD)
        return 0;
    bh_writer length = { w->buf + BH_LENGTH_OFFSET, BH_HEADER_SIZE - BH_LENGTH_OFFSET, 0, 0 };
    bh_put_u24(&length, (uint32_t)(w->len - BH_HEADER_SIZE));
    return w->len;
}

int bh_decode_frame_header(bh_reader *r, bh_frame_header *out)
{
    bh_bytes magic = get_bytes(r, sizeof MAGIC, "magic \"BH\"");
    uint32_t version = get_uint(r, 1, "version");

    out->type = (uint8_t)get_uint(r, 1, "type");
    out->flags = (uint8_t)get_uint(r, 1, "flags");
    out->length = get_uint(r, 3, "payload length");
    if (r->failed || memcmp(magic.data, MAGIC, sizeof MAGIC) != 0 || version != BH_VERSION)
        return -1;
    return 0;
}

/* ----------------------------------------------------------------- headers */

static const char *value_label(uint8_t id)
{
    static const char *const labels[] = {
        "value (custom header)",    "value (Content-Type)",     "value (Content-Length)",
        "value (Host)",             "value (Accept)",           "value (User-Agent)",
        "value (Server)",           "value (Date)",             "value (Cache-Control)",
        "value (Content-Encoding)", "value (Last-Modified)",
    };
    return id <= BH_HDR_LAST_KNOWN ? labels[id] : "value (unassigned id, ignored)";
}

static void put_headers(bh_writer *w, const bh_header *h, size_t count)
{
    if (count > BH_MAX_HEADERS) {
        w->failed = 1;
        return;
    }
    bh_put_u8(w, (uint8_t)count);
    for (size_t i = 0; i < count; i++) {
        int custom = h[i].id == BH_HDR_CUSTOM;

        if (h[i].id > BH_HDR_LAST_KNOWN || h[i].value.len > BH_MAX_VALUE_LEN ||
            (custom && (h[i].name.len == 0 || h[i].name.len > BH_MAX_NAME_LEN))) {
            w->failed = 1;
            return;
        }
        bh_put_u8(w, h[i].id);
        if (custom) {
            bh_put_u8(w, (uint8_t)h[i].name.len);
            bh_put_bytes(w, h[i].name.data, h[i].name.len);
        }
        bh_put_u16(w, (uint16_t)h[i].value.len);
        bh_put_bytes(w, h[i].value.data, h[i].value.len);
    }
}

static size_t get_headers(bh_reader *r, bh_header *h)
{
    size_t count = get_uint(r, 1, "header count");

    if (count > BH_MAX_HEADERS) {
        r->failed = 1;
        return 0;
    }
    for (size_t i = 0; i < count && !r->failed; i++) {
        size_t n;

        h[i].id = (uint8_t)get_uint(r, 1, "header id");
        h[i].name = (bh_bytes){ NULL, 0 };
        if (h[i].id == BH_HDR_CUSTOM) {
            n = get_uint(r, 1, "name length");
            if (n == 0 || n > BH_MAX_NAME_LEN)
                r->failed = 1;
            h[i].name = get_bytes(r, n, "custom header name");
        }
        /*
         * Ids above BH_HDR_LAST_KNOWN are not an error: the value is still
         * length-prefixed, so a header added by a later version can be
         * stepped over exactly like an unknown frame type.
         */
        n = get_uint(r, 2, "value length");
        if (n > BH_MAX_VALUE_LEN)
            r->failed = 1;
        h[i].value = get_bytes(r, n, value_label(h[i].id));
    }
    return count;
}

/* ---------------------------------------------------------------- requests */

static int path_ok(bh_bytes path)
{
    return path.len >= 1 && path.len <= BH_MAX_PATH_LEN && path.data[0] == '/';
}

size_t bh_encode_request(uint8_t *buf, size_t cap, const bh_request *req)
{
    bh_writer w = { buf, cap, 0, 0 };

    if (req->method != BH_METHOD_GET || !path_ok(req->path))
        return 0;
    begin_frame(&w, BH_TYPE_REQUEST);
    bh_put_u8(&w, req->method);
    bh_put_u16(&w, (uint16_t)req->path.len);
    bh_put_bytes(&w, req->path.data, req->path.len);
    put_headers(&w, req->headers, req->header_count);
    return finish_frame(&w);
}

int bh_decode_request(bh_reader *r, bh_request *out)
{
    out->method = (uint8_t)get_uint(r, 1, "method");
    out->path = get_bytes(r, get_uint(r, 2, "path length"), "path");
    out->header_count = get_headers(r, out->headers);

    /* r->pos != r->len: bytes left over after the last field are an error too. */
    if (r->failed || r->pos != r->len)
        return -1;
    if (out->method != BH_METHOD_GET || !path_ok(out->path))
        return -1;
    return 0;
}

/* --------------------------------------------------------------- responses */

static int status_ok(uint32_t status)
{
    return status >= BH_STATUS_MIN && status <= BH_STATUS_MAX;
}

size_t bh_encode_response(uint8_t *buf, size_t cap, const bh_response *res)
{
    bh_writer w = { buf, cap, 0, 0 };

    if (!status_ok(res->status) || res->body.len > BH_MAX_BODY_LEN)
        return 0;
    begin_frame(&w, BH_TYPE_RESPONSE);
    bh_put_u16(&w, res->status);
    put_headers(&w, res->headers, res->header_count);
    bh_put_u32(&w, (uint32_t)res->body.len);
    bh_put_bytes(&w, res->body.data, res->body.len);
    return finish_frame(&w);
}

int bh_decode_response(bh_reader *r, bh_response *out)
{
    out->status = (uint16_t)get_uint(r, 2, "status");
    out->header_count = get_headers(r, out->headers);
    out->body = get_bytes(r, get_uint(r, 4, "body length"), "body");

    /* body length must account for exactly the rest of the payload. */
    if (r->failed || r->pos != r->len)
        return -1;
    if (!status_ok(out->status) || out->body.len > BH_MAX_BODY_LEN)
        return -1;
    return 0;
}
