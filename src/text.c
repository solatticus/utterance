#include "text.h"
#include "gl_loader.h"
#include "utf8.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int push_instance(TextMesh *mesh, GlyphInstance *inst) {
    if (mesh->count >= mesh->capacity) {
        int new_cap = mesh->capacity ? mesh->capacity * 2 : 4096;
        GlyphInstance *tmp = realloc(mesh->instances, (size_t)new_cap * sizeof(GlyphInstance));
        if (!tmp) return -1;
        mesh->instances = tmp;
        mesh->capacity = new_cap;
    }
    mesh->instances[mesh->count++] = *inst;
    return 0;
}

void text_prepare(PreparedText *pt, Font *font, const uint8_t *text, size_t len) {
    memset(pt, 0, sizeof(*pt));
    pt->text = text;
    pt->text_len = len;
    pt->wrap_width = -1.0f; /* force first layout */
    segment_analyze(&pt->segs, text, len);
    segment_measure(&pt->segs, font, text);
}

void text_layout(TextMesh *mesh, PreparedText *pt, Font *font, float wrap_width) {
    memset(mesh, 0, sizeof(*mesh));

    linebreak_run(&pt->lines, pt->segs.segs, pt->segs.count,
                  wrap_width, font, pt->text);
    pt->wrap_width = wrap_width;

    float line_height = font->ascent - font->descent + font->line_gap;
    float cursor_y = 0.0f;

    for (int li = 0; li < pt->lines.count; li++) {
        Line *line = &pt->lines.lines[li];
        float cursor_x = 0.0f;

        for (int si = line->seg_start; si < line->seg_end; si++) {
            Segment *seg = &pt->segs.segs[si];

            if (seg->kind == SEG_TAB) {
                cursor_x += seg->width;
                continue;
            }
            if (seg->kind == SEG_SPACE) {
                cursor_x += seg->width;
                continue;
            }

            /* SEG_WORD: emit individual glyphs */
            const uint8_t *p = pt->text + seg->byte_start;
            const uint8_t *end = p + seg->byte_len;

            /* For overflow words wider than wrap, do character-level wrapping */
            float seg_remaining = (wrap_width > 0) ? (wrap_width - cursor_x) : 0;
            int do_char_wrap = (wrap_width > 0 && seg->width > wrap_width && seg_remaining >= 0);

            while (p < end) {
                const uint8_t *cp_start = p;
                uint32_t cp = utf8_decode(&p, end);
                if (cp == 0) break;

                Glyph *g = font_glyph(font, cp);
                if (!g) continue;

                /* Character-level wrap for overflow words */
                if (do_char_wrap && cursor_x + g->advance > wrap_width && cursor_x > 0) {
                    cursor_x = 0.0f;
                    cursor_y -= line_height;
                }

                if (g->width > 0 && g->height > 0) {
                    GlyphInstance inst;
                    inst.x = cursor_x + g->x_off;
                    inst.y = cursor_y - g->y_off - g->height;
                    inst.z = 0.0f;
                    inst.w = g->width;
                    inst.h = g->height;
                    inst.s0 = g->s0;
                    inst.t0 = g->t0;
                    inst.s1 = g->s1;
                    inst.t1 = g->t1;
                    inst.text_offset = (int)(cp_start - pt->text);
                    if (push_instance(mesh, &inst) < 0) return;
                }
                cursor_x += g->advance;
            }
        }

        cursor_y -= line_height;
    }
}

void text_relayout(TextMesh *mesh, PreparedText *pt, Font *font, float wrap_width) {
    /* Threshold: skip if wrap width barely changed */
    float diff = wrap_width - pt->wrap_width;
    if (diff < 0) diff = -diff;
    if (diff < 1.0f) return;

    /* Tear down old GPU resources */
    if (mesh->vao) { glDeleteVertexArrays(1, &mesh->vao); mesh->vao = 0; }
    if (mesh->vbo) { glDeleteBuffers(1, &mesh->vbo); mesh->vbo = 0; }
    free(mesh->instances);
    mesh->instances = NULL;
    mesh->count = 0;
    mesh->capacity = 0;

    text_layout(mesh, pt, font, wrap_width);
    text_upload(mesh);
}

void text_upload(TextMesh *mesh) {
    if (mesh->count == 0) return;

    int verts_per_glyph = 6;
    int floats_per_vert = 5;
    size_t total_floats = (size_t)mesh->count * verts_per_glyph * floats_per_vert;
    float *verts = malloc(total_floats * sizeof(float));
    if (!verts) return;

    for (int i = 0; i < mesh->count; i++) {
        GlyphInstance *g = &mesh->instances[i];
        float x0 = g->x, y0 = g->y, z = g->z;
        float x1 = x0 + g->w, y1 = y0 + g->h;
        float s0 = g->s0, t0 = g->t0, s1 = g->s1, t1 = g->t1;

        float *v = verts + i * verts_per_glyph * floats_per_vert;
        v[0]=x0; v[1]=y1; v[2]=z; v[3]=s0; v[4]=t0;
        v[5]=x0; v[6]=y0; v[7]=z; v[8]=s0; v[9]=t1;
        v[10]=x1; v[11]=y0; v[12]=z; v[13]=s1; v[14]=t1;
        v[15]=x0; v[16]=y1; v[17]=z; v[18]=s0; v[19]=t0;
        v[20]=x1; v[21]=y0; v[22]=z; v[23]=s1; v[24]=t1;
        v[25]=x1; v[26]=y1; v[27]=z; v[28]=s1; v[29]=t0;
    }

    glGenVertexArrays(1, &mesh->vao);
    glBindVertexArray(mesh->vao);

    glGenBuffers(1, &mesh->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
    glBufferData(GL_ARRAY_BUFFER, total_floats * sizeof(float), verts, GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, floats_per_vert * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, floats_per_vert * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
    free(verts);
}

int text_hit_test(const TextMesh *mesh, float x, float y, float radius) {
    for (int i = 0; i < mesh->count; i++) {
        GlyphInstance *g = &mesh->instances[i];
        float cx = g->x + g->w * 0.5f;
        float cy = g->y + g->h * 0.5f;
        float dx = x - cx;
        float dy = y - cy;
        if (dx * dx + dy * dy < radius * radius)
            return 1;
    }
    return 0;
}

int text_word_bounds(const TextMesh *mesh, const PreparedText *pt, float x, float y, float out[4]) {
    /* Find closest glyph */
    int hit = text_glyph_at(mesh, x, y);
    if (hit < 0) return 0;

    int hit_offset = mesh->instances[hit].text_offset;

    /* Find the SEG_WORD segment containing this byte offset */
    int seg_idx = -1;
    for (int i = 0; i < pt->segs.count; i++) {
        Segment *s = &pt->segs.segs[i];
        if (s->kind == SEG_WORD &&
            hit_offset >= s->byte_start &&
            hit_offset < s->byte_start + s->byte_len) {
            seg_idx = i;
            break;
        }
    }
    if (seg_idx < 0) return 0;

    /* Find all glyphs belonging to this segment */
    Segment *seg = &pt->segs.segs[seg_idx];
    int seg_end = seg->byte_start + seg->byte_len;

    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < mesh->count; i++) {
        GlyphInstance *g = &mesh->instances[i];
        if (g->text_offset >= seg->byte_start && g->text_offset < seg_end) {
            if (g->x < x0) x0 = g->x;
            if (g->y < y0) y0 = g->y;
            if (g->x + g->w > x1) x1 = g->x + g->w;
            if (g->y + g->h > y1) y1 = g->y + g->h;
        }
    }

    out[0] = x0 - 2.0f;
    out[1] = y0 - 2.0f;
    out[2] = x1 + 2.0f;
    out[3] = y1 + 2.0f;
    return 1;
}

int text_glyph_at(const TextMesh *mesh, float x, float y) {
    float best = 1e30f;
    int hit = -1;
    for (int i = 0; i < mesh->count; i++) {
        GlyphInstance *g = &mesh->instances[i];
        float cx = g->x + g->w * 0.5f;
        float cy = g->y + g->h * 0.5f;
        float d = (x - cx)*(x - cx) + (y - cy)*(y - cy);
        if (d < best) { best = d; hit = i; }
    }
    return hit;
}

int text_selection_rects(const TextMesh *mesh, int g0, int g1, float rects[][4], int max_rects) {
    if (g0 < 0 || g1 < 0 || mesh->count == 0 || max_rects <= 0) return 0;
    int lo = g0 < g1 ? g0 : g1;
    int hi = g0 < g1 ? g1 : g0;
    if (lo < 0) lo = 0;
    if (hi >= mesh->count) hi = mesh->count - 1;

    int n = 0;
    GlyphInstance *first = &mesh->instances[lo];
    float rx0 = first->x, ry0 = first->y;
    float rx1 = first->x + first->w, ry1 = first->y + first->h;
    float line_y = first->y;
    float tol = first->h * 0.5f;

    for (int i = lo + 1; i <= hi; i++) {
        GlyphInstance *g = &mesh->instances[i];
        if (fabsf(g->y - line_y) > tol) {
            if (n < max_rects) {
                rects[n][0] = rx0; rects[n][1] = ry0;
                rects[n][2] = rx1; rects[n][3] = ry1;
                n++;
            }
            line_y = g->y;
            tol = g->h * 0.5f;
            rx0 = g->x; ry0 = g->y;
            rx1 = g->x + g->w; ry1 = g->y + g->h;
        } else {
            if (g->x < rx0) rx0 = g->x;
            if (g->y < ry0) ry0 = g->y;
            if (g->x + g->w > rx1) rx1 = g->x + g->w;
            if (g->y + g->h > ry1) ry1 = g->y + g->h;
        }
    }
    if (n < max_rects) {
        rects[n][0] = rx0; rects[n][1] = ry0;
        rects[n][2] = rx1; rects[n][3] = ry1;
        n++;
    }
    return n;
}

void text_destroy(TextMesh *mesh) {
    if (mesh->vao) glDeleteVertexArrays(1, &mesh->vao);
    if (mesh->vbo) glDeleteBuffers(1, &mesh->vbo);
    free(mesh->instances);
    memset(mesh, 0, sizeof(*mesh));
}

void prepared_text_destroy(PreparedText *pt) {
    segment_list_destroy(&pt->segs);
    line_list_destroy(&pt->lines);
    memset(pt, 0, sizeof(*pt));
}
