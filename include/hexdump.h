/*
 * hexdump.h - annotated hexdump of one BH/1 frame.
 */
#ifndef BH_HEXDUMP_H
#define BH_HEXDUMP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Prints the actual wire bytes of one frame, one protocol field per row
 * group: offset, hex bytes, printable ASCII, field meaning. `direction` is a
 * free-form label such as "request (client -> server)".
 */
void hexdump_frame(FILE *out, const char *direction, const uint8_t *frame, size_t len);

#endif
