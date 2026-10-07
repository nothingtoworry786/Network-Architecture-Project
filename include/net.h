/*
 * net.h - robust stream I/O for BH/1 frames.
 *
 * TCP is a byte stream: one recv() may return half a frame or two frames, and
 * one send() may accept only part of a buffer. Nothing outside net.c calls
 * recv() or send() directly.
 */
#ifndef BH_NET_H
#define BH_NET_H

#include <stddef.h>
#include <stdint.h>

/* Reads exactly n bytes. Returns 1 on success, 0 if the peer closed the
 * connection before the first byte (clean EOF), -1 on error or if the
 * connection ended part-way through. Retries on EINTR. */
int read_full(int fd, void *buf, size_t n);

/* Writes exactly n bytes. Returns 0 on success, -1 on error. Retries on EINTR
 * and continues after short writes. Never raises SIGPIPE. */
int write_full(int fd, const void *buf, size_t n);

enum {
    RECV_OK,       /* *frame holds one complete frame; caller frees it */
    RECV_EOF,      /* peer closed the connection cleanly between frames */
    RECV_IO,       /* I/O error, timeout, or connection lost mid-frame */
    RECV_DESYNC,   /* bad magic or version: frame boundaries are lost */
    RECV_REJECTED  /* well-formed but refused (non-zero flags or too large);
                      its payload was consumed, the stream is still in sync */
};

/*
 * Receives the next frame of type want_type, header included, into a freshly
 * allocated buffer. Frames of any other type are skipped using their length
 * field (SPEC 6.3); *skipped, if not NULL, is incremented for each one.
 * At most max_payload bytes are ever allocated for the payload.
 */
int recv_frame(int fd, uint8_t want_type, size_t max_payload, uint8_t **frame,
               size_t *frame_len, unsigned *skipped);

#endif
