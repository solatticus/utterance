#ifndef SVG_H
#define SVG_H

#include "gl_loader.h"
#include "scene.h"
#include "text.h"

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

/* A hit range in a built SDF text mesh that maps back to an <a xlink:href="...">
 * URL. When the user Ctrl+clicks a glyph whose index falls in [glyph_start,
 * glyph_end), href is the target to open. */
typedef struct {
    int   glyph_start;
    int   glyph_end;
    char *href;   /* owned */
} SvgLink;

typedef struct {
    SvgLink *items;
    int      count;
    int      capacity;
} SvgLinkList;

void svg_link_list_destroy(SvgLinkList *ll);

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

/* Build a Scene from an SVG file. Shapes get flattened + triangulated into
 * SceneMesh nodes with world-space coords (Y flipped from SVG's y-down to
 * the world's y-up convention). Text runs are extracted into out_texts (same
 * structure svg_load produced) for the caller to feed into svg_build_text_mesh.
 *
 * Scratch buffers are reused across shapes so allocations are bounded by the
 * high-water mark of any single shape, not the total shape count. The caller
 * owns out_scene and out_texts and must destroy them when done.
 *
 * Returns 0 on success, nonzero on parse failure. On failure no scene nodes
 * are appended and out_texts is left as-is. */
int  svg_build_scene(const char *path,
                     float extrusion_depth,
                     Scene *out_scene,
                     SvgTextList *out_texts,
                     float *out_svg_w, float *out_svg_h);

void svg_text_list_destroy(SvgTextList *tl);

/* Build an SDF glyph mesh (in text.h terms) from an SvgTextList, mapping
 * each text run's SVG-space anchor point into world space using the image's
 * bounding box. The mesh is populated with positioned GlyphInstances and the
 * caller must run text_upload on it afterwards. */
void svg_build_text_mesh(TextMesh *mesh, SvgLinkList *links, Font *font,
                         const SvgTextList *texts,
                         float svg_w, float svg_h,
                         float wx, float wy, float ww, float wh,
                         float wz);

#endif
