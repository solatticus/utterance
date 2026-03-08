#ifndef UTF8_H
#define UTF8_H

#include <stdint.h>

static inline uint32_t utf8_decode(const uint8_t **p, const uint8_t *end) {
    if (*p >= end) return 0;
    uint32_t c = **p;
    int len;
    if      (c < 0x80) { (*p)++; return c; }
    else if (c < 0xC0) { (*p)++; return 0xFFFD; }
    else if (c < 0xE0) { len = 2; c &= 0x1F; }
    else if (c < 0xF0) { len = 3; c &= 0x0F; }
    else if (c < 0xF8) { len = 4; c &= 0x07; }
    else               { (*p)++; return 0xFFFD; }
    for (int i = 1; i < len; i++) {
        if (*p + i >= end || ((*p)[i] & 0xC0) != 0x80) { *p += i; return 0xFFFD; }
        c = (c << 6) | ((*p)[i] & 0x3F);
    }
    *p += len;
    return c;
}

#endif
