#ifndef MARKDOWN_H
#define MARKDOWN_H

#include <stdint.h>
#include <stddef.h>

/* Transform markdown source into ANSI-colored UTF-8.
   Allocates a new buffer (caller frees). Returns NULL on failure.
   Strips markdown syntax chars, replaces with ANSI SGR sequences. */
uint8_t *markdown_to_ansi(const uint8_t *src, size_t src_len, size_t *out_len);

/* Detect if buffer looks like markdown (heuristic). */
int markdown_detect(const uint8_t *src, size_t len);

#endif
