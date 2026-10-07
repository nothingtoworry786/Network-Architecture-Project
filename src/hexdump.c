/*
 * hexdump.c - annotated hexdump of one BH/1 frame.
 *
 * There is no second parser here. The dump is produced by running the real
 * decoders from protocol.c with a trace callback, so the annotation cannot
 * disagree with what the implementation actually does with the bytes.
 */
#include "hexdump.h"

#include <ctype.h>

#include "protocol.h"

#define BYTES_PER_ROW 16

/* bh_trace_fn: prints one field as rows of "offset  hex  |ascii|  label". */
static void dump_field(void *ctx, size_t offset, const uint8_t *bytes, size_t n,
                       const char *label)
{
    FILE *out = ctx;
    size_t done = 0;

    do { /* do-while: a zero-length field still gets its labelled row */
        size_t row = n - done < BYTES_PER_ROW ? n - done : BYTES_PER_ROW;
        char hex[BYTES_PER_ROW * 3 + 1] = "";
        char ascii[BYTES_PER_ROW + 1] = "";

        for (size_t i = 0; i < row; i++) {
            uint8_t b = bytes[done + i];

            snprintf(hex + 3 * i, 4, "%02x ", b);
            ascii[i] = isprint(b) ? (char)b : '.';
        }
        fprintf(out, "%04zx  %-48s |%-16s|%s%s\n", offset + done, hex, ascii,
                done == 0 ? "  " : "", done == 0 ? label : "");
        done += row;
    } while (done < n);
}

static const char *type_name(uint8_t type)
{
    switch (type) {
    case BH_TYPE_REQUEST:  return "REQUEST";
    case BH_TYPE_RESPONSE: return "RESPONSE";
    default:               return "UNKNOWN";
    }
}

void hexdump_frame(FILE *out, const char *direction, const uint8_t *frame, size_t len)
{
    bh_reader r = { .buf = frame, .len = len };
    bh_frame_header h;
    bh_request req;
    bh_response res;
    int ok = bh_decode_frame_header(&r, &h) == 0; /* untraced pass, for the banner */

    if (ok)
        fprintf(out, "== %s: %s frame, type 0x%02x, flags 0x%02x, payload %lu bytes, "
                "%zu bytes on the wire ==\n", direction, type_name(h.type),
                (unsigned)h.type, (unsigned)h.flags, (unsigned long)h.length, len);
    else
        fprintf(out, "== %s: not a BH/1 frame, %zu bytes ==\n", direction, len);

    r = (bh_reader){ .buf = frame, .len = len, .trace = dump_field, .trace_ctx = out };
    ok = bh_decode_frame_header(&r, &h) == 0;
    if (ok && h.type == BH_TYPE_REQUEST)
        ok = bh_decode_request(&r, &req) == 0;
    else if (ok && h.type == BH_TYPE_RESPONSE)
        ok = bh_decode_response(&r, &res) == 0;

    /* Whatever the decoder did not consume is still shown, never hidden. */
    if (r.pos < len)
        dump_field(out, r.pos, frame + r.pos, len - r.pos,
                   ok ? "payload (frame type not understood)" : "not decoded (malformed)");
    fputc('\n', out);
}
