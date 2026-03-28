#include "text.h"
#include "gl_loader.h"
#include "utf8.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- ANSI SGR color parsing ---- */

typedef struct {
    float r, g, b;
    int bold;
} AnsiState;

static void ansi_color_256(int idx, float out[3]) {
    static const float std[16][3] = {
        {0.00f, 0.00f, 0.00f}, {0.67f, 0.00f, 0.00f},
        {0.00f, 0.67f, 0.00f}, {0.67f, 0.33f, 0.00f},
        {0.00f, 0.00f, 0.67f}, {0.67f, 0.00f, 0.67f},
        {0.00f, 0.67f, 0.67f}, {0.67f, 0.67f, 0.67f},
        {0.33f, 0.33f, 0.33f}, {1.00f, 0.33f, 0.33f},
        {0.33f, 1.00f, 0.33f}, {1.00f, 1.00f, 0.33f},
        {0.33f, 0.33f, 1.00f}, {1.00f, 0.33f, 1.00f},
        {0.33f, 1.00f, 1.00f}, {1.00f, 1.00f, 1.00f},
    };
    if (idx < 0) idx = 0;
    if (idx < 16) {
        out[0] = std[idx][0]; out[1] = std[idx][1]; out[2] = std[idx][2];
    } else if (idx < 232) {
        int v = idx - 16;
        int r = v / 36, g = (v / 6) % 6, b = v % 6;
        out[0] = r ? (55.0f + 40.0f * r) / 255.0f : 0.0f;
        out[1] = g ? (55.0f + 40.0f * g) / 255.0f : 0.0f;
        out[2] = b ? (55.0f + 40.0f * b) / 255.0f : 0.0f;
    } else if (idx < 256) {
        float gray = (8.0f + (idx - 232) * 10.0f) / 255.0f;
        out[0] = out[1] = out[2] = gray;
    }
}

static void ansi_reset(AnsiState *s, const float def[3]) {
    s->r = def[0]; s->g = def[1]; s->b = def[2];
    s->bold = 0;
}

static void ansi_apply_sgr(AnsiState *state, const uint8_t *bytes, int len,
                           const float def[3]) {
    if (len < 3) return;
    if (bytes[0] != 0x1B || bytes[1] != '[') return;
    if (bytes[len - 1] != 'm') return;

    int params[16];
    int np = 0;
    int cur = 0;
    int has_digit = 0;

    for (int i = 2; i < len - 1 && np < 16; i++) {
        if (bytes[i] >= '0' && bytes[i] <= '9') {
            cur = cur * 10 + (bytes[i] - '0');
            has_digit = 1;
        } else if (bytes[i] == ';') {
            params[np++] = has_digit ? cur : 0;
            cur = 0;
            has_digit = 0;
        }
    }
    if (np < 16) params[np++] = has_digit ? cur : 0;

    for (int i = 0; i < np; i++) {
        int p = params[i];
        if (p == 0) {
            ansi_reset(state, def);
        } else if (p == 1) {
            state->bold = 1;
        } else if (p == 22) {
            state->bold = 0;
        } else if (p >= 30 && p <= 37) {
            float c[3];
            ansi_color_256(state->bold ? (p - 30 + 8) : (p - 30), c);
            state->r = c[0]; state->g = c[1]; state->b = c[2];
        } else if (p == 39) {
            state->r = def[0]; state->g = def[1]; state->b = def[2];
        } else if (p >= 90 && p <= 97) {
            float c[3];
            ansi_color_256(p - 90 + 8, c);
            state->r = c[0]; state->g = c[1]; state->b = c[2];
        } else if (p == 38 && i + 1 < np) {
            if (params[i + 1] == 5 && i + 2 < np) {
                float c[3];
                ansi_color_256(params[i + 2], c);
                state->r = c[0]; state->g = c[1]; state->b = c[2];
                i += 2;
            } else if (params[i + 1] == 2 && i + 4 < np) {
                state->r = params[i + 2] / 255.0f;
                state->g = params[i + 3] / 255.0f;
                state->b = params[i + 4] / 255.0f;
                i += 4;
            }
        }
    }
}

/* ---- Mesh building ---- */

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

static int emit_glyph(TextMesh *mesh, Font *font, uint32_t cp,
                      float *cursor_x, float cursor_y, const uint8_t *cp_start,
                      const uint8_t *text_base, const AnsiState *color) {
    Glyph *g = font_glyph(font, cp);
    if (!g) return 0;

    if (g->width > 0 && g->height > 0) {
        GlyphInstance inst;
        inst.x = *cursor_x + g->x_off;
        inst.y = cursor_y - g->y_off - g->height;
        inst.z = 0.0f;
        inst.w = g->width;
        inst.h = g->height;
        inst.s0 = g->s0; inst.t0 = g->t0;
        inst.s1 = g->s1; inst.t1 = g->t1;
        inst.text_offset = (int)(cp_start - text_base);
        inst.r = color->r; inst.g = color->g; inst.b = color->b;
        if (push_instance(mesh, &inst) < 0) return -1;
    }
    *cursor_x += g->advance;
    return 0;
}

void text_prepare(PreparedText *pt, Font *font, const uint8_t *text, size_t len) {
    memset(pt, 0, sizeof(*pt));
    pt->text = text;
    pt->text_len = len;
    pt->wrap_width = -1.0f;
    segment_analyze(&pt->segs, text, len);
    segment_measure(&pt->segs, font, text);
}

void text_layout(TextMesh *mesh, PreparedText *pt, Font *font, float wrap_width,
                 const float default_color[3]) {
    memset(mesh, 0, sizeof(*mesh));

    linebreak_run(&pt->lines, pt->segs.segs, pt->segs.count,
                  wrap_width, font, pt->text);
    pt->wrap_width = wrap_width;

    float line_height = font->ascent - font->descent + font->line_gap;
    float cursor_y = 0.0f;

    AnsiState color;
    ansi_reset(&color, default_color);

    for (int li = 0; li < pt->lines.count; li++) {
        Line *line = &pt->lines.lines[li];
        float cursor_x = 0.0f;

        for (int si = line->seg_start; si < line->seg_end; si++) {
            Segment *seg = &pt->segs.segs[si];

            /* ANSI: parse color, emit nothing visible */
            if (seg->kind == SEG_ANSI) {
                ansi_apply_sgr(&color, pt->text + seg->byte_start,
                               seg->byte_len, default_color);
                continue;
            }

            if (seg->kind == SEG_TAB) {
                cursor_x += seg->width;
                continue;
            }
            if (seg->kind == SEG_SPACE) {
                cursor_x += seg->width;
                continue;
            }
            if (seg->kind == SEG_SOFTHYPHEN) {
                /* Invisible unless at a line break — handled below */
                continue;
            }

            /* SEG_WORD or SEG_CJK: emit glyphs */
            int byte_start = seg->byte_start;
            int byte_end = seg->byte_start + seg->byte_len;

            /* Overflow sub-line: start at byte_offset instead of segment start */
            if (line->byte_offset > 0 && line->byte_offset > byte_start &&
                line->byte_offset < byte_end) {
                byte_start = line->byte_offset;
            }

            const uint8_t *p = pt->text + byte_start;
            const uint8_t *end = pt->text + byte_end;

            while (p < end) {
                const uint8_t *cp_start = p;
                uint32_t cp = utf8_decode(&p, end);
                if (cp == 0) break;
                if (emit_glyph(mesh, font, cp, &cursor_x, cursor_y,
                               cp_start, pt->text, &color) < 0)
                    return;
            }
        }

        /* Soft-hyphen at line break: emit visible hyphen */
        if (line->has_hyphen) {
            const uint8_t *hyp_pos = pt->text + pt->segs.segs[line->seg_end - 1].byte_start;
            emit_glyph(mesh, font, '-', &cursor_x, cursor_y,
                       hyp_pos, pt->text, &color);
        }

        cursor_y -= line_height;
    }
}

void text_relayout(TextMesh *mesh, PreparedText *pt, Font *font, float wrap_width,
                   const float default_color[3]) {
    float diff = wrap_width - pt->wrap_width;
    if (diff < 0) diff = -diff;
    if (diff < 1.0f) return;

    if (mesh->vao) { glDeleteVertexArrays(1, &mesh->vao); mesh->vao = 0; }
    if (mesh->vbo) { glDeleteBuffers(1, &mesh->vbo); mesh->vbo = 0; }
    free(mesh->instances);
    mesh->instances = NULL;
    mesh->count = 0;
    mesh->capacity = 0;

    text_layout(mesh, pt, font, wrap_width, default_color);
    text_upload(mesh);
}

void text_upload(TextMesh *mesh) {
    if (mesh->count == 0) return;

    /* 6 vertices per quad, 8 floats each (x,y,z,u,v,r,g,b) */
    int verts_per_glyph = 6;
    int floats_per_vert = 8;
    size_t total_floats = (size_t)mesh->count * verts_per_glyph * floats_per_vert;
    float *verts = malloc(total_floats * sizeof(float));
    if (!verts) return;

    for (int i = 0; i < mesh->count; i++) {
        GlyphInstance *g = &mesh->instances[i];
        float x0 = g->x, y0 = g->y, z = g->z;
        float x1 = x0 + g->w, y1 = y0 + g->h;
        float s0 = g->s0, t0 = g->t0, s1 = g->s1, t1 = g->t1;
        float r = g->r, gr = g->g, b = g->b;

        float *v = verts + i * verts_per_glyph * floats_per_vert;
        v[0]=x0; v[1]=y1; v[2]=z; v[3]=s0; v[4]=t0; v[5]=r; v[6]=gr; v[7]=b;
        v[8]=x0; v[9]=y0; v[10]=z; v[11]=s0; v[12]=t1; v[13]=r; v[14]=gr; v[15]=b;
        v[16]=x1; v[17]=y0; v[18]=z; v[19]=s1; v[20]=t1; v[21]=r; v[22]=gr; v[23]=b;
        v[24]=x0; v[25]=y1; v[26]=z; v[27]=s0; v[28]=t0; v[29]=r; v[30]=gr; v[31]=b;
        v[32]=x1; v[33]=y0; v[34]=z; v[35]=s1; v[36]=t1; v[37]=r; v[38]=gr; v[39]=b;
        v[40]=x1; v[41]=y1; v[42]=z; v[43]=s1; v[44]=t0; v[45]=r; v[46]=gr; v[47]=b;
    }

    glGenVertexArrays(1, &mesh->vao);
    glBindVertexArray(mesh->vao);

    glGenBuffers(1, &mesh->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
    glBufferData(GL_ARRAY_BUFFER, total_floats * sizeof(float), verts, GL_DYNAMIC_DRAW);

    /* pos: location 0 */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, floats_per_vert * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    /* uv: location 1 */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, floats_per_vert * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    /* color: location 2 */
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, floats_per_vert * sizeof(float), (void *)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);

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
    int hit = text_glyph_at(mesh, x, y);
    if (hit < 0) return 0;

    int hit_offset = mesh->instances[hit].text_offset;

    int seg_idx = -1;
    for (int i = 0; i < pt->segs.count; i++) {
        Segment *s = &pt->segs.segs[i];
        if ((s->kind == SEG_WORD || s->kind == SEG_CJK) &&
            hit_offset >= s->byte_start &&
            hit_offset < s->byte_start + s->byte_len) {
            seg_idx = i;
            break;
        }
    }
    if (seg_idx < 0) return 0;

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
