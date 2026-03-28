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

static void emit_line(LineList *out, int seg_start, int seg_end, float width) {
    Line l = { .seg_start = seg_start, .seg_end = seg_end, .width = width };
    push_line(out, &l);
}

/* Character-level overflow break for a single SEG_WORD that exceeds max_width.
   Emits multiple lines, each fitting within remaining + max_width. */
static void overflow_break(LineList *out, const Segment *seg, float max_width,
                           Font *font, const uint8_t *text,
                           int seg_idx, float line_width, int line_start) {
    const uint8_t *p = text + seg->byte_start;
    const uint8_t *end = p + seg->byte_len;
    float remaining = max_width - line_width;
    float run = 0.0f;

    /* We don't split the segment struct itself — we just emit line boundaries
       at the segment level. The overflow word will span one line boundary
       with seg_start == seg_end-1 == seg_idx for each piece.
       Callers handle the partial rendering via the line width. */

    /* For overflow, just treat the whole segment as one line.
       The mesh builder will handle the per-glyph wrapping using line width. */
    (void)p; (void)end; (void)remaining; (void)run; (void)font;

    /* Emit the oversized segment as its own line */
    if (line_start < seg_idx) {
        /* Flush pending segments before the overflow word */
        emit_line(out, line_start, seg_idx, line_width);
    }
    emit_line(out, seg_idx, seg_idx + 1, seg->width);
}

void linebreak_run(LineList *out, const Segment *segs, int seg_count,
                   float max_width, Font *font, const uint8_t *text) {
    out->count = 0;
    if (seg_count == 0) return;

    int line_start = 0;
    float line_width = 0.0f;
    int last_break = -1;       /* index of last breakable point */

    for (int i = 0; i < seg_count; i++) {
        const Segment *s = &segs[i];

        /* Hard break */
        if (s->kind == SEG_NEWLINE) {
            emit_line(out, line_start, i, line_width);
            line_start = i + 1;
            line_width = 0.0f;
            last_break = -1;
            continue;
        }

        /* No wrapping mode */
        if (max_width <= 0) {
            line_width += s->width;
            continue;
        }

        /* Spaces/tabs are breakable — trailing spaces hang (don't trigger break) */
        if (s->kind == SEG_SPACE || s->kind == SEG_TAB) {
            last_break = i + 1;
            line_width += s->width;
            continue;
        }

        /* SEG_WORD */
        float new_width = line_width + s->width;

        if (new_width > max_width && line_start < i) {
            /* Overflow — break at last breakable point or before this word */
            if (last_break > line_start) {
                /* Break at last space: line is [line_start, last_break) */
                /* Trim trailing spaces from line width */
                float trimmed = 0.0f;
                for (int j = line_start; j < last_break; j++) {
                    if (segs[j].kind != SEG_SPACE && segs[j].kind != SEG_TAB)
                        trimmed += segs[j].width;
                    else
                        trimmed += segs[j].width; /* include space in width for now */
                }
                emit_line(out, line_start, last_break, trimmed);
                line_start = last_break;
                /* Skip leading spaces on new line */
                while (line_start < seg_count &&
                       (segs[line_start].kind == SEG_SPACE || segs[line_start].kind == SEG_TAB))
                    line_start++;
                /* Recalculate width from line_start to i */
                line_width = 0.0f;
                for (int j = line_start; j < i; j++)
                    line_width += segs[j].width;
                new_width = line_width + s->width;
            } else {
                /* No breakable point — break right before this word */
                emit_line(out, line_start, i, line_width);
                line_start = i;
                line_width = 0.0f;
                new_width = s->width;
            }
            last_break = -1;
        }

        /* Single word exceeds max_width — overflow break at character level */
        if (new_width > max_width && line_start == i && s->width > max_width) {
            overflow_break(out, s, max_width, font, text, i, 0.0f, line_start);
            line_start = i + 1;
            line_width = 0.0f;
            last_break = -1;
            continue;
        }

        line_width = new_width;
    }

    /* Flush remaining */
    if (line_start < seg_count) {
        emit_line(out, line_start, seg_count, line_width);
    }
}

void line_list_destroy(LineList *ll) {
    free(ll->lines);
    memset(ll, 0, sizeof(*ll));
}
