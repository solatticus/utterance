#ifndef SVG_H
#define SVG_H

#include "gl_loader.h"

/* One parsed <text> run from an SVG. Populated by svg_load; rendered by the
 * caller through the existing SDF glyph pipeline. */
typedef struct {
    char  *utf8;          /* owned — UTF-8 text content of the run */
    float  x, y;          /* position in SVG user units (viewBox space) */
    float  font_size_px;  /* resolved font-size in pixels */
    float  r, g, b, a;    /* fill color, 0..1 */
    int    anchor;        /* 0=start, 1=middle, 2=end */
    float  rotate_deg;    /* rotation from transform="rotate(a cx cy)"; 0 default */
    float  rot_cx, rot_cy;
    char  *href;          /* owned, or NULL — enclosing <a xlink:href="..."> */
} SvgText;

typedef struct {
    SvgText *items;
    int      count;
    int      capacity;
} SvgTextList;

/* Load an SVG file. On success:
 *  - *out_tex owns a GL_RGBA texture sized out_tex_w × out_tex_h containing
 *    the rasterized shape layer (no text).
 *  - out_texts is populated with parsed <text> runs (caller keeps ownership).
 *  - *out_svg_w / *out_svg_h are the SVG viewBox dimensions (for coord mapping).
 *
 * target_px_w controls raster resolution — shapes are rasterized at that width
 * preserving aspect ratio. Pass 0 for a sensible default.
 *
 * Returns 0 on success, nonzero on failure. On failure no GL resource is
 * allocated and out_texts is left empty. */
int  svg_load(const char *path, int target_px_w,
              GLuint *out_tex, int *out_tex_w, int *out_tex_h,
              SvgTextList *out_texts,
              float *out_svg_w, float *out_svg_h);

/* Detect whether a file looks like SVG from a sniff buffer. Returns 1 if it
 * starts with an XML prolog declaring SVG, or an <svg ...> root. */
int  svg_detect(const unsigned char *buf, int len);

void svg_text_list_destroy(SvgTextList *tl);

#endif
