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

/* Find end of current line, returns pointer to \n or end */
static const uint8_t *line_end(const uint8_t *p, const uint8_t *end) {
    while (p < end && *p != '\n') p++;
    return p;
}

/* Count leading chars matching c */
static int count_leading(const uint8_t *p, const uint8_t *end, char c) {
    int n = 0;
    while (p + n < end && p[n] == (uint8_t)c) n++;
    return n;
}

/* Skip leading whitespace, return count */
static int skip_spaces(const uint8_t *p, const uint8_t *end) {
    int n = 0;
    while (p + n < end && (p[n] == ' ' || p[n] == '\t')) n++;
    return n;
}

/* Process inline markdown within a line: **bold**, *italic*, `code`, [link](url) */
static void emit_inline(Buf *out, const uint8_t *p, const uint8_t *end) {
    while (p < end) {
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
                emit_inline(out, start, close);
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
                emit_inline(out, start, close);
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
                    buf_str(out, MD_LINK);
                    buf_push(out, tstart, (size_t)(tclose - tstart));
                    buf_str(out, RESET);
                    buf_str(out, MD_URL);
                    buf_str(out, " (");
                    buf_push(out, ustart, (size_t)(uclose - ustart));
                    buf_str(out, ")");
                    buf_str(out, RESET);
                    p = uclose + 1;
                    continue;
                }
            }
        }

        /* Plain character */
        buf_push(out, p, 1);
        p++;
    }
}

/* Check if a line is a table separator (e.g. |---|---|) */
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

uint8_t *markdown_to_ansi(const uint8_t *src, size_t src_len, size_t *out_len) {
    Buf out;
    buf_init(&out);
    if (!out.buf) return NULL;

    const uint8_t *p = src;
    const uint8_t *end = src + src_len;
    int in_code_block = 0;

    while (p < end) {
        const uint8_t *le = line_end(p, end);
        int sp = skip_spaces(p, le);

        /* Fenced code block: ``` */
        if (starts_with(p + sp, le, "```")) {
            if (in_code_block) {
                in_code_block = 0;
                buf_str(&out, RESET);
            } else {
                in_code_block = 1;
                buf_str(&out, MD_CBLOCK);
                /* Skip the ``` line content (language hint) */
            }
            /* Advance past this line */
            p = le < end ? le + 1 : le;
            continue;
        }

        if (in_code_block) {
            buf_str(&out, MD_CBLOCK);
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

        /* Heading: # ... ###### */
        if (*lp == '#') {
            int level = count_leading(lp, le, '#');
            if (level >= 1 && level <= 6 && lp + level < le && lp[level] == ' ') {
                const char *colors[] = { H1, H2, H3, H4, H5, H6 };
                buf_str(&out, colors[level - 1]);
                emit_inline(&out, lp + level + 1, le);
                buf_str(&out, RESET);
                if (le < end) buf_push(&out, "\n", 1);
                p = le < end ? le + 1 : le;
                    continue;
            }
        }

        /* Horizontal rule: --- or *** or ___ (3+ chars, optional spaces) */
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
                /* Emit a visual horizontal rule */
                for (int i = 0; i < 40; i++) buf_push(&out, "\xe2\x94\x80", 3); /* ─ */
                buf_str(&out, RESET);
                if (le < end) buf_push(&out, "\n", 1);
                p = le < end ? le + 1 : le;
                    continue;
            }
        }

        /* Blockquote: > ... */
        if (*lp == '>') {
            const uint8_t *content = lp + 1;
            if (content < le && *content == ' ') content++;
            buf_str(&out, MD_QUOTE);
            buf_push(&out, "\xe2\x94\x82", 3); /* │ */
            buf_push(&out, " ", 1);
            emit_inline(&out, content, le);
            buf_str(&out, RESET);
            if (le < end) buf_push(&out, "\n", 1);
            p = le < end ? le + 1 : le;
            continue;
        }

        /* Unordered list: - item, * item, + item */
        if ((*lp == '-' || *lp == '*' || *lp == '+') && lp + 1 < le && lp[1] == ' ') {
            buf_str(&out, MD_BULLET);
            buf_push(&out, "\xe2\x80\xa2", 3); /* • */
            buf_str(&out, RESET);
            buf_push(&out, " ", 1);
            emit_inline(&out, lp + 2, le);
            if (le < end) buf_push(&out, "\n", 1);
            p = le < end ? le + 1 : le;
            continue;
        }

        /* Ordered list: 1. item, 2. item, etc. */
        if (*lp >= '0' && *lp <= '9') {
            const uint8_t *np = lp;
            while (np < le && *np >= '0' && *np <= '9') np++;
            if (np < le && *np == '.' && np + 1 < le && np[1] == ' ') {
                buf_str(&out, MD_BULLET);
                buf_push(&out, lp, (size_t)(np - lp + 1));
                buf_str(&out, RESET);
                buf_push(&out, " ", 1);
                emit_inline(&out, np + 2, le);
                if (le < end) buf_push(&out, "\n", 1);
                p = le < end ? le + 1 : le;
                    continue;
            }
        }

        /* Table row: | ... | ... | */
        if (*lp == '|') {
            /* Check if this is a separator row */
            if (is_table_sep(lp, le)) {
                /* Skip separator — the header was already styled */
                p = le < end ? le + 1 : le;
                continue;
            }

            /* Determine if this is a header row (next line is a separator) */
            int is_header = 0;
            if (le < end) {
                const uint8_t *next_line = le + 1;
                const uint8_t *next_le = line_end(next_line, end);
                if (is_table_sep(next_line, next_le))
                    is_header = 1;
            }

            const char *style = is_header ? MD_THEAD : MD_TABLE;

            /* Parse cells */
            const uint8_t *cp = lp + 1;
            buf_str(&out, MD_TABLE);
            buf_push(&out, "\xe2\x94\x82", 3); /* │ */
            buf_str(&out, RESET);

            while (cp < le) {
                const uint8_t *cell_end = cp;
                while (cell_end < le && *cell_end != '|') cell_end++;

                /* Trim whitespace from cell */
                const uint8_t *cs = cp, *ce = cell_end;
                while (cs < ce && *cs == ' ') cs++;
                while (ce > cs && ce[-1] == ' ') ce--;

                buf_str(&out, style);
                buf_push(&out, " ", 1);
                emit_inline(&out, cs, ce);
                buf_push(&out, " ", 1);
                buf_str(&out, RESET);
                buf_str(&out, MD_TABLE);
                buf_push(&out, "\xe2\x94\x82", 3); /* │ */
                buf_str(&out, RESET);

                if (cell_end < le) cp = cell_end + 1;
                else break;
            }
            if (le < end) buf_push(&out, "\n", 1);
            p = le < end ? le + 1 : le;
            continue;
        }

        /* Plain paragraph: just emit with inline formatting */
        emit_inline(&out, lp, le);
        if (le < end) buf_push(&out, "\n", 1);
        p = le < end ? le + 1 : le;
    }

    *out_len = out.len;
    return out.buf;
}

int markdown_detect(const uint8_t *src, size_t len) {
    /* Heuristic: scan first ~2KB for markdown patterns */
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

        /* Check for inline markers */
        for (const uint8_t *c = lp; c + 1 < le; c++) {
            if (c[0] == '*' && c[1] == '*') { score += 1; break; }
            if (c[0] == '[' && memchr(c, ']', (size_t)(le - c))) { score += 1; break; }
            if (c[0] == '`') { score += 1; break; }
        }

        p = le < end ? le + 1 : le;
    }
    return score >= 4;
}
