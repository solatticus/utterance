#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "font.h"
#include "gl_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char *ttf_data;
    stbtt_fontinfo info;
} FontInternal;

static unsigned char *read_file_bytes(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(*len);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, *len, f) != *len) { free(buf); fclose(f); return NULL; }
    fclose(f);
    return buf;
}

static void font_render_glyph(Font *f, int cp) {
    FontInternal *fi = f->internal;
    Glyph *g = &f->glyphs[cp];
    g->rendered = 1;

    int gi = stbtt_FindGlyphIndex(&fi->info, cp);
    if (gi == 0) return;

    int w, h, xoff, yoff;
    unsigned char *sdf = stbtt_GetGlyphSDF(&fi->info, f->scale, gi,
                                            FONT_SDF_PADDING,
                                            (unsigned char)f->onedge, f->dist_scale,
                                            &w, &h, &xoff, &yoff);
    if (!sdf || w == 0 || h == 0) {
        if (sdf) stbtt_FreeSDF(sdf, NULL);
        return;
    }

    /* Pack into atlas */
    if (f->pen_x + w > f->atlas_w) {
        f->pen_x = 0;
        f->pen_y += f->row_h + 1;
        f->row_h = 0;
    }
    if (f->pen_y + h > f->atlas_h) {
        stbtt_FreeSDF(sdf, NULL);
        return; /* atlas full */
    }

    g->s0 = (float)f->pen_x / f->atlas_w;
    g->t0 = (float)f->pen_y / f->atlas_h;
    g->s1 = (float)(f->pen_x + w) / f->atlas_w;
    g->t1 = (float)(f->pen_y + h) / f->atlas_h;
    g->x_off = (float)xoff;
    g->y_off = (float)yoff;
    g->width = (float)w;
    g->height = (float)h;

    /* Upload to GPU */
    glBindTexture(GL_TEXTURE_2D, f->texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, f->pen_x, f->pen_y, w, h,
                    GL_RED, GL_UNSIGNED_BYTE, sdf);

    f->pen_x += w + 1;
    if (h > f->row_h) f->row_h = h;

    stbtt_FreeSDF(sdf, NULL);
}

int font_load(Font *f, const char *ttf_path, float px_size) {
    memset(f, 0, sizeof(*f));
    f->px_size = px_size;
    f->atlas_w = FONT_ATLAS_SIZE;
    f->atlas_h = FONT_ATLAS_SIZE;
    f->onedge = 128.0f;
    f->dist_scale = f->onedge / (float)FONT_SDF_PADDING;

    size_t ttf_len;
    unsigned char *ttf_data = read_file_bytes(ttf_path, &ttf_len);
    if (!ttf_data) {
        fprintf(stderr, "utterance: can't load font %s\n", ttf_path);
        return -1;
    }

    FontInternal *fi = malloc(sizeof(FontInternal));
    if (!fi) { free(ttf_data); return -1; }
    fi->ttf_data = ttf_data;

    if (!stbtt_InitFont(&fi->info, ttf_data, stbtt_GetFontOffsetForIndex(ttf_data, 0))) {
        fprintf(stderr, "utterance: invalid font %s\n", ttf_path);
        free(ttf_data);
        free(fi);
        return -1;
    }
    f->internal = fi;

    f->scale = stbtt_ScaleForPixelHeight(&fi->info, px_size);

    int asc, desc, lg;
    stbtt_GetFontVMetrics(&fi->info, &asc, &desc, &lg);
    f->ascent = asc * f->scale;
    f->descent = desc * f->scale;
    f->line_gap = lg * f->scale;

    /* Phase 1: Scan all BMP codepoints for metrics only (fast — no rasterization) */
    for (int cp = 32; cp < FONT_MAX_CODEPOINT; cp++) {
        int gi = stbtt_FindGlyphIndex(&fi->info, cp);
        if (gi == 0) continue;
        int advance, lsb;
        stbtt_GetGlyphHMetrics(&fi->info, gi, &advance, &lsb);
        f->glyphs[cp].advance = advance * f->scale;
        f->glyphs[cp].present = 1;
    }

    /* Create atlas texture — zeroed so unrendered regions are transparent */
    glGenTextures(1, &f->texture);
    glBindTexture(GL_TEXTURE_2D, f->texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    unsigned char *zeros = calloc((size_t)f->atlas_w * f->atlas_h, 1);
    if (!zeros) {
        fprintf(stderr, "utterance: atlas allocation failed\n");
        free(ttf_data);
        free(fi);
        f->internal = NULL;
        return -1;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, f->atlas_w, f->atlas_h, 0,
                 GL_RED, GL_UNSIGNED_BYTE, zeros);
    free(zeros);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* Phase 2: Pre-render ASCII for instant first frame */
    for (int cp = 32; cp < 128; cp++) {
        if (f->glyphs[cp].present)
            font_render_glyph(f, cp);
    }

    return 0;
}

Glyph *font_glyph(Font *f, uint32_t codepoint) {
    if (codepoint < FONT_MAX_CODEPOINT && f->glyphs[codepoint].present) {
        if (!f->glyphs[codepoint].rendered)
            font_render_glyph(f, (int)codepoint);
        return &f->glyphs[codepoint];
    }
    /* Tofu fallback */
    if (f->glyphs['?'].present)
        return &f->glyphs['?'];
    return NULL;
}

void font_destroy(Font *f) {
    if (f->texture) glDeleteTextures(1, &f->texture);
    FontInternal *fi = f->internal;
    if (fi) {
        free(fi->ttf_data);
        free(fi);
    }
    f->internal = NULL;
}
