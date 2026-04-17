#ifndef IMAGE_H
#define IMAGE_H

#include "gl_loader.h"
#include "svg.h"

typedef struct {
    GLuint texture;
    int    width;
    int    height;
    float  world_x, world_y;   /* position in text world (set during layout) */
    float  world_w, world_h;   /* size in world units */
    int    placed;              /* has been positioned in the text flow */
    /* SVG-specific. For raster sources these stay zero / empty. */
    int         is_svg;
    float       svg_view_w;    /* SVG viewBox width (user units) */
    float       svg_view_h;    /* SVG viewBox height */
    SvgTextList texts;         /* parsed <text> runs — rendered via SDF pipeline */
} Image;

typedef struct {
    Image *items;
    int    count;
    int    capacity;
} ImageList;

/* Load an image file into a GL texture. Returns index in list, -1 on failure. */
int image_load(ImageList *il, const char *path, const char *base_dir);

/* Render all placed images using a textured quad shader. */
void image_render(const ImageList *il, const float mvp[16]);

void image_init_renderer(void);
void image_destroy_renderer(void);
void image_list_destroy(ImageList *il);

#endif
