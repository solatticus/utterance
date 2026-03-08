#ifndef FONT_H
#define FONT_H

#include <stdint.h>

typedef struct {
    float s0, t0, s1, t1;   /* UV in atlas */
    float x_off, y_off;     /* bearing offset */
    float width, height;    /* quad size (normalized) */
    float advance;           /* horizontal advance (normalized) */
    int present;
} Glyph;

typedef struct {
    unsigned int texture;    /* GL texture handle */
    int atlas_w, atlas_h;
    float px_size;
    float ascent, descent, line_gap;
    float scale;
    Glyph glyphs[65536];    /* BMP direct lookup */
} Font;

int   font_load(Font *f, const char *ttf_path, float px_size);
Glyph *font_glyph(Font *f, uint32_t codepoint);
void  font_destroy(Font *f);

#endif
