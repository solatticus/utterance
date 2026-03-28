#ifndef LINEBREAK_H
#define LINEBREAK_H

#include "segment.h"

typedef struct {
    int   seg_start;
    int   seg_end;      /* one past last */
    float width;
    int   has_hyphen;   /* line ends with a soft-hyphen break — emit visible '-' */
    int   byte_offset;  /* byte offset into text for overflow sub-line start (0 = normal) */
} Line;

typedef struct {
    Line  *lines;
    int    count;
    int    capacity;
} LineList;

void linebreak_run(LineList *out, const Segment *segs, int seg_count,
                   float max_width, Font *font, const uint8_t *text);

void line_list_destroy(LineList *ll);

#endif
