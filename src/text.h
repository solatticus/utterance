#ifndef TEXT_H
#define TEXT_H

#include <stdint.h>
#include <stddef.h>
#include "font.h"
#include "segment.h"
#include "linebreak.h"

typedef struct {
    float x, y, z;
    float w, h;
    float s0, t0, s1, t1;
    int text_offset;    /* byte offset in source text */
} GlyphInstance;

typedef struct {
    GlyphInstance *instances;
    int count;
    int capacity;
    unsigned int vao, vbo;
} TextMesh;

typedef struct {
    const uint8_t *text;
    size_t         text_len;
    SegmentList    segs;
    LineList       lines;
    float          wrap_width;
} PreparedText;

/* Phase 1: analyze + measure (run once, or on new text) */
void text_prepare(PreparedText *pt, Font *font, const uint8_t *text, size_t len);

/* Phase 2: line-break + build mesh (cheap, re-run on wrap change) */
void text_layout(TextMesh *mesh, PreparedText *pt, Font *font, float wrap_width);

/* Re-layout only if wrap_width changed. Handles re-upload. */
void text_relayout(TextMesh *mesh, PreparedText *pt, Font *font, float wrap_width);

void text_upload(TextMesh *mesh);
int  text_hit_test(const TextMesh *mesh, float x, float y, float radius);
int  text_word_bounds(const TextMesh *mesh, const PreparedText *pt, float x, float y, float out[4]);
int  text_glyph_at(const TextMesh *mesh, float x, float y);
int  text_selection_rects(const TextMesh *mesh, int g0, int g1, float rects[][4], int max_rects);
void text_destroy(TextMesh *mesh);
void prepared_text_destroy(PreparedText *pt);

#endif
