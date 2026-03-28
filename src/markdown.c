#include "markdown.h"
#include <stdlib.h>
#include <string.h>

/* ANSI SGR sequences for markdown elements */
#define RESET       "\033[0m"
#define BOLD        "\033[1m"
#define DIM         "\033[2m"
#define H1          "\033[1;36m"   /* bold cyan */
#define H2          "\033[1;34m"   /* bold blue */
#define H3          "\033[1;35m"   /* bold magenta */
#define H4          "\033[35m"     /* magenta */
#define H5          "\033[34m"     /* blue */
#define H6          "\033[2;34m"   /* dim blue */
#define MD_BOLD     "\033[1;97m"   /* bold bright white */
#define MD_ITALIC   "\033[3;33m"   /* italic yellow (mapped to color) */
#define MD_CODE     "\033[38;5;178m" /* amber */
#define MD_CBLOCK   "\033[38;5;248m" /* light gray */
#define MD_LINK     "\033[34m"     /* blue */
#define MD_URL      "\033[2;34m"   /* dim blue */
#define MD_BULLET   "\033[36m"     /* cyan */
#define MD_QUOTE    "\033[2;37m"   /* dim white */
#define MD_HR       "\033[2;36m"   /* dim cyan */
#define MD_TABLE    "\033[37m"     /* white */
#define MD_THEAD    "\033[1;37m"   /* bold white */
#define MD_IMG      "\033[38;5;141m" /* light purple for image placeholder */

typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
} Buf;

static void buf_init(Buf *b) {
    b->cap = 4096;
    b->buf = malloc(b->cap);
    b->len = 0;
}

static void buf_push(Buf *b, const void *data, size_t n) {
    while (b->len + n > b->cap) {
        b->cap *= 2;
        b->buf = realloc(b->buf, b->cap);
    }
    memcpy(b->buf + b->len, data, n);
    b->len += n;
}

static void buf_str(Buf *b, const char *s) {
    buf_push(b, s, strlen(s));
}

static int starts_with(const uint8_t *p, const uint8_t *end, const char *prefix) {
    size_t n = strlen(prefix);
    if ((size_t)(end - p) < n) return 0;
    return memcmp(p, prefix, n) == 0;
}

static const uint8_t *line_end(const uint8_t *p, const uint8_t *end) {
    while (p < end && *p != '\n') p++;
    return p;
}

static int count_leading(const uint8_t *p, const uint8_t *end, char c) {
    int n = 0;
    while (p + n < end && p[n] == (uint8_t)c) n++;
    return n;
}

static int skip_spaces(const uint8_t *p, const uint8_t *end) {
    int n = 0;
    while (p + n < end && (p[n] == ' ' || p[n] == '\t')) n++;
    return n;
}

/* Record an image reference */
static void push_image(MdImageList *il, const char *alt, size_t alt_len,
                       const char *path, size_t path_len, int placeholder) {
    if (!il) return;
    if (il->count >= il->capacity) {
        int new_cap = il->capacity ? il->capacity * 2 : 8;
        MdImage *tmp = realloc(il->images, (size_t)new_cap * sizeof(MdImage));
        if (!tmp) return;
        il->images = tmp;
        il->capacity = new_cap;
    }
    MdImage *img = &il->images[il->count++];
    img->alt = malloc(alt_len + 1);
    memcpy(img->alt, alt, alt_len);
    img->alt[alt_len] = '\0';
    img->path = malloc(path_len + 1);
    memcpy(img->path, path, path_len);
    img->path[path_len] = '\0';
    img->placeholder = placeholder;
}

static void push_link(MdLinkList *ll, const char *url, size_t url_len,
                      int start_offset, int end_offset) {
    if (!ll) return;
    if (ll->count >= ll->capacity) {
        int new_cap = ll->capacity ? ll->capacity * 2 : 16;
        MdLink *tmp = realloc(ll->links, (size_t)new_cap * sizeof(MdLink));
        if (!tmp) return;
        ll->links = tmp;
        ll->capacity = new_cap;
    }
    MdLink *lk = &ll->links[ll->count++];
    lk->url = malloc(url_len + 1);
    memcpy(lk->url, url, url_len);
    lk->url[url_len] = '\0';
    lk->start_offset = start_offset;
    lk->end_offset = end_offset;
}

/* Process inline markdown: **bold**, *italic*, `code`, [link](url), ![img](path) */
static void emit_inline(Buf *out, const uint8_t *p, const uint8_t *end,
                        MdImageList *img_out, MdLinkList *link_out) {
    while (p < end) {
        /* Image: ![alt](path) */
        if (*p == '!' && p + 1 < end && p[1] == '[') {
            const uint8_t *astart = p + 2;
            const uint8_t *aclose = astart;
            while (aclose < end && *aclose != ']') aclose++;
            if (aclose < end && aclose + 1 < end && aclose[1] == '(') {
                const uint8_t *pstart = aclose + 2;
                const uint8_t *pclose = pstart;
                while (pclose < end && *pclose != ')') pclose++;
                if (pclose < end) {
                    int placeholder = (int)out->len;
                    buf_str(out, MD_IMG);
                    buf_push(out, "\xe2\x96\xa3", 3); /* ▣ */
                    buf_push(out, " ", 1);
                    buf_push(out, astart, (size_t)(aclose - astart));
                    buf_str(out, RESET);
                    push_image(img_out,
                               (const char *)astart, (size_t)(aclose - astart),
                               (const char *)pstart, (size_t)(pclose - pstart),
                               placeholder);
                    p = pclose + 1;
                    continue;
                }
            }
        }

        /* Code span: `...` */
        if (*p == '`') {
            const uint8_t *start = p + 1;
            const uint8_t *close = start;
            while (close < end && *close != '`') close++;
            if (close < end) {
                buf_str(out, MD_CODE);
                buf_push(out, start, (size_t)(close - start));
                buf_str(out, RESET);
                p = close + 1;
                continue;
            }
        }

        /* Bold: **...**  or __...__ */
        if (p + 1 < end && ((*p == '*' && p[1] == '*') || (*p == '_' && p[1] == '_'))) {
            char marker = *p;
            const uint8_t *start = p + 2;
            const uint8_t *close = start;
            while (close + 1 < end && !(close[0] == marker && close[1] == marker)) close++;
            if (close + 1 < end) {
                buf_str(out, MD_BOLD);
                emit_inline(out, start, close, img_out, link_out);
                buf_str(out, RESET);
                p = close + 2;
                continue;
            }
        }

        /* Italic: *...* or _..._ (single) */
        if ((*p == '*' || *p == '_') && p + 1 < end && p[1] != *p) {
            char marker = *p;
            const uint8_t *start = p + 1;
            const uint8_t *close = start;
            while (close < end && *close != marker) close++;
            if (close < end && close > start) {
                buf_str(out, MD_ITALIC);
                emit_inline(out, start, close, img_out, link_out);
                buf_str(out, RESET);
                p = close + 1;
                continue;
            }
        }

        /* Link: [text](url) */
        if (*p == '[') {
            const uint8_t *tstart = p + 1;
            const uint8_t *tclose = tstart;
            while (tclose < end && *tclose != ']') tclose++;
            if (tclose < end && tclose + 1 < end && tclose[1] == '(') {
                const uint8_t *ustart = tclose + 2;
                const uint8_t *uclose = ustart;
                while (uclose < end && *uclose != ')') uclose++;
                if (uclose < end) {
                    int link_start = (int)out->len;
                    buf_str(out, MD_LINK);
                    buf_push(out, tstart, (size_t)(tclose - tstart));
                    buf_str(out, RESET);
                    buf_str(out, MD_URL);
                    buf_str(out, " (");
                    buf_push(out, ustart, (size_t)(uclose - ustart));
                    buf_str(out, ")");
                    buf_str(out, RESET);
                    int link_end = (int)out->len;
                    push_link(link_out, (const char *)ustart,
                              (size_t)(uclose - ustart), link_start, link_end);
                    p = uclose + 1;
                    continue;
                }
            }
        }

        buf_push(out, p, 1);
        p++;
    }
}

static int is_table_sep(const uint8_t *p, const uint8_t *end) {
    int sp = skip_spaces(p, end);
    p += sp;
    if (p >= end || *p != '|') return 0;
    int has_dash = 0;
    while (p < end) {
        if (*p == '-' || *p == ':') has_dash = 1;
        else if (*p != '|' && *p != ' ') return 0;
        p++;
    }
    return has_dash;
}

uint8_t *markdown_to_ansi(const uint8_t *src, size_t src_len, size_t *out_len,
                          MdImageList *img_out, MdLinkList *link_out,
                          MdCodeBlockList *code_out) {
    Buf out;
    buf_init(&out);
    if (!out.buf) return NULL;
    if (img_out) { img_out->images = NULL; img_out->count = 0; img_out->capacity = 0; }
    if (link_out) { link_out->links = NULL; link_out->count = 0; link_out->capacity = 0; }
    if (code_out) { code_out->blocks = NULL; code_out->count = 0; code_out->capacity = 0; }

    const uint8_t *p = src;
    const uint8_t *end = src + src_len;
    int in_code_block = 0;
    int code_block_start = 0;

    while (p < end) {
        const uint8_t *le = line_end(p, end);
        int sp = skip_spaces(p, le);

        /* Fenced code block: ``` */
        if (starts_with(p + sp, le, "```")) {
            if (in_code_block) {
                /* Record code block region */
                if (code_out) {
                    if (code_out->count >= code_out->capacity) {
                        int nc = code_out->capacity ? code_out->capacity * 2 : 8;
                        code_out->blocks = realloc(code_out->blocks, (size_t)nc * sizeof(MdCodeBlock));
                        code_out->capacity = nc;
                    }
                    code_out->blocks[code_out->count].start_offset = code_block_start;
                    code_out->blocks[code_out->count].end_offset = (int)out.len;
                    code_out->count++;
                }
                in_code_block = 0;
                buf_str(&out, RESET);
            } else {
                in_code_block = 1;
                code_block_start = (int)out.len;
                buf_str(&out, MD_CBLOCK);
            }
            p = le < end ? le + 1 : le;
            continue;
        }

        if (in_code_block) {
            buf_str(&out, MD_CBLOCK);
            buf_push(&out, "    ", 4); /* indent code blocks */
            buf_push(&out, p, (size_t)(le - p));
            buf_str(&out, RESET);
            if (le < end) buf_push(&out, "\n", 1);
            p = le < end ? le + 1 : le;
            continue;
        }

        /* Empty line */
        if (sp == (int)(le - p)) {
            buf_push(&out, "\n", 1);
            p = le < end ? le + 1 : le;
            continue;
        }

        const uint8_t *lp = p + sp;

        /* Heading */
        if (*lp == '#') {
            int level = count_leading(lp, le, '#');
            if (level >= 1 && level <= 6 && lp + level < le && lp[level] == ' ') {
                const char *colors[] = { H1, H2, H3, H4, H5, H6 };
                buf_str(&out, colors[level - 1]);
                emit_inline(&out, lp + level + 1, le, img_out, link_out);
                buf_str(&out, RESET);
                if (le < end) buf_push(&out, "\n", 1);
                p = le < end ? le + 1 : le;
                continue;
            }
        }

        /* Horizontal rule */
        {
            int dashes = 0, stars = 0, undscr = 0, other = 0;
            for (const uint8_t *c = lp; c < le; c++) {
                if (*c == '-') dashes++;
                else if (*c == '*') stars++;
                else if (*c == '_') undscr++;
                else if (*c != ' ') other++;
            }
            if (!other && (dashes >= 3 || stars >= 3 || undscr >= 3)) {
                buf_str(&out, MD_HR);
                for (int i = 0; i < 40; i++) buf_push(&out, "\xe2\x94\x80", 3);
                buf_str(&out, RESET);
                if (le < end) buf_push(&out, "\n", 1);
                p = le < end ? le + 1 : le;
                continue;
            }
        }

        /* Blockquote */
        if (*lp == '>') {
            const uint8_t *content = lp + 1;
            if (content < le && *content == ' ') content++;
            buf_str(&out, MD_QUOTE);
            buf_push(&out, "\xe2\x94\x82", 3);
            buf_push(&out, " ", 1);
            emit_inline(&out, content, le, img_out, link_out);
            buf_str(&out, RESET);
            if (le < end) buf_push(&out, "\n", 1);
            p = le < end ? le + 1 : le;
            continue;
        }

        /* Unordered list */
        if ((*lp == '-' || *lp == '*' || *lp == '+') && lp + 1 < le && lp[1] == ' ') {
            buf_str(&out, MD_BULLET);
            buf_push(&out, "\xe2\x80\xa2", 3);
            buf_str(&out, RESET);
            buf_push(&out, " ", 1);
            emit_inline(&out, lp + 2, le, img_out, link_out);
            if (le < end) buf_push(&out, "\n", 1);
            p = le < end ? le + 1 : le;
            continue;
        }

        /* Ordered list */
        if (*lp >= '0' && *lp <= '9') {
            const uint8_t *np = lp;
            while (np < le && *np >= '0' && *np <= '9') np++;
            if (np < le && *np == '.' && np + 1 < le && np[1] == ' ') {
                buf_str(&out, MD_BULLET);
                buf_push(&out, lp, (size_t)(np - lp + 1));
                buf_str(&out, RESET);
                buf_push(&out, " ", 1);
                emit_inline(&out, np + 2, le, img_out, link_out);
                if (le < end) buf_push(&out, "\n", 1);
                p = le < end ? le + 1 : le;
                continue;
            }
        }

        /* Table row */
        if (*lp == '|') {
            if (is_table_sep(lp, le)) {
                p = le < end ? le + 1 : le;
                continue;
            }
            int is_header = 0;
            if (le < end) {
                const uint8_t *next_line = le + 1;
                const uint8_t *next_le = line_end(next_line, end);
                if (is_table_sep(next_line, next_le))
                    is_header = 1;
            }
            const char *style = is_header ? MD_THEAD : MD_TABLE;
            const uint8_t *cp = lp + 1;
            buf_str(&out, MD_TABLE);
            buf_push(&out, "\xe2\x94\x82", 3);
            buf_str(&out, RESET);
            while (cp < le) {
                const uint8_t *cell_end = cp;
                while (cell_end < le && *cell_end != '|') cell_end++;
                const uint8_t *cs = cp, *ce = cell_end;
                while (cs < ce && *cs == ' ') cs++;
                while (ce > cs && ce[-1] == ' ') ce--;
                buf_str(&out, style);
                buf_push(&out, " ", 1);
                emit_inline(&out, cs, ce, img_out, link_out);
                buf_push(&out, " ", 1);
                buf_str(&out, RESET);
                buf_str(&out, MD_TABLE);
                buf_push(&out, "\xe2\x94\x82", 3);
                buf_str(&out, RESET);
                if (cell_end < le) cp = cell_end + 1;
                else break;
            }
            if (le < end) buf_push(&out, "\n", 1);
            p = le < end ? le + 1 : le;
            continue;
        }

        /* Plain paragraph */
        emit_inline(&out, lp, le, img_out, link_out);
        if (le < end) buf_push(&out, "\n", 1);
        p = le < end ? le + 1 : le;
    }

    *out_len = out.len;
    return out.buf;
}

int markdown_detect(const uint8_t *src, size_t len) {
    size_t scan = len < 2048 ? len : 2048;
    const uint8_t *p = src;
    const uint8_t *end = src + scan;
    int score = 0;

    while (p < end) {
        const uint8_t *le = line_end(p, end);
        int sp = skip_spaces(p, le);
        const uint8_t *lp = p + sp;

        if (*lp == '#' && lp + 1 < le && lp[1] == ' ') score += 3;
        else if (*lp == '#' && lp + 1 < le && lp[1] == '#') score += 3;
        else if (starts_with(lp, le, "```")) score += 3;
        else if (starts_with(lp, le, "---") || starts_with(lp, le, "***")) score += 1;
        else if (*lp == '>' && lp + 1 < le && lp[1] == ' ') score += 2;
        else if ((*lp == '-' || *lp == '*') && lp + 1 < le && lp[1] == ' ') score += 1;
        else if (*lp == '|') score += 2;

        for (const uint8_t *c = lp; c + 1 < le; c++) {
            if (c[0] == '*' && c[1] == '*') { score += 1; break; }
            if (c[0] == '[' && memchr(c, ']', (size_t)(le - c))) { score += 1; break; }
            if (c[0] == '`') { score += 1; break; }
        }

        p = le < end ? le + 1 : le;
    }
    return score >= 4;
}

void md_image_list_destroy(MdImageList *il) {
    for (int i = 0; i < il->count; i++) {
        free(il->images[i].alt);
        free(il->images[i].path);
    }
    free(il->images);
    memset(il, 0, sizeof(*il));
}

void md_link_list_destroy(MdLinkList *ll) {
    for (int i = 0; i < ll->count; i++)
        free(ll->links[i].url);
    free(ll->links);
    memset(ll, 0, sizeof(*ll));
}

void md_codeblock_list_destroy(MdCodeBlockList *cl) {
    free(cl->blocks);
    memset(cl, 0, sizeof(*cl));
}
