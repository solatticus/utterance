#ifndef RENDER_H
#define RENDER_H

#include "text.h"
#include "font.h"

void render_init(void);
void render_text(const TextMesh *mesh, const Font *font, const float mvp[16], const float color[3]);
void render_overlay(Font *font, const char *str, float x, float y, float scale, int win_w, int win_h, const float color[3]);
void render_destroy(void);

#endif
