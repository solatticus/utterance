#ifndef IMAGE_H
#define IMAGE_H

#include "gl_loader.h"
#include "svg.h"
#include "text.h"

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
    TextMesh    svg_text_mesh; /* SDF glyph mesh built from texts, in world space */
    SvgLinkList svg_links;     /* glyph-range → URL map for Ctrl+click */
} Image;

typedef struct {
    Image *items;
    int    count;
    int    capacity;
} ImageList;

/* Load an image file into a GL texture. Returns index in list, -1 on failure.
 * svg_target_px_w: rasterization width for SVG sources (0 = default 2048).
 * Higher values give a crisper raster when the SVG is the top-level view. */
int image_load(ImageList *il, const char *path, const char *base_dir,
               int svg_target_px_w);

/* Render all placed images using a textured quad shader. */
void image_render(const ImageList *il, const float mvp[16]);

void image_init_renderer(void);
void image_destroy_renderer(void);
void image_list_destroy(ImageList *il);

#endif
