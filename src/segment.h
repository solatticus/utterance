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
    SEG_CJK,          /* single CJK character — breakable between adjacent */
    SEG_ANSI,          /* ANSI escape sequence — zero width, invisible */
    SEG_SOFTHYPHEN,    /* U+00AD — zero width, visible hyphen at break */
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

void segment_analyze(SegmentList *out, const uint8_t *text, size_t len);
void segment_analyze_append(SegmentList *out, const uint8_t *text, size_t len, size_t from);
void segment_measure(SegmentList *segs, Font *font, const uint8_t *text);
void segment_measure_from(SegmentList *segs, Font *font, const uint8_t *text, int from_seg);
void segment_list_destroy(SegmentList *sl);

#endif
