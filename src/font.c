#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "font.h"
#include "gl_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *read_file_bytes(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(*len);
    if (fread(buf, 1, *len, f) != *len) { free(buf); fclose(f); return NULL; }
    fclose(f);
    return buf;
}

int font_load(Font *f, const char *ttf_path, float px_size) {
    memset(f, 0, sizeof(*f));
    f->px_size = px_size;

    size_t ttf_len;
    unsigned char *ttf_data = read_file_bytes(ttf_path, &ttf_len);
    if (!ttf_data) {
        fprintf(stderr, "utterance: can't load font %s\n", ttf_path);
        return -1;
    }

    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttf_data, stbtt_GetFontOffsetForIndex(ttf_data, 0))) {
        fprintf(stderr, "utterance: invalid font %s\n", ttf_path);
        free(ttf_data);
        return -1;
    }

    f->scale = stbtt_ScaleForPixelHeight(&info, px_size);

    int asc, desc, lg;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &lg);
    f->ascent = asc * f->scale;
    f->descent = desc * f->scale;
    f->line_gap = lg * f->scale;

    /* SDF atlas generation */
    int atlas_w = 4096, atlas_h = 4096;
    unsigned char *atlas = calloc(atlas_w * atlas_h, 1);
    int pen_x = 0, pen_y = 0, row_h = 0;
    int padding = 5;
    float onedge = 128.0f;
    float dist_scale = onedge / (float)padding;

    /* Walk the entire BMP — if the font has it, atlas it */
    for (int cp = 32; cp < 65536; cp++) {
        {
            int gi = stbtt_FindGlyphIndex(&info, cp);
            if (gi == 0) continue;

            /* Always store advance for present glyphs (space, etc.) */
            int advance, lsb;
            stbtt_GetGlyphHMetrics(&info, gi, &advance, &lsb);
            f->glyphs[cp].advance = advance * f->scale;
            f->glyphs[cp].present = 1;

            int w, h, xoff, yoff;
            unsigned char *sdf = stbtt_GetGlyphSDF(&info, f->scale, gi, padding,
                                                    (unsigned char)onedge, dist_scale,
                                                    &w, &h, &xoff, &yoff);
            if (!sdf || w == 0 || h == 0) {
                if (sdf) stbtt_FreeSDF(sdf, NULL);
                continue;
            }

            /* Advance to next row if needed */
            if (pen_x + w > atlas_w) {
                pen_x = 0;
                pen_y += row_h + 1;
                row_h = 0;
            }
            if (pen_y + h > atlas_h) {
                /* Atlas full — stop generating */
                stbtt_FreeSDF(sdf, NULL);
                break;
            }

            /* Blit into atlas */
            for (int y = 0; y < h; y++)
                memcpy(atlas + (pen_y + y) * atlas_w + pen_x, sdf + y * w, w);

            /* Store glyph metrics */
            Glyph *g = &f->glyphs[cp];
            g->s0 = (float)pen_x / atlas_w;
            g->t0 = (float)pen_y / atlas_h;
            g->s1 = (float)(pen_x + w) / atlas_w;
            g->t1 = (float)(pen_y + h) / atlas_h;
            g->x_off = (float)xoff;
            g->y_off = (float)yoff;
            g->width = (float)w;
            g->height = (float)h;

            pen_x += w + 1;
            if (h > row_h) row_h = h;

            stbtt_FreeSDF(sdf, NULL);
        }
    }  /* end BMP walk */

    /* Upload atlas to GPU */
    glGenTextures(1, &f->texture);
    glBindTexture(GL_TEXTURE_2D, f->texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlas_w, atlas_h, 0, GL_RED, GL_UNSIGNED_BYTE, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    f->atlas_w = atlas_w;
    f->atlas_h = atlas_h;

    free(atlas);
    free(ttf_data);
    return 0;
}

Glyph *font_glyph(Font *f, uint32_t codepoint) {
    if (codepoint < 65536 && f->glyphs[codepoint].present)
        return &f->glyphs[codepoint];
    /* Tofu fallback: return '?' glyph */
    if (f->glyphs['?'].present)
        return &f->glyphs['?'];
    return NULL;
}

void font_destroy(Font *f) {
    if (f->texture) glDeleteTextures(1, &f->texture);
}
