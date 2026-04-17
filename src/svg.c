/* svg.c — SVG loader using NanoSVG for the shape layer plus a dedicated
 * <text> extractor for the text layer.
 *
 * NanoSVG ignores <text> elements entirely. For utterance that's a feature,
 * not a bug: we rasterize shapes only, then route every <text> through the
 * existing SDF glyph pipeline so labels stay vector-crisp at any zoom.
 *
 * The text extractor is deliberately small — it scans for <text ...> ... </text>
 * ranges and pulls out only the attributes we need (x, y, font-size, fill,
 * text-anchor, transform, plus enclosing <a xlink:href>). Graphviz output is
 * regular and well-formed; full XML parsing is overkill. */

#define _POSIX_C_SOURCE 200809L

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#include "svg.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- detection */

int svg_detect(const unsigned char *buf, int len) {
    if (!buf || len <= 0) return 0;
    /* Skip leading whitespace / BOM. */
    int i = 0;
    if (len >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) i = 3;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) i++;
    if (i >= len) return 0;
    /* Sniff the first ~256 bytes for "<svg". */
    int end = i + 256 < len ? i + 256 : len;
    for (int j = i; j + 3 < end; j++) {
        if (buf[j] == '<' && (buf[j+1] == 's' || buf[j+1] == 'S') &&
            (buf[j+2] == 'v' || buf[j+2] == 'V') &&
            (buf[j+3] == 'g' || buf[j+3] == 'G')) {
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- storage */

void svg_text_list_destroy(SvgTextList *tl) {
    if (!tl) return;
    for (int i = 0; i < tl->count; i++) {
        free(tl->items[i].utf8);
        free(tl->items[i].href);
    }
    free(tl->items);
    memset(tl, 0, sizeof(*tl));
}

/* ---------------------------------------------------------------- loader */

int svg_load(const char *path, int target_px_w,
             GLuint *out_tex, int *out_tex_w, int *out_tex_h,
             SvgTextList *out_texts,
             float *out_svg_w, float *out_svg_h) {
    (void)out_texts;  /* text extraction arrives in commit 2 */

    if (!path || !out_tex) return -1;

    /* nsvgParseFromFile resolves units against a DPI. 96 matches CSS pixel
     * convention and what graphviz and most SVG authors assume. */
    NSVGimage *img = nsvgParseFromFile(path, "px", 96.0f);
    if (!img) {
        fprintf(stderr, "utterance: svg parse failed: %s\n", path);
        return -1;
    }
    if (img->width <= 0 || img->height <= 0) {
        fprintf(stderr, "utterance: svg has no dimensions: %s\n", path);
        nsvgDelete(img);
        return -1;
    }

    /* Rasterize shape layer at target resolution. */
    if (target_px_w <= 0) target_px_w = 2048;
    float scale = target_px_w / img->width;
    int W = target_px_w;
    int H = (int)(img->height * scale + 0.5f);
    if (H <= 0) H = 1;

    unsigned char *pixels = malloc((size_t)W * H * 4);
    if (!pixels) {
        nsvgDelete(img);
        return -1;
    }
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) {
        free(pixels);
        nsvgDelete(img);
        return -1;
    }
    nsvgRasterize(rast, img, 0, 0, scale, pixels, W, H, W * 4);
    nsvgDeleteRasterizer(rast);

    /* Flip Y — OpenGL expects bottom-up textures (matches image.c convention). */
    unsigned char *flipped = malloc((size_t)W * H * 4);
    if (flipped) {
        for (int row = 0; row < H; row++)
            memcpy(flipped + row * W * 4, pixels + (H - 1 - row) * W * 4, (size_t)W * 4);
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 flipped ? flipped : pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    free(flipped);
    free(pixels);

    *out_tex = tex;
    if (out_tex_w) *out_tex_w = W;
    if (out_tex_h) *out_tex_h = H;
    if (out_svg_w) *out_svg_w = img->width;
    if (out_svg_h) *out_svg_h = img->height;

    fprintf(stderr, "utterance: loaded svg %s (viewbox %.0fx%.0f, raster %dx%d)\n",
            path, img->width, img->height, W, H);

    nsvgDelete(img);
    return 0;
}
