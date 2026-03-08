#ifndef TEXT_H
#define TEXT_H

#include <stdint.h>
#include <stddef.h>
#include "font.h"

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

void text_layout(TextMesh *mesh, Font *font, const uint8_t *text, size_t len, float wrap_width);
void text_upload(TextMesh *mesh);
int  text_hit_test(const TextMesh *mesh, float x, float y, float radius);
int  text_word_bounds(const TextMesh *mesh, float x, float y, float out[4]); /* x0,y0,x1,y1 */
int  text_glyph_at(const TextMesh *mesh, float x, float y);  /* nearest glyph index, -1 if none */
int  text_selection_rects(const TextMesh *mesh, int g0, int g1, float rects[][4], int max_rects);
void text_destroy(TextMesh *mesh);

#endif
