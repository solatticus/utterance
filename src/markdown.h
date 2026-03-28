#ifndef MARKDOWN_H
#define MARKDOWN_H

#include <stdint.h>
#include <stddef.h>

/* Image reference found during markdown parsing */
typedef struct {
    char *alt;
    char *path;
    int   placeholder;
} MdImage;

typedef struct {
    MdImage *images;
    int      count;
    int      capacity;
} MdImageList;

/* Clickable link found during markdown parsing */
typedef struct {
    char *url;
    int   start_offset;
    int   end_offset;
} MdLink;

typedef struct {
    MdLink *links;
    int     count;
    int     capacity;
} MdLinkList;

/* Code block region for background rendering */
typedef struct {
    int start_offset;
    int end_offset;
} MdCodeBlock;

typedef struct {
    MdCodeBlock *blocks;
    int          count;
    int          capacity;
} MdCodeBlockList;

uint8_t *markdown_to_ansi(const uint8_t *src, size_t src_len, size_t *out_len,
                          MdImageList *img_out, MdLinkList *link_out,
                          MdCodeBlockList *code_out);

int markdown_detect(const uint8_t *src, size_t len);

void md_image_list_destroy(MdImageList *il);
void md_link_list_destroy(MdLinkList *ll);
void md_codeblock_list_destroy(MdCodeBlockList *cl);

#endif
