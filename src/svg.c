/* svg.c — SVG loader using NanoSVG for the shape layer plus a dedicated
 * <text> extractor for the text layer.
 *
 * NanoSVG ignores <text> elements entirely. For utterance that's a feature,
 * not a bug: we rasterize shapes only, then route every <text> through the
 * existing SDF glyph pipeline so labels stay vector-crisp at any zoom.
 *
 * The text extractor is deliberately small — it scans for <text ...> ... </text>
 * ranges and pulls out only the attributes we need (x, y, font-size, fill,
 * text-anchor, transform) plus any wrapping <a> hyperlink. Graphviz output is
 * regular and well-formed; full XML parsing is overkill.
 *
 * Transform handling is scoped to a single root <g transform="..."> wrapper
 * (which is how graphviz structures its output). Arbitrary nested transform
 * chains are not supported in this revision — documented limitation. */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE  /* memmem */

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#include "svg.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strncasecmp */

/* ---------------------------------------------------------------- detection */

int svg_detect(const unsigned char *buf, int len) {
    if (!buf || len <= 0) return 0;
    /* Skip leading whitespace / BOM. */
    int i = 0;
    if (len >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) i = 3;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) i++;
    if (i >= len) return 0;
    /* Sniff the first ~256 bytes for "<svg". */
    int end = i + 256 < len ? i + 256 : len;
    for (int j = i; j + 3 < end; j++) {
        if (buf[j] == '<' && (buf[j+1] == 's' || buf[j+1] == 'S') &&
            (buf[j+2] == 'v' || buf[j+2] == 'V') &&
            (buf[j+3] == 'g' || buf[j+3] == 'G')) {
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- storage */

void svg_text_list_destroy(SvgTextList *tl) {
    if (!tl) return;
    for (int i = 0; i < tl->count; i++) {
        free(tl->items[i].utf8);
        free(tl->items[i].href);
    }
    free(tl->items);
    memset(tl, 0, sizeof(*tl));
}

static void text_list_append(SvgTextList *tl, SvgText t) {
    if (tl->count >= tl->capacity) {
        int nc = tl->capacity ? tl->capacity * 2 : 16;
        SvgText *tmp = realloc(tl->items, (size_t)nc * sizeof(SvgText));
        if (!tmp) return;
        tl->items = tmp;
        tl->capacity = nc;
    }
    tl->items[tl->count++] = t;
}

/* ---------------------------------------------------------------- 2×3 affine */

typedef struct { float a, b, c, d, e, f; } Mat23;
/* Stores the column-major affine transform as on SVG's `matrix(a b c d e f)`:
 *   | a  c  e |
 *   | b  d  f |
 *   | 0  0  1 | */

static Mat23 mat_identity(void) { Mat23 m = {1, 0, 0, 1, 0, 0}; return m; }

static Mat23 mat_mul(Mat23 A, Mat23 B) {
    Mat23 r;
    r.a = A.a * B.a + A.c * B.b;
    r.b = A.b * B.a + A.d * B.b;
    r.c = A.a * B.c + A.c * B.d;
    r.d = A.b * B.c + A.d * B.d;
    r.e = A.a * B.e + A.c * B.f + A.e;
    r.f = A.b * B.e + A.d * B.f + A.f;
    return r;
}

static void mat_apply(Mat23 m, float x, float y, float *ox, float *oy) {
    *ox = m.a * x + m.c * y + m.e;
    *oy = m.b * x + m.d * y + m.f;
}

/* ---------------------------------------------------------------- lexing */

static const char *skip_ws(const char *s, const char *end) {
    while (s < end && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n' || *s == ',')) s++;
    return s;
}

/* Parse a float from s..end, advancing *sp. Returns 1 on success. */
static int lex_float(const char **sp, const char *end, float *out) {
    const char *s = skip_ws(*sp, end);
    if (s >= end) return 0;
    char buf[64];
    int n = 0;
    while (s < end && n < (int)sizeof(buf) - 1) {
        char c = *s;
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E') {
            buf[n++] = c;
            s++;
        } else break;
    }
    if (n == 0) return 0;
    buf[n] = '\0';
    *out = (float)strtod(buf, NULL);
    *sp = s;
    return 1;
}

/* ---------------------------------------------------------------- transform parser */

/* Parse one SVG transform() function starting at *sp into m_out, advancing
 * past the closing paren. Returns 1 on success. */
static int parse_one_transform(const char **sp, const char *end, Mat23 *m_out) {
    const char *s = skip_ws(*sp, end);
    if (s >= end) return 0;
    /* Read the function name. */
    const char *name = s;
    while (s < end && ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z'))) s++;
    int name_len = (int)(s - name);
    if (name_len == 0) return 0;

    s = skip_ws(s, end);
    if (s >= end || *s != '(') return 0;
    s++;

    float args[6] = {0};
    int n_args = 0;
    while (n_args < 6) {
        const char *before = s;
        float v;
        if (!lex_float(&s, end, &v)) break;
        args[n_args++] = v;
        if (s == before) break;
    }
    s = skip_ws(s, end);
    if (s >= end || *s != ')') return 0;
    s++;

    Mat23 m = mat_identity();
    if (name_len == 6 && strncmp(name, "matrix", 6) == 0 && n_args == 6) {
        m.a = args[0]; m.b = args[1]; m.c = args[2];
        m.d = args[3]; m.e = args[4]; m.f = args[5];
    } else if (name_len == 9 && strncmp(name, "translate", 9) == 0) {
        m.e = args[0];
        m.f = n_args >= 2 ? args[1] : 0.0f;
    } else if (name_len == 5 && strncmp(name, "scale", 5) == 0) {
        m.a = args[0];
        m.d = n_args >= 2 ? args[1] : args[0];
    } else if (name_len == 6 && strncmp(name, "rotate", 6) == 0) {
        float rad = args[0] * (float)M_PI / 180.0f;
        float cs = cosf(rad), sn = sinf(rad);
        if (n_args >= 3) {
            Mat23 t1 = mat_identity(); t1.e =  args[1]; t1.f =  args[2];
            Mat23 r  = mat_identity(); r.a = cs; r.b = sn; r.c = -sn; r.d = cs;
            Mat23 t2 = mat_identity(); t2.e = -args[1]; t2.f = -args[2];
            m = mat_mul(mat_mul(t1, r), t2);
        } else {
            m.a = cs; m.b = sn; m.c = -sn; m.d = cs;
        }
    } else if (name_len == 4 && strncmp(name, "skew", 4) == 0) {
        /* skewX / skewY — rare in our targets; skip silently. */
        (void)m_out;
    }
    *m_out = m;
    *sp = s;
    return 1;
}

static Mat23 parse_transform_chain(const char *s, int len) {
    Mat23 acc = mat_identity();
    const char *end = s + len;
    while (s < end) {
        s = skip_ws(s, end);
        if (s >= end) break;
        Mat23 m;
        if (!parse_one_transform(&s, end, &m)) break;
        acc = mat_mul(acc, m);
    }
    return acc;
}

/* ---------------------------------------------------------------- attribute lookup */

/* Within [tag_start, tag_end), find attribute `name` and return its value
 * span in [*val_start, *val_end). Returns 1 if found, 0 otherwise. */
static int find_attr(const char *tag_start, const char *tag_end, const char *name,
                     const char **val_start, const char **val_end) {
    size_t nlen = strlen(name);
    const char *p = tag_start;
    while (p < tag_end) {
        /* scan for name preceded by whitespace */
        const char *hit = memmem(p, (size_t)(tag_end - p), name, nlen);
        if (!hit) return 0;
        if (hit == tag_start || hit[-1] == ' ' || hit[-1] == '\t' ||
            hit[-1] == '\r' || hit[-1] == '\n') {
            const char *q = hit + nlen;
            while (q < tag_end && (*q == ' ' || *q == '\t')) q++;
            if (q < tag_end && *q == '=') {
                q++;
                while (q < tag_end && (*q == ' ' || *q == '\t')) q++;
                if (q < tag_end && (*q == '"' || *q == '\'')) {
                    char quote = *q++;
                    const char *e = q;
                    while (e < tag_end && *e != quote) e++;
                    *val_start = q;
                    *val_end = e;
                    return 1;
                }
            }
        }
        p = hit + 1;
    }
    return 0;
}

static int attr_float(const char *tag_start, const char *tag_end, const char *name, float *out) {
    const char *vs, *ve;
    if (!find_attr(tag_start, tag_end, name, &vs, &ve)) return 0;
    char buf[64];
    int n = (int)(ve - vs);
    if (n <= 0 || n >= (int)sizeof(buf)) return 0;
    memcpy(buf, vs, (size_t)n);
    buf[n] = '\0';
    *out = (float)strtod(buf, NULL);
    return 1;
}

/* ---------------------------------------------------------------- color parse */

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

/* Parse `fill` attribute value into r,g,b,a in 0..1. Returns 1 on success.
 * Supports #rgb, #rrggbb, rgb(r,g,b), a few named colors, and `none`. */
static int parse_color(const char *s, const char *e, float *r, float *g, float *b, float *a) {
    while (s < e && (*s == ' ' || *s == '\t')) s++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    int n = (int)(e - s);
    if (n <= 0) return 0;
    *a = 1.0f;
    if (n >= 4 && strncasecmp(s, "none", 4) == 0) {
        *r = *g = *b = *a = 0.0f; return 1;
    }
    if (*s == '#') {
        const char *h = s + 1;
        int hl = n - 1;
        if (hl == 3) {
            int R = hex_nibble(h[0]), G = hex_nibble(h[1]), B = hex_nibble(h[2]);
            if (R < 0 || G < 0 || B < 0) return 0;
            *r = (R * 17) / 255.0f; *g = (G * 17) / 255.0f; *b = (B * 17) / 255.0f;
            return 1;
        }
        if (hl == 6) {
            int v[6];
            for (int i = 0; i < 6; i++) { v[i] = hex_nibble(h[i]); if (v[i] < 0) return 0; }
            *r = ((v[0] << 4) | v[1]) / 255.0f;
            *g = ((v[2] << 4) | v[3]) / 255.0f;
            *b = ((v[4] << 4) | v[5]) / 255.0f;
            return 1;
        }
        return 0;
    }
    if (n >= 4 && strncasecmp(s, "rgb(", 4) == 0) {
        const char *p = s + 4;
        const char *endp = e;
        float c[3] = {0};
        for (int i = 0; i < 3; i++) {
            if (!lex_float(&p, endp, &c[i])) return 0;
        }
        *r = c[0] / 255.0f; *g = c[1] / 255.0f; *b = c[2] / 255.0f;
        return 1;
    }
    /* A handful of named colors common in graphviz / hand-written SVGs. */
    struct { const char *name; float r, g, b; } named[] = {
        {"black",   0, 0, 0},
        {"white",   1, 1, 1},
        {"red",     1, 0, 0},
        {"green",   0, 0.5f, 0},
        {"blue",    0, 0, 1},
        {"gray",    0.5f, 0.5f, 0.5f},
        {"grey",    0.5f, 0.5f, 0.5f},
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        size_t nl = strlen(named[i].name);
        if ((int)nl == n && strncasecmp(s, named[i].name, nl) == 0) {
            *r = named[i].r; *g = named[i].g; *b = named[i].b;
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- xml entity decode */

/* Decode XML entities in [s, e) into a newly allocated NUL-terminated string.
 * Handles &amp; &lt; &gt; &quot; &apos; and &#NN; / &#xHH;. Anything unknown
 * is copied verbatim. */
static char *xml_decode(const char *s, int len) {
    char *out = malloc((size_t)len + 1);
    if (!out) return NULL;
    int oi = 0;
    int i = 0;
    while (i < len) {
        if (s[i] == '&') {
            int j = i + 1;
            int max = i + 10 < len ? i + 10 : len;
            while (j < max && s[j] != ';') j++;
            if (j < len && s[j] == ';') {
                int el = j - i - 1;
                const char *ent = s + i + 1;
                unsigned cp = 0;
                int matched = 1;
                if (el == 3 && memcmp(ent, "amp", 3) == 0) cp = '&';
                else if (el == 2 && memcmp(ent, "lt", 2) == 0) cp = '<';
                else if (el == 2 && memcmp(ent, "gt", 2) == 0) cp = '>';
                else if (el == 4 && memcmp(ent, "quot", 4) == 0) cp = '"';
                else if (el == 4 && memcmp(ent, "apos", 4) == 0) cp = '\'';
                else if (el >= 2 && ent[0] == '#') {
                    if (ent[1] == 'x' || ent[1] == 'X') {
                        for (int k = 2; k < el; k++) {
                            int n = hex_nibble(ent[k]);
                            if (n < 0) { matched = 0; break; }
                            cp = cp * 16 + (unsigned)n;
                        }
                    } else {
                        for (int k = 1; k < el; k++) {
                            if (ent[k] < '0' || ent[k] > '9') { matched = 0; break; }
                            cp = cp * 10 + (unsigned)(ent[k] - '0');
                        }
                    }
                } else matched = 0;

                if (matched) {
                    /* Encode cp as UTF-8. */
                    if (cp < 0x80) {
                        out[oi++] = (char)cp;
                    } else if (cp < 0x800) {
                        out[oi++] = (char)(0xC0 | (cp >> 6));
                        out[oi++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        out[oi++] = (char)(0xE0 | (cp >> 12));
                        out[oi++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[oi++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        out[oi++] = (char)(0xF0 | (cp >> 18));
                        out[oi++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        out[oi++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[oi++] = (char)(0x80 | (cp & 0x3F));
                    }
                    i = j + 1;
                    continue;
                }
            }
        }
        out[oi++] = s[i++];
    }
    out[oi] = '\0';
    return out;
}

/* ---------------------------------------------------------------- file slurp */

static char *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

/* ---------------------------------------------------------------- text extraction */

/* Find the open-tag span for the first `<g` whose attribute list contains
 * class="graph" — graphviz's root group — or fall back to the first `<g`.
 * Returns the transform as an affine matrix (identity if none). */
static Mat23 extract_root_transform(const char *buf, size_t len) {
    Mat23 identity = mat_identity();
    const char *end = buf + len;
    const char *p = buf;
    const char *best_start = NULL, *best_end = NULL;
    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) break;
        if (lt + 2 < end && lt[1] == 'g' && (lt[2] == ' ' || lt[2] == '\t' || lt[2] == '\n' || lt[2] == '>')) {
            const char *gt = memchr(lt, '>', (size_t)(end - lt));
            if (!gt) break;
            if (!best_start) { best_start = lt; best_end = gt; }
            /* Prefer one with class="graph". */
            const char *vs, *ve;
            if (find_attr(lt, gt, "class", &vs, &ve)) {
                int n = (int)(ve - vs);
                if (n == 5 && strncmp(vs, "graph", 5) == 0) {
                    best_start = lt; best_end = gt;
                    break;
                }
            }
            p = gt + 1;
            continue;
        }
        p = lt + 1;
    }
    if (!best_start) return identity;
    const char *vs, *ve;
    if (!find_attr(best_start, best_end, "transform", &vs, &ve)) return identity;
    return parse_transform_chain(vs, (int)(ve - vs));
}

/* Extract every <text>…</text> element into tl. */
static void extract_text_elements(const char *buf, size_t len, Mat23 root_xf, SvgTextList *tl) {
    const char *end = buf + len;
    const char *p = buf;

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) break;
        /* Match only an opening <text tag (not <textPath, not </text>). */
        if (lt + 5 >= end || memcmp(lt, "<text", 5) != 0 ||
            (lt[5] != ' ' && lt[5] != '\t' && lt[5] != '\n' && lt[5] != '>')) {
            p = lt + 1;
            continue;
        }
        const char *tag_end = memchr(lt, '>', (size_t)(end - lt));
        if (!tag_end) break;
        /* Self-closing <text .../> has no inner content; rare but skip gracefully. */
        int self_close = (tag_end > lt && tag_end[-1] == '/');
        const char *content_start = tag_end + 1;
        const char *content_end = content_start;
        if (!self_close) {
            /* Find matching </text>. */
            const char *close = memmem(content_start, (size_t)(end - content_start), "</text>", 7);
            if (!close) { p = tag_end + 1; continue; }
            content_end = close;
            p = close + 7;
        } else {
            p = tag_end + 1;
        }

        /* Defaults. */
        SvgText t = {0};
        t.r = 0; t.g = 0; t.b = 0; t.a = 1;  /* black */
        t.font_size_px = 12.0f;
        t.anchor = 0;

        float x = 0, y = 0;
        attr_float(lt, tag_end, "x", &x);
        attr_float(lt, tag_end, "y", &y);

        /* Apply any local transform on the element, then the root transform. */
        const char *vs, *ve;
        Mat23 local = mat_identity();
        if (find_attr(lt, tag_end, "transform", &vs, &ve))
            local = parse_transform_chain(vs, (int)(ve - vs));
        Mat23 combined = mat_mul(root_xf, local);
        mat_apply(combined, x, y, &t.x, &t.y);

        float fs = 0;
        if (attr_float(lt, tag_end, "font-size", &fs) && fs > 0) t.font_size_px = fs;

        if (find_attr(lt, tag_end, "fill", &vs, &ve)) {
            float cr, cg, cb, ca;
            if (parse_color(vs, ve, &cr, &cg, &cb, &ca)) {
                t.r = cr; t.g = cg; t.b = cb; t.a = ca;
            }
        }

        if (find_attr(lt, tag_end, "text-anchor", &vs, &ve)) {
            int n = (int)(ve - vs);
            if (n == 6 && strncmp(vs, "middle", 6) == 0) t.anchor = 1;
            else if (n == 3 && strncmp(vs, "end", 3) == 0) t.anchor = 2;
        }

        int clen = (int)(content_end - content_start);
        if (clen < 0) clen = 0;
        t.utf8 = xml_decode(content_start, clen);
        if (!t.utf8) continue;
        if (t.utf8[0] == '\0') { free(t.utf8); continue; }

        text_list_append(tl, t);
    }
}

/* ---------------------------------------------------------------- loader */

int svg_load(const char *path, int target_px_w,
             GLuint *out_tex, int *out_tex_w, int *out_tex_h,
             SvgTextList *out_texts,
             float *out_svg_w, float *out_svg_h) {
    if (!path || !out_tex) return -1;

    /* nsvgParseFromFile resolves units against a DPI. 96 matches CSS pixel
     * convention and what graphviz and most SVG authors assume. */
    NSVGimage *img = nsvgParseFromFile(path, "px", 96.0f);
    if (!img) {
        fprintf(stderr, "utterance: svg parse failed: %s\n", path);
        return -1;
    }
    if (img->width <= 0 || img->height <= 0) {
        fprintf(stderr, "utterance: svg has no dimensions: %s\n", path);
        nsvgDelete(img);
        return -1;
    }

    /* Rasterize shape layer at target resolution. */
    if (target_px_w <= 0) target_px_w = 2048;
    float scale = target_px_w / img->width;
    int W = target_px_w;
    int H = (int)(img->height * scale + 0.5f);
    if (H <= 0) H = 1;

    unsigned char *pixels = malloc((size_t)W * H * 4);
    if (!pixels) {
        nsvgDelete(img);
        return -1;
    }
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) {
        free(pixels);
        nsvgDelete(img);
        return -1;
    }
    nsvgRasterize(rast, img, 0, 0, scale, pixels, W, H, W * 4);
    nsvgDeleteRasterizer(rast);

    /* Flip Y — OpenGL expects bottom-up textures (matches image.c convention). */
    unsigned char *flipped = malloc((size_t)W * H * 4);
    if (flipped) {
        for (int row = 0; row < H; row++)
            memcpy(flipped + row * W * 4, pixels + (H - 1 - row) * W * 4, (size_t)W * 4);
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 flipped ? flipped : pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    free(flipped);
    free(pixels);

    *out_tex = tex;
    if (out_tex_w) *out_tex_w = W;
    if (out_tex_h) *out_tex_h = H;
    if (out_svg_w) *out_svg_w = img->width;
    if (out_svg_h) *out_svg_h = img->height;

    float svg_w_final = img->width;
    float svg_h_final = img->height;

    nsvgDelete(img);

    /* Parse <text> runs. Second pass through the file — small cost relative to
     * rasterization, and keeps text logic cleanly separated from nanosvg. */
    if (out_texts) {
        size_t file_len = 0;
        char *file_buf = slurp(path, &file_len);
        if (file_buf) {
            Mat23 root = extract_root_transform(file_buf, file_len);
            extract_text_elements(file_buf, file_len, root, out_texts);
            free(file_buf);
            fprintf(stderr, "utterance: parsed %d <text> elements (viewbox %.0fx%.0f)\n",
                    out_texts->count, svg_w_final, svg_h_final);
        }
    }

    return 0;
}
