/*
 * net.c - robust stream I/O for BH/1 frames.
 */
#include "net.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "protocol.h"

int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t got = 0;

    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);

        if (r > 0)
            got += (size_t)r; /* short read: loop for the rest */
        else if (r == 0)
            return got == 0 ? 0 : -1; /* EOF: clean only on a frame boundary */
        else if (errno != EINTR)
            return -1;
    }
    return 1;
}

int write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    size_t sent = 0;

    while (sent < n) {
        /* MSG_NOSIGNAL: a vanished peer gives EPIPE instead of killing us. */
        ssize_t r = send(fd, p + sent, n - sent, MSG_NOSIGNAL);

        if (r >= 0)
            sent += (size_t)r; /* short write: loop for the rest */
        else if (errno != EINTR)
            return -1;
    }
    return 0;
}

/* Reads and throws away n bytes without ever holding more than one chunk. */
static int discard_full(int fd, size_t n)
{
    uint8_t scratch[4096];

    while (n > 0) {
        size_t chunk = n < sizeof scratch ? n : sizeof scratch;

        if (read_full(fd, scratch, chunk) != 1)
            return -1;
        n -= chunk;
    }
    return 0;
}

int recv_frame(int fd, uint8_t want_type, size_t max_payload, uint8_t **frame,
               size_t *frame_len, unsigned *skipped)
{
    uint8_t raw[BH_HEADER_SIZE];
    bh_frame_header h;
    uint8_t *buf;

    for (;;) {
        int rc = read_full(fd, raw, sizeof raw);

        if (rc != 1)
            return rc == 0 ? RECV_EOF : RECV_IO;
        bh_reader r = { .buf = raw, .len = sizeof raw };
        if (bh_decode_frame_header(&r, &h) != 0)
            return RECV_DESYNC;
        if (h.type == want_type)
            break;
        /*
         * UNKNOWN FRAME TYPE (SPEC 6.3). The header is valid, so the length
         * field tells us exactly where the next frame starts: consume the
         * payload, ignore it (flags included), and keep going. Nothing is
         * allocated, so a huge unknown frame costs no memory.
         */
        if (discard_full(fd, h.length) != 0)
            return RECV_IO;
        if (skipped)
            (*skipped)++;
    }

    /* Validate before allocating: the length came from the network. */
    if (h.flags != 0 || h.length > max_payload)
        return discard_full(fd, h.length) == 0 ? RECV_REJECTED : RECV_IO;

    buf = malloc(BH_HEADER_SIZE + h.length);
    if (buf == NULL)
        return RECV_IO;
    memcpy(buf, raw, BH_HEADER_SIZE);
    if (read_full(fd, buf + BH_HEADER_SIZE, h.length) != 1) {
        free(buf);
        return RECV_IO;
    }
    *frame = buf;
    *frame_len = BH_HEADER_SIZE + h.length;
    return RECV_OK;
}
