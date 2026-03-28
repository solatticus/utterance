#ifndef SEGMENT_H
#define SEGMENT_H

#include <stdint.h>
#include <stddef.h>
#include "font.h"

typedef enum {
    SEG_WORD,
    SEG_SPACE,
    SEG_NEWLINE,
    SEG_TAB,
} SegKind;

typedef struct {
    int      byte_start;
    int      byte_len;
    float    width;
    SegKind  kind;
} Segment;

typedef struct {
    Segment *segs;
    int      count;
    int      capacity;
} SegmentList;

/* Analyze UTF-8 text into segments. Resets out before populating. */
void segment_analyze(SegmentList *out, const uint8_t *text, size_t len);

/* Analyze new bytes starting at byte offset 'from'. Merges with last segment if needed. */
void segment_analyze_append(SegmentList *out, const uint8_t *text, size_t len, size_t from);

/* Measure segment widths using font advances. */
void segment_measure(SegmentList *segs, Font *font, const uint8_t *text);

/* Measure only segments from index 'from_seg' onward. */
void segment_measure_from(SegmentList *segs, Font *font, const uint8_t *text, int from_seg);

void segment_list_destroy(SegmentList *sl);

#endif
