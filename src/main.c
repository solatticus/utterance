#define GL_LOADER_IMPLEMENTATION
#include "gl_loader.h"
#include "window.h"
#include "camera.h"
#include "font.h"
#include "text.h"
#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "utterance: can't open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(*len);
    if (fread(buf, 1, *len, f) != *len) { free(buf); fclose(f); return NULL; }
    fclose(f);
    return buf;
}

static uint8_t *read_stdin(size_t *len) {
    if (isatty(0)) return NULL;
    size_t cap = 1 << 20;
    uint8_t *buf = malloc(cap);
    size_t n = 0;
    ssize_t r;
    while ((r = read(0, buf + n, cap - n)) > 0) {
        n += r;
        if (n == cap) { cap *= 2; buf = realloc(buf, cap); }
    }
    *len = n;
    return buf;
}

int main(int argc, char **argv) {
    /* 1. Read input */
    uint8_t *text = NULL;
    size_t text_len = 0;

    const char *font_path = "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            font_path = argv[++i];
        } else if (!text) {
            text = read_file(argv[i], &text_len);
        }
    }

    if (!text) text = read_stdin(&text_len);
    if (!text || text_len == 0) {
        fprintf(stderr, "usage: utterance [-f font.ttf] [file]\n       echo 'hello' | utterance\n");
        return 1;
    }

    /* 2. Window + GL — if no display, just be cat */
    Window win;
    if (window_init(&win, 1920, 1080, "utterance") < 0) {
        fwrite(text, 1, text_len, stdout);
        free(text);
        return 0;
    }
    gl_load_all();

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* 3. Font */
    Font font;
    double t0 = glfwGetTime();
    if (font_load(&font, font_path, 48.0f) < 0) return 1;
    fprintf(stderr, "utterance: font loaded in %.2fs\n", glfwGetTime() - t0);

    /* 4. Layout */
    TextMesh mesh;
    text_layout(&mesh, &font, text, text_len, 0.0f); /* no wrap for now */
    text_upload(&mesh);

    /* 5. Renderer */
    render_init();

    /* 6. Camera — start at top-left of text, pulled back */
    Camera cam;
    camera_init(&cam, 0.0f, 0.0f, 200.0f);

    /* FPS counter — update once per second */
    int fps_frames = 0;
    double fps_time = glfwGetTime();
    char fps_str[16] = "0 fps";

    /* Main loop */
    while (!window_should_close(&win)) {
        window_poll(&win);

        /* Input */
        float speed = 100.0f * win.dt;
        if (glfwGetKey(win.handle, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
            speed *= 5.0f;

        float fwd[3], right[3];
        camera_forward(&cam, fwd);
        camera_right(&cam, right);

        float dx = 0, dy = 0, dz = 0;
        if (glfwGetKey(win.handle, GLFW_KEY_W) == GLFW_PRESS) { dx += fwd[0]*speed; dy += fwd[1]*speed; dz += fwd[2]*speed; }
        if (glfwGetKey(win.handle, GLFW_KEY_S) == GLFW_PRESS) { dx -= fwd[0]*speed; dy -= fwd[1]*speed; dz -= fwd[2]*speed; }
        if (glfwGetKey(win.handle, GLFW_KEY_A) == GLFW_PRESS) { dx -= right[0]*speed; dy -= right[1]*speed; dz -= right[2]*speed; }
        if (glfwGetKey(win.handle, GLFW_KEY_D) == GLFW_PRESS) { dx += right[0]*speed; dy += right[1]*speed; dz += right[2]*speed; }
        if (glfwGetKey(win.handle, GLFW_KEY_SPACE) == GLFW_PRESS) dy += speed;
        if (glfwGetKey(win.handle, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) dy -= speed;

        float dyaw = 0, dpitch = 0;
        if (win.mouse_captured) {
            dyaw = (float)(win.mouse_x - win.last_mouse_x) * 0.002f;
            dpitch = (float)(win.mouse_y - win.last_mouse_y) * 0.002f;
        }

        camera_update(&cam, dx, dy, dz, dyaw, dpitch);
        cam.aspect = (float)win.width / (float)win.height;

        float mvp[16];
        camera_mvp(&cam, mvp);

        /* Render */
        glViewport(0, 0, win.width, win.height);
        glClearColor(0.02f, 0.02f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float color[3] = {0.85f, 0.9f, 0.95f};
        render_text(&mesh, &font, mvp, color);

        /* FPS overlay — top right, once per second */
        fps_frames++;
        double now = glfwGetTime();
        if (now - fps_time >= 1.0) {
            snprintf(fps_str, sizeof(fps_str), "%d fps", fps_frames);
            fps_frames = 0;
            fps_time = now;
        }
        float fps_scale = 0.4f;
        float fps_x = win.width - (float)strlen(fps_str) * font.glyphs[' '].advance * fps_scale - 10.0f;
        float fps_color[3] = {0.4f, 0.4f, 0.45f};
        render_overlay(&font, fps_str, fps_x, 10.0f, fps_scale, win.width, win.height, fps_color);

        window_swap(&win);
    }

    /* Cleanup */
    text_destroy(&mesh);
    font_destroy(&font);
    render_destroy();
    window_destroy(&win);
    free(text);
    return 0;
}
