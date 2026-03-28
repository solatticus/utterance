#ifndef MARKDOWN_H
#define MARKDOWN_H

#include <stdint.h>
#include <stddef.h>

/* Image reference found during markdown parsing */
typedef struct {
    char *alt;          /* alt text (owned) */
    char *path;         /* image path or URL (owned) */
    int   placeholder;  /* byte offset in output where image placeholder was inserted */
} MdImage;

typedef struct {
    MdImage *images;
    int      count;
    int      capacity;
} MdImageList;

/* Transform markdown source into ANSI-colored UTF-8.
   Allocates a new buffer (caller frees). Returns NULL on failure.
   If img_out is non-NULL, populates it with image references found. */
uint8_t *markdown_to_ansi(const uint8_t *src, size_t src_len, size_t *out_len,
                          MdImageList *img_out);

/* Detect if buffer looks like markdown (heuristic). */
int markdown_detect(const uint8_t *src, size_t len);

void md_image_list_destroy(MdImageList *il);

#endif
