#ifndef TEXT_H
#define TEXT_H

#include <stdint.h>
#include <stddef.h>
#include "font.h"

typedef struct {
    float x, y, z;
    float w, h;
    float s0, t0, s1, t1;
} GlyphInstance;

typedef struct {
    GlyphInstance *instances;
    int count;
    int capacity;
    unsigned int vao, vbo;
} TextMesh;

void text_layout(TextMesh *mesh, Font *font, const uint8_t *text, size_t len, float wrap_width);
void text_upload(TextMesh *mesh);
void text_destroy(TextMesh *mesh);

#endif
