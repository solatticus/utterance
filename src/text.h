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
    float r, g, b;      /* per-glyph color (ANSI or default) */
    /* Optional 2D rotation around (pivot_x, pivot_y) applied by text_upload.
     * Identity when rot_cos=1, rot_sin=0 (pivot is then irrelevant). Producers
     * that don't rotate can leave these at zero and text_layout defaults them
     * to identity on emission. */
    float pivot_x, pivot_y;
    float rot_cos, rot_sin;
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

/* Append a single already-positioned glyph to a mesh. Used by non-layout
 * producers (SVG text extractor) that do their own positioning. */
int  text_mesh_push(TextMesh *mesh, const GlyphInstance *inst);

void text_prepare(PreparedText *pt, Font *font, const uint8_t *text, size_t len);
void text_layout(TextMesh *mesh, PreparedText *pt, Font *font, float wrap_width, const float default_color[3]);
void text_relayout(TextMesh *mesh, PreparedText *pt, Font *font, float wrap_width, const float default_color[3]);
void text_upload(TextMesh *mesh);
int  text_hit_test(const TextMesh *mesh, float x, float y, float radius);
int  text_word_bounds(const TextMesh *mesh, const PreparedText *pt, float x, float y, float out[4]);
int  text_glyph_at(const TextMesh *mesh, float x, float y);
int  text_selection_rects(const TextMesh *mesh, int g0, int g1, float rects[][4], int max_rects);
/* Cursor navigation */
int  text_line_start(const TextMesh *mesh, int idx);
int  text_line_end(const TextMesh *mesh, int idx);
int  text_line_up(const TextMesh *mesh, int idx, float target_x);
int  text_line_down(const TextMesh *mesh, int idx, float target_x);

void text_destroy(TextMesh *mesh);
void prepared_text_destroy(PreparedText *pt);

#endif
