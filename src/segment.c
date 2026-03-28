#include "segment.h"
#include "utf8.h"
#include <stdlib.h>
#include <string.h>

static int push_seg(SegmentList *sl, Segment *s) {
    if (sl->count >= sl->capacity) {
        int new_cap = sl->capacity ? sl->capacity * 2 : 256;
        Segment *tmp = realloc(sl->segs, (size_t)new_cap * sizeof(Segment));
        if (!tmp) return -1;
        sl->segs = tmp;
        sl->capacity = new_cap;
    }
    sl->segs[sl->count++] = *s;
    return 0;
}

static SegKind classify(uint32_t cp) {
    if (cp == '\n') return SEG_NEWLINE;
    if (cp == '\t') return SEG_TAB;
    if (cp == ' ' || cp == 0xA0 /* NBSP */) return SEG_SPACE;
    return SEG_WORD;
}

static void analyze_range(SegmentList *out, const uint8_t *text, size_t len, size_t from) {
    const uint8_t *p = text + from;
    const uint8_t *end = text + len;

    while (p < end) {
        const uint8_t *seg_start = p;
        uint32_t cp = utf8_decode(&p, end);
        if (cp == 0) break;

        SegKind kind = classify(cp);

        /* Newlines are always single-segment (hard breaks) */
        if (kind == SEG_NEWLINE) {
            Segment s = { .byte_start = (int)(seg_start - text),
                          .byte_len   = (int)(p - seg_start),
                          .width      = 0.0f,
                          .kind       = SEG_NEWLINE };
            push_seg(out, &s);
            continue;
        }

        /* Merge consecutive codepoints of same kind.
           For SEG_WORD, punctuation sticks to adjacent word text. */
        const uint8_t *seg_end = p;
        while (seg_end < end) {
            const uint8_t *peek = seg_end;
            uint32_t next = utf8_decode(&peek, end);
            if (next == 0) break;
            SegKind nk = classify(next);
            if (nk != kind) break;
            seg_end = peek;
        }

        Segment s = { .byte_start = (int)(seg_start - text),
                      .byte_len   = (int)(seg_end - seg_start),
                      .width      = 0.0f,
                      .kind       = kind };
        push_seg(out, &s);
        p = seg_end;
    }
}

void segment_analyze(SegmentList *out, const uint8_t *text, size_t len) {
    out->count = 0;
    analyze_range(out, text, len, 0);
}

void segment_analyze_append(SegmentList *out, const uint8_t *text, size_t len, size_t from) {
    /* Re-analyze from the start of the last segment to handle boundary merging */
    if (out->count > 0) {
        Segment *last = &out->segs[out->count - 1];
        from = (size_t)last->byte_start;
        out->count--;
    }
    analyze_range(out, text, len, from);
}

static void measure_one(Segment *s, Font *font, const uint8_t *text) {
    if (s->kind == SEG_NEWLINE) {
        s->width = 0.0f;
        return;
    }
    if (s->kind == SEG_TAB) {
        float space_adv = font->glyphs[' '].present ? font->glyphs[' '].advance : font->px_size;
        s->width = space_adv * 4.0f;
        return;
    }

    const uint8_t *p = text + s->byte_start;
    const uint8_t *end = p + s->byte_len;
    float w = 0.0f;
    while (p < end) {
        uint32_t cp = utf8_decode(&p, end);
        if (cp == 0) break;
        Glyph *g = font_glyph(font, cp);
        if (g) w += g->advance;
    }
    s->width = w;
}

void segment_measure(SegmentList *segs, Font *font, const uint8_t *text) {
    segment_measure_from(segs, font, text, 0);
}

void segment_measure_from(SegmentList *segs, Font *font, const uint8_t *text, int from_seg) {
    for (int i = from_seg; i < segs->count; i++)
        measure_one(&segs->segs[i], font, text);
}

void segment_list_destroy(SegmentList *sl) {
    free(sl->segs);
    memset(sl, 0, sizeof(*sl));
}
