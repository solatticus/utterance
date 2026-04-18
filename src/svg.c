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
#include "utf8.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
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

void svg_link_list_destroy(SvgLinkList *ll) {
    if (!ll) return;
    for (int i = 0; i < ll->count; i++) free(ll->items[i].href);
    free(ll->items);
    memset(ll, 0, sizeof(*ll));
}

static void link_list_append(SvgLinkList *ll, SvgLink l) {
    if (ll->count >= ll->capacity) {
        int nc = ll->capacity ? ll->capacity * 2 : 8;
        SvgLink *tmp = realloc(ll->items, (size_t)nc * sizeof(SvgLink));
        if (!tmp) { free(l.href); return; }
        ll->items = tmp;
        ll->capacity = nc;
    }
    ll->items[ll->count++] = l;
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

/* ---------------------------------------------------------------- primitive storage */

/* Primitive records — one per SVG primitive element (rect/circle/ellipse/line/
 * path/polygon/polyline) in document order. Emitted by a parallel XML walker
 * alongside the text extractor. The nanosvg shape iterator correlates against
 * this list by doc-order index so rects can be swapped for cuboid meshes
 * (and later: cylinders/spheres for circle/ellipse, oriented boxes for line).
 *
 * Record kept internal to svg.c — not part of the public API. */
typedef struct {
    SvgElemKind kind;
    char       *id;         /* owned, or NULL */
    char       *href;       /* owned, or NULL — enclosing <a xlink:href> */
    Mat23       xform;      /* root × any enclosing <g> × this element's transform */
    /* Kind-specific geometry in SVG user-space, pre-transform:
     *   RECT:    x, y, w, h, rx, ry
     *   CIRCLE:  cx, cy, r, 0, 0, 0
     *   ELLIPSE: cx, cy, rx, ry, 0, 0
     *   LINE:    x1, y1, x2, y2, 0, 0
     *   PATH/POLYGON/POLYLINE: unused — record acts as a doc-order spacer */
    float       params[6];
} SvgPrimitive;

typedef struct {
    SvgPrimitive *items;
    int           count;
    int           capacity;
} SvgPrimitiveList;

static void prim_list_destroy(SvgPrimitiveList *pl) {
    if (!pl) return;
    for (int i = 0; i < pl->count; i++) {
        free(pl->items[i].id);
        free(pl->items[i].href);
    }
    free(pl->items);
    memset(pl, 0, sizeof(*pl));
}

static void prim_append(SvgPrimitiveList *pl, SvgPrimitive p) {
    if (pl->count >= pl->capacity) {
        int nc = pl->capacity ? pl->capacity * 2 : 64;
        SvgPrimitive *tmp = realloc(pl->items, (size_t)nc * sizeof(SvgPrimitive));
        if (!tmp) { free(p.id); free(p.href); return; }
        pl->items = tmp;
        pl->capacity = nc;
    }
    pl->items[pl->count++] = p;
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

/* Shallow stack of open <a> hrefs shared between the text and primitive
 * walkers. SVGs rarely nest anchors more than once; 8 is plenty. */
#define HREF_STACK_MAX 8

/* Extract every <text>…</text> element into tl. Also tracks enclosing
 * <a xlink:href="..."> wrappers so each text run picks up its link target. */
static void extract_text_elements(const char *buf, size_t len, Mat23 root_xf, SvgTextList *tl) {
    const char *end = buf + len;
    const char *p = buf;

    char *href_stack[HREF_STACK_MAX] = {0};
    int   href_depth = 0;

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) break;

        /* <a ...>  — open anchor, capture href */
        if (lt + 2 < end && lt[1] == 'a' &&
            (lt[2] == ' ' || lt[2] == '\t' || lt[2] == '\n' || lt[2] == '>')) {
            const char *tag_end = memchr(lt, '>', (size_t)(end - lt));
            if (!tag_end) break;
            int self_close = (tag_end > lt && tag_end[-1] == '/');
            const char *vs, *ve;
            char *href = NULL;
            /* SVG 1.1 uses xlink:href; SVG 2 allows bare href. Check both. */
            if (find_attr(lt, tag_end, "xlink:href", &vs, &ve) ||
                find_attr(lt, tag_end, "href", &vs, &ve)) {
                int n = (int)(ve - vs);
                if (n > 0) {
                    href = xml_decode(vs, n);
                }
            }
            if (!self_close && href_depth < HREF_STACK_MAX) {
                href_stack[href_depth++] = href;
            } else {
                free(href);
            }
            p = tag_end + 1;
            continue;
        }

        /* </a> — pop anchor */
        if (lt + 3 < end && lt[1] == '/' && lt[2] == 'a' &&
            (lt[3] == '>' || lt[3] == ' ' || lt[3] == '\t' || lt[3] == '\n')) {
            const char *tag_end = memchr(lt, '>', (size_t)(end - lt));
            if (!tag_end) break;
            if (href_depth > 0) {
                href_depth--;
                free(href_stack[href_depth]);
                href_stack[href_depth] = NULL;
            }
            p = tag_end + 1;
            continue;
        }

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

        /* Extract rotation angle from the combined affine. Assumes no shear
         * or non-uniform scale in the chain — true for graphviz and most
         * hand-authored SVGs. Angle is in the SVG (y-down) frame; the mesh
         * builder flips sign when mapping into y-up world. */
        float theta = atan2f(combined.b, combined.a);
        if (fabsf(theta) > 1e-5f)
            t.rotate_deg = theta * 180.0f / (float)M_PI;

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

        /* Inherit href from any enclosing <a>. */
        if (href_depth > 0 && href_stack[href_depth - 1])
            t.href = strdup(href_stack[href_depth - 1]);

        text_list_append(tl, t);
    }

    /* Drain any anchor frames left open by malformed input. */
    while (href_depth > 0) {
        href_depth--;
        free(href_stack[href_depth]);
    }
}

/* ---------------------------------------------------------------- primitive extraction */

/* Tag dispatch table — name length is precomputed so we can match without
 * strcmp's NUL-terminator assumption on our tag-start pointer. */
static const struct { const char *name; int nlen; SvgElemKind kind; } kPrimTags[] = {
    {"rect",     4, SVG_ELEM_RECT},
    {"circle",   6, SVG_ELEM_CIRCLE},
    {"ellipse",  7, SVG_ELEM_ELLIPSE},
    {"line",     4, SVG_ELEM_LINE},
    {"path",     4, SVG_ELEM_PATH},
    {"polygon",  7, SVG_ELEM_POLYGON},
    {"polyline", 8, SVG_ELEM_POLYLINE},
};

static int match_primitive_tag(const char *lt, const char *end, int *out_idx) {
    for (size_t i = 0; i < sizeof(kPrimTags) / sizeof(kPrimTags[0]); i++) {
        int nl = kPrimTags[i].nlen;
        if (lt + 1 + nl >= end) continue;
        if (lt[0] != '<') continue;
        if (memcmp(lt + 1, kPrimTags[i].name, (size_t)nl) != 0) continue;
        char after = lt[1 + nl];
        if (after != ' ' && after != '\t' && after != '\n' &&
            after != '\r' && after != '>' && after != '/') continue;
        *out_idx = (int)i;
        return 1;
    }
    return 0;
}

/* Walks the SVG buffer emitting one SvgPrimitive record per visible primitive
 * element in document order. Tracks <a> for href capture and <defs> for
 * suppression — nanosvg skips primitives inside <defs>, and we must match
 * that filter exactly so the primitive list aligns 1:1 with the nanosvg shape
 * list in source order. */
static void extract_primitive_elements(const char *buf, size_t len, Mat23 root_xf,
                                       SvgPrimitiveList *pl) {
    const char *end = buf + len;
    const char *p = buf;

    char *href_stack[HREF_STACK_MAX] = {0};
    int   href_depth = 0;
    int   defs_depth = 0;

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) break;

        /* <defs> open */
        if (lt + 5 < end && memcmp(lt, "<defs", 5) == 0 &&
            (lt[5] == ' ' || lt[5] == '\t' || lt[5] == '\n' || lt[5] == '>' || lt[5] == '/')) {
            const char *tag_end = memchr(lt, '>', (size_t)(end - lt));
            if (!tag_end) break;
            int self_close = (tag_end > lt && tag_end[-1] == '/');
            if (!self_close) defs_depth++;
            p = tag_end + 1;
            continue;
        }
        /* </defs> */
        if (lt + 6 < end && memcmp(lt, "</defs", 6) == 0) {
            const char *tag_end = memchr(lt, '>', (size_t)(end - lt));
            if (!tag_end) break;
            if (defs_depth > 0) defs_depth--;
            p = tag_end + 1;
            continue;
        }

        /* <a> — push anchor href */
        if (lt + 2 < end && lt[1] == 'a' &&
            (lt[2] == ' ' || lt[2] == '\t' || lt[2] == '\n' || lt[2] == '>')) {
            const char *tag_end = memchr(lt, '>', (size_t)(end - lt));
            if (!tag_end) break;
            int self_close = (tag_end > lt && tag_end[-1] == '/');
            const char *vs, *ve;
            char *href = NULL;
            if (find_attr(lt, tag_end, "xlink:href", &vs, &ve) ||
                find_attr(lt, tag_end, "href", &vs, &ve)) {
                int n = (int)(ve - vs);
                if (n > 0) href = xml_decode(vs, n);
            }
            if (!self_close && href_depth < HREF_STACK_MAX) {
                href_stack[href_depth++] = href;
            } else {
                free(href);
            }
            p = tag_end + 1;
            continue;
        }
        /* </a> — pop anchor */
        if (lt + 3 < end && lt[1] == '/' && lt[2] == 'a' &&
            (lt[3] == '>' || lt[3] == ' ' || lt[3] == '\t' || lt[3] == '\n')) {
            const char *tag_end = memchr(lt, '>', (size_t)(end - lt));
            if (!tag_end) break;
            if (href_depth > 0) {
                href_depth--;
                free(href_stack[href_depth]);
                href_stack[href_depth] = NULL;
            }
            p = tag_end + 1;
            continue;
        }

        /* Primitive tag dispatch */
        int prim_idx = -1;
        if (defs_depth == 0 && match_primitive_tag(lt, end, &prim_idx)) {
            const char *tag_end = memchr(lt, '>', (size_t)(end - lt));
            if (!tag_end) break;

            SvgPrimitive prim = {0};
            prim.kind  = kPrimTags[prim_idx].kind;
            prim.xform = root_xf;

            const char *vs, *ve;
            if (find_attr(lt, tag_end, "id", &vs, &ve)) {
                int n = (int)(ve - vs);
                if (n > 0) prim.id = xml_decode(vs, n);
            }
            if (find_attr(lt, tag_end, "transform", &vs, &ve)) {
                Mat23 local = parse_transform_chain(vs, (int)(ve - vs));
                prim.xform = mat_mul(root_xf, local);
            }
            if (href_depth > 0 && href_stack[href_depth - 1])
                prim.href = strdup(href_stack[href_depth - 1]);

            float x = 0, y = 0, w = 0, h = 0, rx = 0, ry = 0;
            float cx = 0, cy = 0, r = 0;
            float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            switch (prim.kind) {
            case SVG_ELEM_RECT:
                attr_float(lt, tag_end, "x", &x);
                attr_float(lt, tag_end, "y", &y);
                attr_float(lt, tag_end, "width",  &w);
                attr_float(lt, tag_end, "height", &h);
                attr_float(lt, tag_end, "rx", &rx);
                attr_float(lt, tag_end, "ry", &ry);
                prim.params[0] = x;  prim.params[1] = y;
                prim.params[2] = w;  prim.params[3] = h;
                prim.params[4] = rx; prim.params[5] = ry;
                break;
            case SVG_ELEM_CIRCLE:
                attr_float(lt, tag_end, "cx", &cx);
                attr_float(lt, tag_end, "cy", &cy);
                attr_float(lt, tag_end, "r",  &r);
                prim.params[0] = cx; prim.params[1] = cy; prim.params[2] = r;
                break;
            case SVG_ELEM_ELLIPSE:
                attr_float(lt, tag_end, "cx", &cx);
                attr_float(lt, tag_end, "cy", &cy);
                attr_float(lt, tag_end, "rx", &rx);
                attr_float(lt, tag_end, "ry", &ry);
                prim.params[0] = cx; prim.params[1] = cy;
                prim.params[2] = rx; prim.params[3] = ry;
                break;
            case SVG_ELEM_LINE:
                attr_float(lt, tag_end, "x1", &x1);
                attr_float(lt, tag_end, "y1", &y1);
                attr_float(lt, tag_end, "x2", &x2);
                attr_float(lt, tag_end, "y2", &y2);
                prim.params[0] = x1; prim.params[1] = y1;
                prim.params[2] = x2; prim.params[3] = y2;
                break;
            default:
                /* PATH/POLYGON/POLYLINE — spacer record only. */
                break;
            }

            prim_append(pl, prim);

            /* Advance past the element. Primitives are usually self-closing;
             * if not we scan for the matching close tag. */
            int self_close = (tag_end > lt && tag_end[-1] == '/');
            if (self_close) {
                p = tag_end + 1;
            } else {
                char close_tag[16];
                int cn = snprintf(close_tag, sizeof close_tag, "</%s>",
                                  kPrimTags[prim_idx].name);
                const char *close = memmem(tag_end + 1,
                                           (size_t)(end - tag_end - 1),
                                           close_tag, (size_t)cn);
                p = close ? close + cn : tag_end + 1;
            }
            continue;
        }

        p = lt + 1;
    }

    while (href_depth > 0) {
        href_depth--;
        free(href_stack[href_depth]);
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
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
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

/* ---------------------------------------------------------------- scene build */

#include "geom.h"

/* nanosvg paint → SceneMaterial fill_rgba (we store fills and strokes as
 * separate materials; each node references one). opacity applies on top. */
static void unpack_color(unsigned int c, float opacity, float out[4]) {
    out[0] = ((c >> 0)  & 0xff) / 255.0f;
    out[1] = ((c >> 8)  & 0xff) / 255.0f;
    out[2] = ((c >> 16) & 0xff) / 255.0f;
    out[3] = ((c >> 24) & 0xff) / 255.0f * opacity;
}

/* Flatten a nanosvg NSVGpath into scratch as a polyline (y-flipped to world
 * conventions: world_y = -svg_y). Appends to *scratch without clearing — caller
 * resets *count before each path. */
static void flatten_nsvg_path(NSVGpath *path, float flatness,
                              GVec2 **scratch, uint32_t *count, uint32_t *cap) {
    if (path->npts < 1) return;
    GVec2 p0 = {path->pts[0], -path->pts[1]};
    /* Emit starting point. */
    if (*count >= *cap) {
        uint32_t nc = *cap ? *cap * 2 : 128;
        GVec2 *tmp = realloc(*scratch, (size_t)nc * sizeof(GVec2));
        if (!tmp) return;
        *scratch = tmp; *cap = nc;
    }
    (*scratch)[(*count)++] = p0;

    for (int i = 0; i < path->npts - 1; i += 3) {
        float *p = &path->pts[i * 2];
        GVec2 c0 = {p[0], -p[1]};
        GVec2 c1 = {p[2], -p[3]};
        GVec2 c2 = {p[4], -p[5]};
        GVec2 c3 = {p[6], -p[7]};
        /* geom_flatten_cubic emits only interior subdivisions; we append c3
         * manually afterwards so the polyline includes the endpoint. */
        geom_flatten_cubic(c0, c1, c2, c3, flatness, scratch, count, cap);
        if (*count >= *cap) {
            uint32_t nc = *cap * 2;
            GVec2 *tmp = realloc(*scratch, (size_t)nc * sizeof(GVec2));
            if (!tmp) return;
            *scratch = tmp; *cap = nc;
        }
        (*scratch)[(*count)++] = c3;
    }
}

/* Grow a typed buffer to hold at least `need` elements. */
#define ENSURE_CAP(buf, cap_var, need, type)                            \
    do {                                                                \
        if ((cap_var) < (uint32_t)(need)) {                             \
            uint32_t _nc = (cap_var) ? (cap_var) : 64;                  \
            while (_nc < (uint32_t)(need)) _nc *= 2;                    \
            type *_tmp = realloc((buf), (size_t)_nc * sizeof(type));    \
            if (!_tmp) return -1;                                       \
            (buf) = _tmp;                                               \
            (cap_var) = _nc;                                            \
        }                                                               \
    } while (0)

int svg_build_scene(const char *path, float extrusion_depth,
                    Scene *out_scene,
                    SvgTextList *out_texts,
                    float *out_svg_w, float *out_svg_h) {
    if (!path || !out_scene) return -1;
    int extrude = extrusion_depth > 0.0f;

    /* First pass: slurp the file and parse primitives + root transform so the
     * nanosvg shape walk below can correlate each shape to its source element
     * by doc-order index (nanosvg preserves source order; our extractor skips
     * the same elements nanosvg skips — <defs>-nested primitives). */
    size_t file_len = 0;
    char *file_buf = slurp(path, &file_len);
    Mat23 root_xf = mat_identity();
    SvgPrimitiveList prims = {0};
    if (file_buf) {
        root_xf = extract_root_transform(file_buf, file_len);
        extract_primitive_elements(file_buf, file_len, root_xf, &prims);
    }

    NSVGimage *img = nsvgParseFromFile(path, "px", 96.0f);
    if (!img) {
        fprintf(stderr, "utterance: svg parse failed: %s\n", path);
        prim_list_destroy(&prims);
        free(file_buf);
        return -1;
    }
    if (img->width <= 0 || img->height <= 0) {
        fprintf(stderr, "utterance: svg has no dimensions: %s\n", path);
        nsvgDelete(img);
        prim_list_destroy(&prims);
        free(file_buf);
        return -1;
    }

    if (out_svg_w) *out_svg_w = img->width;
    if (out_svg_h) *out_svg_h = img->height;

    /* Flatness tolerance: scale-relative so curves look smooth at any zoom.
     * 0.05% of the larger dim is a good balance between vertex count and
     * visual fidelity; clamped to ≥0.25 so tiny SVGs don't over-subdivide. */
    float flatness = (img->width > img->height ? img->width : img->height) * 0.0005f;
    if (flatness < 0.25f) flatness = 0.25f;

    /* Scratch buffers, reused across every shape + subpath. Allocated on
     * first need, grown only to the high-water mark, freed once at exit. */
    GVec2       *poly       = NULL; uint32_t poly_cap       = 0;
    uint32_t    *tri_idx    = NULL; uint32_t tri_cap        = 0;
    GVec2       *ribbon     = NULL; uint32_t ribbon_cap     = 0;
    uint32_t    *strip_idx  = NULL; uint32_t strip_cap      = 0;
    SceneVertex *verts      = NULL; uint32_t verts_cap      = 0;
    GVec3       *ex_verts   = NULL; uint32_t ex_verts_cap   = 0;
    GVec3       *ex_normals = NULL; uint32_t ex_normals_cap = 0;
    uint32_t    *ex_idx     = NULL; uint32_t ex_idx_cap     = 0;

    int shape_count = 0, fill_nodes = 0, stroke_nodes = 0;
    int cuboid_swaps = 0, cylinder_swaps = 0, line_swaps = 0;
    int prim_idx = 0;

    for (NSVGshape *shape = img->shapes; shape; shape = shape->next) {
        /* Advance the primitive cursor in lockstep with nanosvg's shape list,
         * regardless of whether we end up drawing this shape. Keeps doc-order
         * alignment intact across skipped-invisible shapes. */
        SvgPrimitive *prim = (prim_idx < prims.count) ? &prims.items[prim_idx] : NULL;
        prim_idx++;

        if (!(shape->flags & NSVG_FLAGS_VISIBLE)) continue;
        shape_count++;

        float fill_rgba[4]   = {0}, stroke_rgba[4] = {0};
        int has_fill   = (shape->fill.type   == NSVG_PAINT_COLOR);
        int has_stroke = (shape->stroke.type == NSVG_PAINT_COLOR) && shape->strokeWidth > 0;
        if (has_fill)   unpack_color(shape->fill.color,   shape->opacity, fill_rgba);
        if (has_stroke) unpack_color(shape->stroke.color, shape->opacity, stroke_rgba);
        if (has_fill   && fill_rgba[3]   < 0.001f) has_fill   = 0;
        if (has_stroke && stroke_rgba[3] < 0.001f) has_stroke = 0;
        if (!has_fill && !has_stroke) continue;

        /* One material per shape for fill, one for stroke — cheap and
         * enables later per-material draw batching. */
        int fill_mat = -1, stroke_mat = -1;
        if (has_fill) {
            SceneMaterial m = {0};
            memcpy(m.fill_rgba, fill_rgba, sizeof fill_rgba);
            m.fill_rgba[3] = fill_rgba[3];
            fill_mat = scene_add_material(out_scene, &m);
        }
        if (has_stroke) {
            SceneMaterial m = {0};
            memcpy(m.fill_rgba, stroke_rgba, sizeof stroke_rgba);
            m.stroke_width = shape->strokeWidth;
            stroke_mat = scene_add_material(out_scene, &m);
        }

        /* Provenance that every emitted node for this shape inherits. */
        SvgElemKind node_elem = prim ? prim->kind : SVG_ELEM_PATH;
        char *node_href = prim ? prim->href : NULL;

        /* Rects without rounded corners become cuboids when extruded — one
         * fill-mesh emit per shape instead of per subpath. Detected once up
         * front; the fill loop below checks the flag and skips its work. */
        int cuboid_emit = (extrude && has_fill && prim &&
                           prim->kind == SVG_ELEM_RECT &&
                           prim->params[2] > 0 && prim->params[3] > 0 &&
                           prim->params[4] == 0.0f && prim->params[5] == 0.0f);
        if (cuboid_emit) {
            float rx = prim->params[0], ry = prim->params[1];
            float rw = prim->params[2], rh = prim->params[3];
            /* 4 corners in SVG y-down space → apply element transform → y-flip
             * to world y-up. SVG clockwise order (tl,tr,br,bl) becomes CCW in
             * world space after the flip, which is what geom_build_cuboid wants. */
            GVec2 w[4];
            float sx, sy;
            mat_apply(prim->xform, rx,      ry,      &sx, &sy); w[0].x = sx; w[0].y = -sy;
            mat_apply(prim->xform, rx + rw, ry,      &sx, &sy); w[1].x = sx; w[1].y = -sy;
            mat_apply(prim->xform, rx + rw, ry + rh, &sx, &sy); w[2].x = sx; w[2].y = -sy;
            mat_apply(prim->xform, rx,      ry + rh, &sx, &sy); w[3].x = sx; w[3].y = -sy;

            ENSURE_CAP(ex_verts,   ex_verts_cap,   24, GVec3);
            ENSURE_CAP(ex_normals, ex_normals_cap, 24, GVec3);
            ENSURE_CAP(ex_idx,     ex_idx_cap,     36, uint32_t);
            uint32_t tcount = geom_build_cuboid(w[0], w[1], w[2], w[3], extrusion_depth,
                                                ex_verts, ex_normals, ex_idx);
            if (tcount > 0) {
                ENSURE_CAP(verts, verts_cap, 24, SceneVertex);
                for (int k = 0; k < 24; k++) {
                    SceneVertex v = {0};
                    v.pos[0]    = ex_verts[k].x;
                    v.pos[1]    = ex_verts[k].y;
                    v.pos[2]    = ex_verts[k].z;
                    v.normal[0] = ex_normals[k].x;
                    v.normal[1] = ex_normals[k].y;
                    v.normal[2] = ex_normals[k].z;
                    v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
                    verts[k] = v;
                }
                int mid = scene_add_mesh(out_scene, verts, 24, ex_idx, tcount * 3);
                SceneNode nd = {0};
                scene_mat_identity(nd.local_xform);
                nd.parent      = -1;
                nd.mesh_id     = mid;
                nd.material_id = fill_mat;
                nd.flags       = SCENE_NODE_VISIBLE | SCENE_NODE_SELECTABLE;
                nd.svg_elem    = SVG_ELEM_RECT;
                nd.depth       = extrusion_depth;
                memcpy(nd.svg_params, prim->params, sizeof nd.svg_params);
                nd.href        = node_href;
                scene_add_node(out_scene, &nd);
                fill_nodes++;
                cuboid_swaps++;
            }
        }

        /* Circles and ellipses become authored cylinders when extruded with a
         * fill. Sample a 32-point ring in SVG user-space, push it through the
         * element transform, y-flip, hand off to geom_build_cylinder. Any
         * stroke on the shape still emits as a ribbon via the normal stroke
         * path below (sits at depth + 0.5 on top of the cap). */
        int cylinder_emit = (extrude && has_fill && prim &&
                             (prim->kind == SVG_ELEM_CIRCLE ||
                              prim->kind == SVG_ELEM_ELLIPSE));
        if (cylinder_emit) {
            float ccx = prim->params[0], ccy = prim->params[1];
            float crx, cry;
            if (prim->kind == SVG_ELEM_CIRCLE) {
                crx = cry = prim->params[2];
            } else {
                crx = prim->params[2]; cry = prim->params[3];
            }
            enum { CYL_N = 32 };
            GVec2 ring[CYL_N];
            for (int i = 0; i < CYL_N; i++) {
                float t = (float)i / (float)CYL_N * 2.0f * (float)M_PI;
                float sxp = ccx + crx * cosf(t);
                float syp = ccy + cry * sinf(t);
                float wxp, wyp;
                mat_apply(prim->xform, sxp, syp, &wxp, &wyp);
                ring[i].x =  wxp;
                ring[i].y = -wyp;
            }
            uint32_t vmax  = 2 + 4 * CYL_N;
            uint32_t ineed = 12 * CYL_N;
            ENSURE_CAP(ex_verts,   ex_verts_cap,   vmax,  GVec3);
            ENSURE_CAP(ex_normals, ex_normals_cap, vmax,  GVec3);
            ENSURE_CAP(ex_idx,     ex_idx_cap,     ineed, uint32_t);
            uint32_t tcount = geom_build_cylinder(ring, CYL_N, extrusion_depth,
                                                  ex_verts, ex_normals, ex_idx);
            if (tcount > 0) {
                ENSURE_CAP(verts, verts_cap, vmax, SceneVertex);
                for (uint32_t k = 0; k < vmax; k++) {
                    SceneVertex v = {0};
                    v.pos[0]    = ex_verts[k].x;
                    v.pos[1]    = ex_verts[k].y;
                    v.pos[2]    = ex_verts[k].z;
                    v.normal[0] = ex_normals[k].x;
                    v.normal[1] = ex_normals[k].y;
                    v.normal[2] = ex_normals[k].z;
                    v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
                    verts[k] = v;
                }
                int mid = scene_add_mesh(out_scene, verts, vmax, ex_idx, tcount * 3);
                SceneNode nd = {0};
                scene_mat_identity(nd.local_xform);
                nd.parent      = -1;
                nd.mesh_id     = mid;
                nd.material_id = fill_mat;
                nd.flags       = SCENE_NODE_VISIBLE | SCENE_NODE_SELECTABLE;
                nd.svg_elem    = prim->kind;
                nd.depth       = extrusion_depth;
                memcpy(nd.svg_params, prim->params, sizeof nd.svg_params);
                nd.href        = node_href;
                scene_add_node(out_scene, &nd);
                fill_nodes++;
                cylinder_swaps++;
            }
        }

        /* <line> with a stroke becomes an oriented cuboid when extruded. The
         * four bottom corners are the endpoints offset perpendicular by half
         * the stroke width (world units — nanosvg already baked any transform
         * scale into strokeWidth). Suppresses the default stroke ribbon for
         * this shape. */
        int line_cuboid_emit = (extrude && has_stroke && prim &&
                                prim->kind == SVG_ELEM_LINE);
        if (line_cuboid_emit) {
            float x1 = prim->params[0], y1 = prim->params[1];
            float x2 = prim->params[2], y2 = prim->params[3];
            float wx1, wy1, wx2, wy2;
            mat_apply(prim->xform, x1, y1, &wx1, &wy1);
            mat_apply(prim->xform, x2, y2, &wx2, &wy2);
            wy1 = -wy1; wy2 = -wy2;
            float dx = wx2 - wx1, dy = wy2 - wy1;
            float len = sqrtf(dx * dx + dy * dy);
            if (len > 1e-6f) {
                float hw = shape->strokeWidth * 0.5f;
                float px = -dy / len * hw;
                float py =  dx / len * hw;
                GVec2 w0 = {wx1 + px, wy1 + py};
                GVec2 w1 = {wx2 + px, wy2 + py};
                GVec2 w2 = {wx2 - px, wy2 - py};
                GVec2 w3 = {wx1 - px, wy1 - py};

                ENSURE_CAP(ex_verts,   ex_verts_cap,   24, GVec3);
                ENSURE_CAP(ex_normals, ex_normals_cap, 24, GVec3);
                ENSURE_CAP(ex_idx,     ex_idx_cap,     36, uint32_t);
                uint32_t tcount = geom_build_cuboid(w0, w1, w2, w3, extrusion_depth,
                                                    ex_verts, ex_normals, ex_idx);
                if (tcount > 0) {
                    ENSURE_CAP(verts, verts_cap, 24, SceneVertex);
                    for (int k = 0; k < 24; k++) {
                        SceneVertex v = {0};
                        v.pos[0]    = ex_verts[k].x;
                        v.pos[1]    = ex_verts[k].y;
                        v.pos[2]    = ex_verts[k].z;
                        v.normal[0] = ex_normals[k].x;
                        v.normal[1] = ex_normals[k].y;
                        v.normal[2] = ex_normals[k].z;
                        v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
                        verts[k] = v;
                    }
                    int mid = scene_add_mesh(out_scene, verts, 24, ex_idx, tcount * 3);
                    SceneNode nd = {0};
                    scene_mat_identity(nd.local_xform);
                    nd.parent      = -1;
                    nd.mesh_id     = mid;
                    nd.material_id = stroke_mat;
                    nd.flags       = SCENE_NODE_VISIBLE | SCENE_NODE_SELECTABLE;
                    nd.svg_elem    = SVG_ELEM_LINE;
                    nd.depth       = extrusion_depth;
                    memcpy(nd.svg_params, prim->params, sizeof nd.svg_params);
                    nd.href        = node_href;
                    scene_add_node(out_scene, &nd);
                    stroke_nodes++;
                    line_swaps++;
                } else {
                    /* degenerate length — let the ribbon path handle it */
                    line_cuboid_emit = 0;
                }
            } else {
                line_cuboid_emit = 0;
            }
        }

        for (NSVGpath *pth = shape->paths; pth; pth = pth->next) {
            uint32_t poly_count = 0;
            flatten_nsvg_path(pth, flatness, &poly, &poly_count, &poly_cap);
            if (poly_count < 2) continue;

            /* --- Fill --- */
            if (has_fill && !cuboid_emit && !cylinder_emit && poly_count >= 3) {
                if (extrude) {
                    /* Prism now emits 6n verts (caps + 4 per side quad) with
                     * authored flat face normals. Index count unchanged. */
                    uint32_t vmax  = poly_count * 6;
                    uint32_t ineed = 6 * (poly_count - 2) + 6 * poly_count;
                    ENSURE_CAP(ex_verts,   ex_verts_cap,   vmax,  GVec3);
                    ENSURE_CAP(ex_normals, ex_normals_cap, vmax,  GVec3);
                    ENSURE_CAP(ex_idx,     ex_idx_cap,     ineed, uint32_t);
                    uint32_t ex_vcount = 0;
                    uint32_t tcount = geom_extrude_polygon(poly, poly_count,
                                                           extrusion_depth,
                                                           ex_verts, ex_normals,
                                                           ex_idx, &ex_vcount);
                    if (tcount > 0) {
                        ENSURE_CAP(verts, verts_cap, ex_vcount, SceneVertex);
                        for (uint32_t k = 0; k < ex_vcount; k++) {
                            SceneVertex v = {0};
                            v.pos[0]    = ex_verts[k].x;
                            v.pos[1]    = ex_verts[k].y;
                            v.pos[2]    = ex_verts[k].z;
                            v.normal[0] = ex_normals[k].x;
                            v.normal[1] = ex_normals[k].y;
                            v.normal[2] = ex_normals[k].z;
                            v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
                            verts[k] = v;
                        }
                        int mid = scene_add_mesh(out_scene, verts, ex_vcount,
                                                 ex_idx, tcount * 3);
                        SceneNode nd = {0};
                        scene_mat_identity(nd.local_xform);
                        nd.parent      = -1;
                        nd.mesh_id     = mid;
                        nd.material_id = fill_mat;
                        nd.flags       = SCENE_NODE_VISIBLE | SCENE_NODE_SELECTABLE;
                        nd.svg_elem    = node_elem;
                        nd.depth       = extrusion_depth;
                        if (prim) memcpy(nd.svg_params, prim->params, sizeof nd.svg_params);
                        nd.href        = node_href;
                        scene_add_node(out_scene, &nd);
                        fill_nodes++;
                    }
                } else {
                    ENSURE_CAP(tri_idx, tri_cap, 3 * (poly_count - 2), uint32_t);
                    uint32_t tcount = geom_triangulate(poly, poly_count, tri_idx);
                    if (tcount > 0) {
                        ENSURE_CAP(verts, verts_cap, poly_count, SceneVertex);
                        for (uint32_t k = 0; k < poly_count; k++) {
                            SceneVertex v = {0};
                            v.pos[0] = poly[k].x;
                            v.pos[1] = poly[k].y;
                            /* normal = (0,0,0) so the shader falls through to
                             * unlit shade=1.0 for flat 2D fills. */
                            v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
                            verts[k] = v;
                        }
                        int mid = scene_add_mesh(out_scene, verts, poly_count,
                                                 tri_idx, tcount * 3);
                        SceneNode nd = {0};
                        scene_mat_identity(nd.local_xform);
                        nd.parent      = -1;
                        nd.mesh_id     = mid;
                        nd.material_id = fill_mat;
                        nd.flags       = SCENE_NODE_VISIBLE | SCENE_NODE_SELECTABLE;
                        nd.svg_elem    = node_elem;
                        if (prim) memcpy(nd.svg_params, prim->params, sizeof nd.svg_params);
                        nd.href        = node_href;
                        scene_add_node(out_scene, &nd);
                        fill_nodes++;
                    }
                }
            }

            /* --- Stroke --- */
            if (has_stroke && !line_cuboid_emit) {
                uint32_t rv_count = poly_count * 2;
                ENSURE_CAP(ribbon, ribbon_cap, rv_count, GVec2);
                geom_stroke_ribbon(poly, poly_count, shape->strokeWidth * 0.5f,
                                   pth->closed, ribbon);

                uint32_t seg_count = poly_count - 1;
                if (pth->closed) seg_count = poly_count;
                uint32_t need_idx = seg_count * 6;

                ENSURE_CAP(verts,     verts_cap, rv_count,  SceneVertex);
                ENSURE_CAP(strip_idx, strip_cap, need_idx, uint32_t);

                float stroke_z = extrude ? extrusion_depth + 0.5f : 0.0f;
                for (uint32_t k = 0; k < rv_count; k++) {
                    SceneVertex v = {0};
                    v.pos[0] = ribbon[k].x;
                    v.pos[1] = ribbon[k].y;
                    v.pos[2] = stroke_z;
                    /* Zero normal — ribbons are flat, want full brightness. */
                    v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
                    verts[k] = v;
                }
                uint32_t ic = 0;
                for (uint32_t i = 0; i < seg_count; i++) {
                    uint32_t a = (i * 2) % rv_count;
                    uint32_t b = (i * 2 + 1) % rv_count;
                    uint32_t c = ((i + 1) * 2) % rv_count;
                    uint32_t d = ((i + 1) * 2 + 1) % rv_count;
                    strip_idx[ic++] = a; strip_idx[ic++] = b; strip_idx[ic++] = c;
                    strip_idx[ic++] = b; strip_idx[ic++] = d; strip_idx[ic++] = c;
                }
                int mid = scene_add_mesh(out_scene, verts, rv_count,
                                         strip_idx, ic);
                SceneNode nd = {0};
                scene_mat_identity(nd.local_xform);
                nd.parent      = -1;
                nd.mesh_id     = mid;
                nd.material_id = stroke_mat;
                nd.flags       = SCENE_NODE_VISIBLE | SCENE_NODE_SELECTABLE;
                nd.svg_elem    = node_elem;
                if (prim) memcpy(nd.svg_params, prim->params, sizeof nd.svg_params);
                nd.href        = node_href;
                scene_add_node(out_scene, &nd);
                stroke_nodes++;
            }
        }
    }

    free(poly);
    free(tri_idx);
    free(ribbon);
    free(strip_idx);
    free(verts);
    free(ex_verts);
    free(ex_normals);
    free(ex_idx);

    if (extrude) out_scene->depth_enabled = 1;

    float svg_w_final = img->width;
    float svg_h_final = img->height;
    nsvgDelete(img);

    fprintf(stderr, "utterance: scene built — %d shapes, %d fill + %d stroke nodes, "
            "%d cuboid / %d cylinder / %d line swaps, %u verts\n",
            shape_count, fill_nodes, stroke_nodes,
            cuboid_swaps, cylinder_swaps, line_swaps, out_scene->vert_count);

    /* Text extraction — reuses the file buffer we slurped for primitive
     * parsing, avoiding a second file read. */
    if (out_texts && file_buf) {
        extract_text_elements(file_buf, file_len, root_xf, out_texts);
        fprintf(stderr, "utterance: parsed %d <text> elements (viewbox %.0fx%.0f)\n",
                out_texts->count, svg_w_final, svg_h_final);
    }

    prim_list_destroy(&prims);
    free(file_buf);

    return 0;
}

/* ---------------------------------------------------------------- mesh build */

/* Measure the total advance width of a UTF-8 string in font pixel units. */
static float measure_utf8_advance(Font *font, const char *utf8) {
    const uint8_t *p = (const uint8_t *)utf8;
    const uint8_t *end = p + strlen(utf8);
    float adv = 0;
    while (p < end) {
        uint32_t cp = utf8_decode(&p, end);
        if (cp == 0) break;
        Glyph *g = font_glyph(font, cp);
        if (g) adv += g->advance;
    }
    return adv;
}

void svg_build_text_mesh(TextMesh *mesh, SvgLinkList *links, Font *font,
                         const SvgTextList *texts,
                         float svg_w, float svg_h,
                         float wx, float wy, float ww, float wh,
                         float wz) {
    if (!mesh || !font || !texts || svg_w <= 0 || svg_h <= 0 || ww <= 0 || wh <= 0)
        return;

    /* World units per SVG user unit. If the caller sized the image quad from
     * (svg_w, svg_h) with preserved aspect, sx == sy. Use sx for horizontal
     * advance (keeps letter proportions) and sy only for the Y coord map. */
    float sx = ww / svg_w;
    float sy = wh / svg_h;

    for (int i = 0; i < texts->count; i++) {
        const SvgText *t = &texts->items[i];
        if (!t->utf8 || !t->utf8[0]) continue;

        /* Glyphs are natively sized at font->px_size. Scale them to the SVG's
         * requested font-size, then into world units. */
        float font_scale = (t->font_size_px * sx) / font->px_size;
        if (font_scale <= 0) continue;

        /* SVG anchor → world. Flip Y: SVG is y-down, world is y-up. */
        float origin_x = wx + t->x * sx;
        float origin_y = wy + wh - t->y * sy;

        /* Rotation: the SVG transform angle is in y-down SVG coords. Flipping
         * Y to get world coords inverts rotation sign. Pivot is the anchor
         * point in world space — SVG rotates the text run's local frame
         * around the anchor, which is where the glyphs fan out from. */
        float rot_rad = -t->rotate_deg * (float)M_PI / 180.0f;
        float rot_cos = cosf(rot_rad);
        float rot_sin = sinf(rot_rad);

        /* text-anchor: shift origin by total advance so the run ends up in the
         * right place. Measured in native font px, scaled by font_scale. */
        if (t->anchor != 0) {
            float total_adv = measure_utf8_advance(font, t->utf8) * font_scale;
            if (t->anchor == 1)       origin_x -= total_adv * 0.5f;  /* middle */
            else if (t->anchor == 2)  origin_x -= total_adv;         /* end */
        }

        const uint8_t *p = (const uint8_t *)t->utf8;
        const uint8_t *end = p + strlen(t->utf8);
        float cursor_x_native = 0.0f;
        int glyph_start = mesh->count;

        while (p < end) {
            uint32_t cp = utf8_decode(&p, end);
            if (cp == 0) break;
            Glyph *g = font_glyph(font, cp);
            if (!g) continue;

            if (g->width > 0 && g->height > 0) {
                /* Glyph quad in native font-px coords with baseline at y=0,
                 * mirroring text.c's emit_glyph. */
                float nx = cursor_x_native + g->x_off;
                float ny = -g->y_off - g->height;
                float nw = g->width;
                float nh = g->height;

                GlyphInstance inst;
                inst.x = origin_x + nx * font_scale;
                inst.y = origin_y + ny * font_scale;
                inst.z = wz;
                inst.w = nw * font_scale;
                inst.h = nh * font_scale;
                inst.s0 = g->s0; inst.t0 = g->t0;
                inst.s1 = g->s1; inst.t1 = g->t1;
                inst.text_offset = -1;
                inst.r = t->r; inst.g = t->g; inst.b = t->b;
                inst.pivot_x = origin_x;
                inst.pivot_y = origin_y;
                inst.rot_cos = rot_cos;
                inst.rot_sin = rot_sin;

                text_mesh_push(mesh, &inst);
            }
            cursor_x_native += g->advance;
        }

        /* If this run sat inside an <a xlink:href>, record the glyph range
         * so the caller can Ctrl+click any letter and reach the link. */
        if (links && t->href && mesh->count > glyph_start) {
            SvgLink L;
            L.glyph_start = glyph_start;
            L.glyph_end   = mesh->count;
            L.href        = strdup(t->href);
            link_list_append(links, L);
        }
    }
}
