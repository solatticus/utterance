#ifndef FONT_H
#define FONT_H

#include <stdint.h>

#define FONT_ATLAS_SIZE    4096
#define FONT_MAX_CODEPOINT 65536
#define FONT_SDF_PADDING   5

typedef struct {
    float s0, t0, s1, t1;   /* UV in atlas */
    float x_off, y_off;     /* bearing offset */
    float width, height;    /* quad size (pixels) */
    float advance;          /* horizontal advance (scaled) */
    uint8_t present;        /* glyph exists in font */
    uint8_t rendered;       /* SDF generated and atlased */
} Glyph;

typedef struct {
    unsigned int texture;    /* GL texture handle */
    int atlas_w, atlas_h;
    float px_size;
    float ascent, descent, line_gap;
    float scale;
    Glyph glyphs[FONT_MAX_CODEPOINT]; /* BMP direct lookup */

    /* Lazy atlas packing state */
    void *internal;          /* opaque: ttf_data + stbtt_fontinfo */
    int pen_x, pen_y, row_h;
    float onedge, dist_scale;
} Font;

int   font_load(Font *f, const char *ttf_path, float px_size);
Glyph *font_glyph(Font *f, uint32_t codepoint);
void  font_destroy(Font *f);

#endif
