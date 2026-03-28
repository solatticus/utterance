#include "linebreak.h"
#include "utf8.h"
#include <stdlib.h>
#include <string.h>

static int push_line(LineList *ll, Line *l) {
    if (ll->count >= ll->capacity) {
        int new_cap = ll->capacity ? ll->capacity * 2 : 256;
        Line *tmp = realloc(ll->lines, (size_t)new_cap * sizeof(Line));
        if (!tmp) return -1;
        ll->lines = tmp;
        ll->capacity = new_cap;
    }
    ll->lines[ll->count++] = *l;
    return 0;
}

static void emit(LineList *out, int seg_start, int seg_end, float width,
                 int has_hyphen, int byte_offset) {
    Line l = { .seg_start = seg_start, .seg_end = seg_end, .width = width,
               .has_hyphen = has_hyphen, .byte_offset = byte_offset };
    push_line(out, &l);
}

/* Character-level overflow break: walk codepoints, emit multiple lines.
   Each sub-line references the same segment but with different byte_offset. */
static void overflow_break(LineList *out, const Segment *seg, float max_width,
                           Font *font, const uint8_t *text,
                           int seg_idx, float line_width, int line_start) {
    /* Flush pending segments before the overflow word */
    if (line_start < seg_idx) {
        emit(out, line_start, seg_idx, line_width, 0, 0);
    }

    const uint8_t *base = text + seg->byte_start;
    const uint8_t *p = base;
    const uint8_t *end = base + seg->byte_len;
    float run_width = 0.0f;
    int sub_start = seg->byte_start;

    while (p < end) {
        const uint8_t *cp_start = p;
        uint32_t cp = utf8_decode(&p, end);
        if (cp == 0) break;

        Glyph *g = font_glyph(font, cp);
        float adv = g ? g->advance : 0.0f;

        if (run_width + adv > max_width && run_width > 0) {
            /* Emit sub-line up to (not including) this codepoint */
            emit(out, seg_idx, seg_idx + 1, run_width, 0, sub_start);
            sub_start = (int)(cp_start - text);
            run_width = 0.0f;
        }
        run_width += adv;
    }
    /* Emit final sub-line */
    if (run_width > 0 || sub_start == seg->byte_start) {
        emit(out, seg_idx, seg_idx + 1, run_width, 0, sub_start);
    }
}

void linebreak_run(LineList *out, const Segment *segs, int seg_count,
                   float max_width, Font *font, const uint8_t *text) {
    out->count = 0;
    if (seg_count == 0) return;

    /* Compute hyphen advance for soft-hyphen support */
    float hyphen_adv = 0.0f;
    Glyph *hg = font_glyph(font, '-');
    if (hg) hyphen_adv = hg->advance;

    int line_start = 0;
    float line_width = 0.0f;
    int last_break = -1;
    int last_break_is_hyphen = 0;

    for (int i = 0; i < seg_count; i++) {
        const Segment *s = &segs[i];

        /* Hard break */
        if (s->kind == SEG_NEWLINE) {
            emit(out, line_start, i, line_width, 0, 0);
            line_start = i + 1;
            line_width = 0.0f;
            last_break = -1;
            last_break_is_hyphen = 0;
            continue;
        }

        /* ANSI: zero width, stays on current line, not a break point */
        if (s->kind == SEG_ANSI) {
            continue;
        }

        /* Soft-hyphen: zero width, but marks a break opportunity */
        if (s->kind == SEG_SOFTHYPHEN) {
            last_break = i + 1;
            last_break_is_hyphen = 1;
            continue;
        }

        /* No wrapping mode */
        if (max_width <= 0) {
            line_width += s->width;
            continue;
        }

        /* Spaces/tabs are breakable */
        if (s->kind == SEG_SPACE || s->kind == SEG_TAB) {
            last_break = i + 1;
            last_break_is_hyphen = 0;
            line_width += s->width;
            continue;
        }

        /* SEG_WORD or SEG_CJK */
        float new_width = line_width + s->width;

        if (new_width > max_width && line_start < i) {
            /* Overflow — break at best point */
            if (last_break > line_start) {
                int break_at_hyphen = last_break_is_hyphen;
                float trimmed = 0.0f;
                for (int j = line_start; j < last_break; j++)
                    trimmed += segs[j].width;
                if (break_at_hyphen) trimmed += hyphen_adv;
                emit(out, line_start, last_break, trimmed, break_at_hyphen, 0);
                line_start = last_break;
                /* Skip leading spaces on new line */
                while (line_start < seg_count &&
                       (segs[line_start].kind == SEG_SPACE || segs[line_start].kind == SEG_TAB))
                    line_start++;
                /* Recalculate width */
                line_width = 0.0f;
                for (int j = line_start; j < i; j++)
                    line_width += segs[j].width;
                new_width = line_width + s->width;
            } else {
                /* No breakable point — break right before this word */
                emit(out, line_start, i, line_width, 0, 0);
                line_start = i;
                line_width = 0.0f;
                new_width = s->width;
            }
            last_break = -1;
            last_break_is_hyphen = 0;
        }

        /* Single word exceeds max_width — character-level overflow */
        if (new_width > max_width && line_start == i && s->width > max_width) {
            overflow_break(out, s, max_width, font, text, i, 0.0f, line_start);
            line_start = i + 1;
            line_width = 0.0f;
            last_break = -1;
            last_break_is_hyphen = 0;
            continue;
        }

        line_width = new_width;
    }

    /* Flush remaining */
    if (line_start < seg_count) {
        emit(out, line_start, seg_count, line_width, 0, 0);
    }
}

void line_list_destroy(LineList *ll) {
    free(ll->lines);
    memset(ll, 0, sizeof(*ll));
}
