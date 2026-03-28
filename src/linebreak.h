#ifndef LINEBREAK_H
#define LINEBREAK_H

#include "segment.h"

typedef struct {
    int   seg_start;
    int   seg_end;      /* one past last */
    float width;
} Line;

typedef struct {
    Line  *lines;
    int    count;
    int    capacity;
} LineList;

/* Greedy line-breaking on measured segments.
   max_width <= 0 means no wrapping (one line per hard break).
   text + font are needed only for character-level overflow breaking. */
void linebreak_run(LineList *out, const Segment *segs, int seg_count,
                   float max_width, Font *font, const uint8_t *text);

void line_list_destroy(LineList *ll);

#endif
