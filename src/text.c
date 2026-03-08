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

void text_layout(TextMesh *mesh, Font *font, const uint8_t *text, size_t len, float wrap_width) {
    memset(mesh, 0, sizeof(*mesh));

    float line_height = font->ascent - font->descent + font->line_gap;
    float cursor_x = 0.0f;
    float cursor_y = 0.0f;
    float tab_width = font->glyphs[' '].present ? font->glyphs[' '].advance * 4.0f : font->px_size;

    const uint8_t *p = text;
    const uint8_t *end = text + len;

    while (p < end) {
        const uint8_t *cp_start = p;
        uint32_t cp = utf8_decode(&p, end);
        if (cp == 0) break;

        if (cp == '\n') {
            cursor_x = 0.0f;
            cursor_y -= line_height;
            continue;
        }
        if (cp == '\r') continue;
        if (cp == '\t') {
            cursor_x += tab_width;
            continue;
        }

        Glyph *g = font_glyph(font, cp);
        if (!g) continue;

        /* Wrap */
        if (wrap_width > 0 && cursor_x + g->advance > wrap_width) {
            cursor_x = 0.0f;
            cursor_y -= line_height;
        }

        /* Only emit a quad if glyph has a visible bitmap */
        if (g->width > 0 && g->height > 0) {
            GlyphInstance inst;
            inst.x = cursor_x + g->x_off;
            inst.y = cursor_y - g->y_off - g->height; /* flip Y: OpenGL up, font down */
            inst.z = 0.0f;
            inst.w = g->width;
            inst.h = g->height;
            inst.s0 = g->s0;
            inst.t0 = g->t0;
            inst.s1 = g->s1;
            inst.t1 = g->t1;
            inst.text_offset = (int)(cp_start - text);
            if (push_instance(mesh, &inst) < 0) break; /* OOM */
        }
        cursor_x += g->advance;
    }
}

void text_upload(TextMesh *mesh) {
    if (mesh->count == 0) return;

    /* 6 vertices per quad (2 triangles), 5 floats each (x,y,z,u,v) */
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
        /* Triangle 1 */
        v[0]=x0; v[1]=y1; v[2]=z; v[3]=s0; v[4]=t0;    /* top-left */
        v[5]=x0; v[6]=y0; v[7]=z; v[8]=s0; v[9]=t1;    /* bottom-left */
        v[10]=x1; v[11]=y0; v[12]=z; v[13]=s1; v[14]=t1; /* bottom-right */
        /* Triangle 2 */
        v[15]=x0; v[16]=y1; v[17]=z; v[18]=s0; v[19]=t0;  /* top-left */
        v[20]=x1; v[21]=y0; v[22]=z; v[23]=s1; v[24]=t1;  /* bottom-right */
        v[25]=x1; v[26]=y1; v[27]=z; v[28]=s1; v[29]=t0;  /* top-right */
    }

    glGenVertexArrays(1, &mesh->vao);
    glBindVertexArray(mesh->vao);

    glGenBuffers(1, &mesh->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
    glBufferData(GL_ARRAY_BUFFER, total_floats * sizeof(float), verts, GL_STATIC_DRAW);

    /* pos: location 0 */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, floats_per_vert * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    /* uv: location 1 */
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

int text_word_bounds(const TextMesh *mesh, float x, float y, float out[4]) {
    /* Find closest glyph to (x,y) */
    float best = 1e30f;
    int hit = -1;
    for (int i = 0; i < mesh->count; i++) {
        GlyphInstance *g = &mesh->instances[i];
        float cx = g->x + g->w * 0.5f;
        float cy = g->y + g->h * 0.5f;
        float d = (x - cx)*(x - cx) + (y - cy)*(y - cy);
        if (d < best) { best = d; hit = i; }
    }
    if (hit < 0) return 0;

    GlyphInstance *h = &mesh->instances[hit];
    float line_y = h->y;
    float line_tol = h->h * 0.5f; /* same-line tolerance */

    /* Expand left: find contiguous glyphs on same line */
    int left = hit, right = hit;
    for (int i = hit - 1; i >= 0; i--) {
        GlyphInstance *g = &mesh->instances[i];
        if (fabsf(g->y - line_y) > line_tol) break; /* different line */
        float gap = mesh->instances[i+1].x - (g->x + g->w);
        if (gap > g->w * 0.8f) break; /* word boundary (space-sized gap) */
        left = i;
    }
    /* Expand right */
    for (int i = hit + 1; i < mesh->count; i++) {
        GlyphInstance *g = &mesh->instances[i];
        if (fabsf(g->y - line_y) > line_tol) break;
        float gap = g->x - (mesh->instances[i-1].x + mesh->instances[i-1].w);
        if (gap > g->w * 0.8f) break;
        right = i;
    }

    /* Compute bounds with small padding */
    out[0] = mesh->instances[left].x - 2.0f;
    out[1] = mesh->instances[left].y - 2.0f;
    out[2] = mesh->instances[right].x + mesh->instances[right].w + 2.0f;
    out[3] = mesh->instances[left].y + mesh->instances[left].h + 2.0f;
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
            /* New line — emit current rect */
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
    /* Emit last rect */
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
