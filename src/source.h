#ifndef SOURCE_H
#define SOURCE_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    int      fd;     /* -1 = buffer-only (file loaded in full) */
    int      eof;
} Source;

/* Load entire file into source. Sets eof=1. Returns 0 on success. */
int source_from_file(Source *s, const char *path);

/* Attach a file descriptor (e.g. STDIN_FILENO). Sets fd non-blocking. */
int source_from_fd(Source *s, int fd);

/* Non-blocking read. Returns bytes added (0 = nothing available, sets eof on EOF). */
size_t source_poll(Source *s);

void source_destroy(Source *s);

#endif
