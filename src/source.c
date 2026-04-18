#include "source.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef _WIN32
#  include <io.h>
#  include <windows.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#endif

int source_from_file(Source *s, const char *path) {
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    s->eof = 1;

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "utterance: can't open %s\n", path); return -1; }

    fseek(f, 0, SEEK_END);
    s->len = ftell(f);
    fseek(f, 0, SEEK_SET);
    s->cap = s->len;
    s->buf = malloc(s->cap);
    if (!s->buf) { fclose(f); return -1; }
    if (fread(s->buf, 1, s->len, f) != s->len) { free(s->buf); fclose(f); return -1; }
    fclose(f);
    return 0;
}

int source_from_fd(Source *s, int fd) {
    memset(s, 0, sizeof(*s));
    s->fd = fd;
    s->eof = 0;
    s->cap = 1 << 16;
    s->buf = malloc(s->cap);
    if (!s->buf) return -1;

#ifdef _WIN32
    /* On Windows, stdin pipe handle is used directly via Win32 API in source_poll */
    (void)fd;
#else
    /* Set non-blocking */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    return 0;
}

size_t source_poll(Source *s) {
    if (s->fd < 0 || s->eof) return 0;

    /* Grow if needed */
    if (s->len >= s->cap) {
        size_t new_cap = s->cap * 2;
        uint8_t *tmp = realloc(s->buf, new_cap);
        if (!tmp) return 0;
        s->buf = tmp;
        s->cap = new_cap;
    }

#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD avail = 0;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
        s->eof = 1;
        return 0;
    }
    if (avail == 0) return 0;
    DWORD to_read = (DWORD)(s->cap - s->len);
    if (to_read > avail) to_read = avail;
    DWORD nread = 0;
    if (!ReadFile(h, s->buf + s->len, to_read, &nread, NULL) || nread == 0) {
        s->eof = 1;
        return 0;
    }
    s->len += nread;
    return (size_t)nread;
#else
    ssize_t r = read(s->fd, s->buf + s->len, s->cap - s->len);
    if (r > 0) {
        s->len += r;
        return (size_t)r;
    }
    if (r == 0) {
        s->eof = 1;
    }
    /* EAGAIN/EWOULDBLOCK = nothing available, not an error */
    return 0;
#endif
}

void source_destroy(Source *s) {
    free(s->buf);
    memset(s, 0, sizeof(*s));
    s->fd = -1;
}
