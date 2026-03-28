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

static int is_cjk(uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) ||   /* CJK radicals, kangxi, ideographs */
           (cp >= 0xF900 && cp <= 0xFAFF) ||   /* CJK compatibility ideographs */
           (cp >= 0xFE30 && cp <= 0xFE4F) ||   /* CJK compatibility forms */
           (cp >= 0x3000 && cp <= 0x303F) ||   /* CJK symbols and punctuation */
           (cp >= 0x3040 && cp <= 0x309F) ||   /* Hiragana */
           (cp >= 0x30A0 && cp <= 0x30FF) ||   /* Katakana */
           (cp >= 0x31F0 && cp <= 0x31FF) ||   /* Katakana phonetic extensions */
           (cp >= 0xFF00 && cp <= 0xFFEF);     /* Fullwidth forms */
}

static SegKind classify(uint32_t cp) {
    if (cp == '\n') return SEG_NEWLINE;
    if (cp == '\t') return SEG_TAB;
    if (cp == ' ' || cp == 0xA0) return SEG_SPACE;
    if (cp == 0xAD) return SEG_SOFTHYPHEN;
    if (is_cjk(cp)) return SEG_CJK;
    return SEG_WORD;
}

static void analyze_range(SegmentList *out, const uint8_t *text, size_t len, size_t from) {
    const uint8_t *p = text + from;
    const uint8_t *end = text + len;

    while (p < end) {
        const uint8_t *seg_start = p;
        uint32_t cp = utf8_decode(&p, end);
        if (cp == 0) break;

        /* ANSI escape sequences */
        if (cp == 0x1B && p < end) {
            if (*p == '[') {
                /* CSI: ESC [ params final_byte */
                p++;
                while (p < end && (*p < 0x40 || *p > 0x7E)) p++;
                if (p < end) p++;
            } else if (*p == ']') {
                /* OSC: ESC ] ... BEL/ST */
                p++;
                while (p < end && *p != 0x07) {
                    if (*p == 0x1B && p + 1 < end && p[1] == '\\') { p += 2; break; }
                    p++;
                }
                if (p < end && *p == 0x07) p++;
            } else {
                /* Other escape: ESC + single char */
                p++;
            }
            Segment s = { .byte_start = (int)(seg_start - text),
                          .byte_len   = (int)(p - seg_start),
                          .width      = 0.0f,
                          .kind       = SEG_ANSI };
            push_seg(out, &s);
            continue;
        }

        SegKind kind = classify(cp);

        /* Single-segment kinds: newline, CJK, soft-hyphen — never merge */
        if (kind == SEG_NEWLINE || kind == SEG_CJK || kind == SEG_SOFTHYPHEN) {
            Segment s = { .byte_start = (int)(seg_start - text),
                          .byte_len   = (int)(p - seg_start),
                          .width      = 0.0f,
                          .kind       = kind };
            push_seg(out, &s);
            continue;
        }

        /* Merge consecutive same-kind codepoints (WORD, SPACE, TAB) */
        const uint8_t *seg_end = p;
        while (seg_end < end) {
            const uint8_t *peek = seg_end;
            uint32_t next = utf8_decode(&peek, end);
            if (next == 0) break;
            SegKind nk = classify(next);
            if (nk != kind) break;
            /* Don't merge into non-mergeable kinds */
            if (nk == SEG_CJK || nk == SEG_SOFTHYPHEN) break;
            /* Don't merge across ANSI escapes */
            if (next == 0x1B) break;
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
    if (out->count > 0) {
        Segment *last = &out->segs[out->count - 1];
        from = (size_t)last->byte_start;
        out->count--;
    }
    analyze_range(out, text, len, from);
}

static void measure_one(Segment *s, Font *font, const uint8_t *text) {
    if (s->kind == SEG_NEWLINE || s->kind == SEG_ANSI || s->kind == SEG_SOFTHYPHEN) {
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
