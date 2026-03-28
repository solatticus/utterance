#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#include "stb_image.h"
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Image quad shader --- */

static const char *img_vert_src =
    "#version 330 core\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "layout(location = 1) in vec2 a_uv;\n"
    "uniform mat4 u_mvp;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "}\n";

static const char *img_frag_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 frag_color;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "    frag_color = texture(u_tex, v_uv);\n"
    "}\n";

static GLuint img_program = 0;
static GLint  img_loc_mvp, img_loc_tex;
static GLuint img_vao, img_vbo;

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "utterance: img shader error: %s\n", log);
    }
    return s;
}

void image_init_renderer(void) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, img_vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, img_frag_src);
    img_program = glCreateProgram();
    glAttachShader(img_program, vs);
    glAttachShader(img_program, fs);
    glLinkProgram(img_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    img_loc_mvp = glGetUniformLocation(img_program, "u_mvp");
    img_loc_tex = glGetUniformLocation(img_program, "u_tex");

    glGenVertexArrays(1, &img_vao);
    glBindVertexArray(img_vao);
    glGenBuffers(1, &img_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, img_vbo);
    /* 6 verts * 5 floats (pos3 + uv2) — reused for each image */
    glBufferData(GL_ARRAY_BUFFER, 6 * 5 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

/* Resolve path relative to base_dir */
static char *resolve_path(const char *path, const char *base_dir) {
    if (!path || !path[0]) return NULL;
    /* Absolute path or URL — use as-is */
    if (path[0] == '/' || strstr(path, "://")) return strdup(path);
    if (!base_dir) return strdup(path);

    size_t blen = strlen(base_dir);
    size_t plen = strlen(path);
    char *full = malloc(blen + 1 + plen + 1);
    if (!full) return NULL;
    memcpy(full, base_dir, blen);
    if (blen > 0 && base_dir[blen - 1] != '/') full[blen++] = '/';
    memcpy(full + blen, path, plen);
    full[blen + plen] = '\0';
    return full;
}

int image_load(ImageList *il, const char *path, const char *base_dir) {
    char *resolved = resolve_path(path, base_dir);
    if (!resolved) return -1;

    /* Fetch remote URLs with curl → temp file */
    char *tmp_path = NULL;
    if (strstr(resolved, "://")) {
        tmp_path = strdup("/tmp/utterance-img-XXXXXX");
        int fd = mkstemp(tmp_path);
        if (fd < 0) {
            fprintf(stderr, "utterance: can't create temp file for image\n");
            free(resolved); free(tmp_path);
            return -1;
        }
        close(fd);
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "curl -sL -o '%s' '%s' 2>/dev/null", tmp_path, resolved);
        int ret = system(cmd);
        if (ret != 0) {
            fprintf(stderr, "utterance: failed to fetch image: %s\n", resolved);
            unlink(tmp_path);
            free(resolved); free(tmp_path);
            return -1;
        }
        fprintf(stderr, "utterance: fetched remote image: %s\n", resolved);
        free(resolved);
        resolved = tmp_path;
        tmp_path = NULL;
    }

    int is_temp = (strncmp(resolved, "/tmp/utterance-img-", 19) == 0);

    int w, h, channels;
    unsigned char *data = stbi_load(resolved, &w, &h, &channels, 4); /* force RGBA */
    if (!data) {
        fprintf(stderr, "utterance: can't load image: %s\n", resolved);
        if (is_temp) unlink(resolved);
        free(resolved);
        return -1;
    }
    if (is_temp) unlink(resolved);
    free(resolved);

    /* Create GL texture */
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    /* Flip Y — stb loads top-down, OpenGL expects bottom-up */
    unsigned char *flipped = malloc((size_t)w * h * 4);
    if (flipped) {
        for (int row = 0; row < h; row++)
            memcpy(flipped + row * w * 4, data + (h - 1 - row) * w * 4, (size_t)w * 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, flipped);
        free(flipped);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);

    /* Add to list */
    if (il->count >= il->capacity) {
        int new_cap = il->capacity ? il->capacity * 2 : 8;
        Image *tmp = realloc(il->items, (size_t)new_cap * sizeof(Image));
        if (!tmp) { glDeleteTextures(1, &tex); return -1; }
        il->items = tmp;
        il->capacity = new_cap;
    }

    int idx = il->count++;
    Image *img = &il->items[idx];
    img->texture = tex;
    img->width = w;
    img->height = h;
    img->world_x = 0;
    img->world_y = 0;
    img->world_w = 0;
    img->world_h = 0;
    img->placed = 0;

    fprintf(stderr, "utterance: loaded image %dx%d\n", w, h);
    return idx;
}

void image_render(const ImageList *il, const float mvp[16]) {
    if (!il || il->count == 0 || !img_program) return;

    glUseProgram(img_program);
    glUniformMatrix4fv(img_loc_mvp, 1, GL_FALSE, mvp);
    glUniform1i(img_loc_tex, 0);
    glActiveTexture(GL_TEXTURE0);

    for (int i = 0; i < il->count; i++) {
        const Image *img = &il->items[i];
        if (!img->placed || !img->texture) continue;

        float x0 = img->world_x, y0 = img->world_y;
        float x1 = x0 + img->world_w, y1 = y0 + img->world_h;
        float z = -0.01f; /* slightly behind text plane */

        float verts[30] = {
            x0, y1, z,  0, 1,
            x0, y0, z,  0, 0,
            x1, y0, z,  1, 0,
            x0, y1, z,  0, 1,
            x1, y0, z,  1, 0,
            x1, y1, z,  1, 1,
        };

        glBindBuffer(GL_ARRAY_BUFFER, img_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

        glBindTexture(GL_TEXTURE_2D, img->texture);
        glBindVertexArray(img_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
    }
}

void image_destroy_renderer(void) {
    if (img_vbo) glDeleteBuffers(1, &img_vbo);
    if (img_vao) glDeleteVertexArrays(1, &img_vao);
    if (img_program) glDeleteProgram(img_program);
}

void image_list_destroy(ImageList *il) {
    for (int i = 0; i < il->count; i++) {
        if (il->items[i].texture)
            glDeleteTextures(1, &il->items[i].texture);
    }
    free(il->items);
    memset(il, 0, sizeof(*il));
}
