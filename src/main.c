#define GL_LOADER_IMPLEMENTATION
#include "gl_loader.h"
#include "window.h"
#include "camera.h"
#include "font.h"
#include "text.h"
#include "render.h"
#include "fx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOUSE_SENSITIVITY 0.002f

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "utterance: can't open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(*len);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, *len, f) != *len) { free(buf); fclose(f); return NULL; }
    fclose(f);
    return buf;
}

static uint8_t *read_stdin(size_t *len) {
    if (isatty(0)) return NULL;
    size_t cap = 1 << 20;
    uint8_t *buf = malloc(cap);
    if (!buf) return NULL;
    size_t n = 0;
    ssize_t r;
    while ((r = read(0, buf + n, cap - n)) > 0) {
        n += r;
        if (n == cap) {
            cap *= 2;
            uint8_t *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); *len = 0; return NULL; }
            buf = tmp;
        }
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
        free(text);
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
    if (font_load(&font, font_path, 48.0f) < 0) {
        free(text);
        window_destroy(&win);
        return 1;
    }
    fprintf(stderr, "utterance: font loaded in %.2fs\n", glfwGetTime() - t0);

    /* 4. Layout */
    TextMesh mesh;
    text_layout(&mesh, &font, text, text_len, 0.0f); /* no wrap for now */
    text_upload(&mesh);

    /* 5. Renderer */
    render_init();
    fx_init(win.width, win.height);

    /* 6. Camera — start at top-left of text, pulled back */
    Camera cam;
    camera_init(&cam, 0.0f, 0.0f, 200.0f);

    /* FPS counter */
    int fps_frames = 0;
    double fps_time = glfwGetTime();
    char fps_str[16] = "0 fps";
    int fps_len = 5;

    static const float text_color[3]      = {0.85f, 0.9f, 0.95f};
    static const float fps_color[3]       = {0.4f, 0.4f, 0.45f};
    static const float crosshair_color[3] = {0.6f, 0.6f, 0.65f};
    float speed_mult = 1.0f;
    float blink_dist = 200.0f;  /* base blink distance, scales with speed_mult */
    int prev_plus = GLFW_RELEASE, prev_minus = GLFW_RELEASE;
    int prev_blink = GLFW_RELEASE;
    int prev_bslash = GLFW_RELEASE;
    int prev_fw = 0, prev_fh = 0;

    /* Blink animation state */
    int blink_phase = 0;        /* 0=idle, 1=forward, 2=bounce */
    float blink_origin[3] = {0};
    float blink_target[3] = {0};
    float blink_t = 0.0f;
    int blink_has_text = 0;

    /* Main loop */
    while (!window_should_close(&win)) {
        window_poll(&win);

        /* Speed adjustment: +/- keys, edge-triggered */
        int cur_plus = glfwGetKey(win.handle, GLFW_KEY_EQUAL);   /* =/+ key */
        int cur_minus = glfwGetKey(win.handle, GLFW_KEY_MINUS);
        if (cur_plus == GLFW_PRESS && prev_plus == GLFW_RELEASE)
            speed_mult *= 1.25f;
        if (cur_minus == GLFW_PRESS && prev_minus == GLFW_RELEASE)
            speed_mult *= 0.80f;  /* inverse of 1.25 */
        prev_plus = cur_plus;
        prev_minus = cur_minus;

        /* Blink FX: \ key cycles effect */
        int cur_bslash = glfwGetKey(win.handle, GLFW_KEY_BACKSLASH);
        if (cur_bslash == GLFW_PRESS && prev_bslash == GLFW_RELEASE)
            fx_cycle();
        prev_bslash = cur_bslash;

        /* Input */
        float speed = 100.0f * speed_mult * win.dt;
        if (glfwGetKey(win.handle, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
            speed *= 5.0f;

        float fwd[3], right[3];
        camera_forward(&cam, fwd);
        camera_right(&cam, right);

        float dx = 0, dy = 0, dz = 0;
        float fspeed = speed * 4.0f;   /* forward: 4x base */
        float bspeed = speed * 2.5f;   /* backward: 2.5x base */
        if (glfwGetKey(win.handle, GLFW_KEY_W) == GLFW_PRESS) { dx += fwd[0]*fspeed; dy += fwd[1]*fspeed; dz += fwd[2]*fspeed; }
        if (glfwGetKey(win.handle, GLFW_KEY_S) == GLFW_PRESS) { dx -= fwd[0]*bspeed; dy -= fwd[1]*bspeed; dz -= fwd[2]*bspeed; }
        float sspeed = speed * 3.0f;   /* strafe/vertical: 3x base */
        if (glfwGetKey(win.handle, GLFW_KEY_A) == GLFW_PRESS) { dx -= right[0]*sspeed; dy -= right[1]*sspeed; dz -= right[2]*sspeed; }
        if (glfwGetKey(win.handle, GLFW_KEY_D) == GLFW_PRESS) { dx += right[0]*sspeed; dy += right[1]*sspeed; dz += right[2]*sspeed; }
        if (glfwGetKey(win.handle, GLFW_KEY_SPACE) == GLFW_PRESS) dy += sspeed;
        if (glfwGetKey(win.handle, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) dy -= sspeed;

        /* Blink: left-click or F = animated raycast flight */
        int cur_blink = glfwGetKey(win.handle, GLFW_KEY_F);
        int do_blink = win.blink_triggered ||
                       (cur_blink == GLFW_PRESS && prev_blink == GLFW_RELEASE);
        if (do_blink && blink_phase == 0) {
            blink_origin[0] = cam.pos[0];
            blink_origin[1] = cam.pos[1];
            blink_origin[2] = cam.pos[2];

            float t = (fwd[2] != 0.0f) ? -cam.pos[2] / fwd[2] : -1.0f;
            if (t > 0.0f) {
                float hit_x = cam.pos[0] + t * fwd[0];
                float hit_y = cam.pos[1] + t * fwd[1];
                float standoff = 80.0f;
                blink_target[0] = hit_x - fwd[0] * standoff;
                blink_target[1] = hit_y - fwd[1] * standoff;
                blink_target[2] = 0.0f  - fwd[2] * standoff;
                blink_has_text = text_hit_test(&mesh, hit_x, hit_y, 60.0f);
            } else {
                float dist = blink_dist * speed_mult;
                blink_target[0] = cam.pos[0] + fwd[0] * dist;
                blink_target[1] = cam.pos[1] + fwd[1] * dist;
                blink_target[2] = cam.pos[2] + fwd[2] * dist;
                blink_has_text = 0;
            }
            blink_phase = 1;
            blink_t = 0.0f;
        }
        prev_blink = cur_blink;

        /* Animate blink */
        if (blink_phase > 0) {
            float dur = (blink_phase == 1) ? 0.25f : 0.35f;
            blink_t += win.dt / dur;
            if (blink_t > 1.0f) blink_t = 1.0f;

            /* Ease-out cubic (forward), ease-in-out (bounce) */
            float e;
            if (blink_phase == 1) {
                float inv = 1.0f - blink_t;
                e = 1.0f - inv * inv * inv;
            } else {
                e = blink_t * blink_t * (3.0f - 2.0f * blink_t);
            }

            float src[3], dst[3];
            if (blink_phase == 1) {
                src[0] = blink_origin[0]; src[1] = blink_origin[1]; src[2] = blink_origin[2];
                dst[0] = blink_target[0]; dst[1] = blink_target[1]; dst[2] = blink_target[2];
            } else {
                src[0] = blink_target[0]; src[1] = blink_target[1]; src[2] = blink_target[2];
                dst[0] = blink_origin[0]; dst[1] = blink_origin[1]; dst[2] = blink_origin[2];
            }

            cam.pos[0] = src[0] + (dst[0] - src[0]) * e;
            cam.pos[1] = src[1] + (dst[1] - src[1]) * e;
            cam.pos[2] = src[2] + (dst[2] - src[2]) * e;

            /* Suppress WASD during animation */
            dx = 0; dy = 0; dz = 0;

            if (blink_t >= 1.0f) {
                if (blink_phase == 1 && !blink_has_text) {
                    /* Overshoot — bounce back */
                    blink_phase = 2;
                    blink_t = 0.0f;
                } else {
                    blink_phase = 0;
                }
            }
        }

        float dyaw = 0, dpitch = 0;
        if (win.mouse_captured) {
            dyaw = (float)(win.mouse_x - win.last_mouse_x) * MOUSE_SENSITIVITY;
            dpitch = (float)(win.mouse_y - win.last_mouse_y) * MOUSE_SENSITIVITY;
        }

        camera_update(&cam, dx, dy, dz, dyaw, dpitch);
        cam.aspect = (float)win.width / (float)win.height;

        float mvp[16];
        camera_mvp(&cam, mvp);

        /* Render */
        if (win.width != prev_fw || win.height != prev_fh) {
            fx_resize(win.width, win.height);
            prev_fw = win.width;
            prev_fh = win.height;
        }

        /* FX progress: 0→1 over the entire blink animation */
        float fx_progress = 0.0f;
        if (blink_phase == 1)
            fx_progress = blink_t;       /* forward: 0→1 */
        else if (blink_phase == 2)
            fx_progress = 1.0f - blink_t * 0.5f; /* bounce: 1→0.5, fading out */

        fx_begin(fx_progress);
        glViewport(0, 0, win.width, win.height);
        glClearColor(0.02f, 0.02f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        render_text(&mesh, &font, mvp, text_color);
        fx_end(win.width, win.height, fx_progress);

        /* FPS overlay — update text once per second */
        fps_frames++;
        double now = glfwGetTime();
        if (now - fps_time >= 1.0) {
            fps_len = snprintf(fps_str, sizeof(fps_str), "%d fps", fps_frames);
            fps_frames = 0;
            fps_time = now;
        }
        float fps_scale = 0.4f;
        float space_advance = font.glyphs[' '].present ? font.glyphs[' '].advance : font.px_size;
        float fps_x = win.width - (float)fps_len * space_advance * fps_scale - 10.0f;
        render_overlay(&font, fps_str, fps_x, 10.0f, fps_scale, win.width, win.height, fps_color);

        /* Crosshair — small dot at screen center */
        float ch_scale = 0.25f;
        Glyph *ch_g = font_glyph(&font, '+');
        if (ch_g) {
            float ch_x = (float)win.width * 0.5f - ch_g->advance * ch_scale * 0.5f;
            float ch_y = (float)win.height * 0.5f - (font.ascent + ch_g->y_off + ch_g->height * 0.5f) * ch_scale;
            render_overlay(&font, "+", ch_x, ch_y, ch_scale, win.width, win.height, crosshair_color);
        }

        window_swap(&win);
    }

    /* Cleanup */
    text_destroy(&mesh);
    font_destroy(&font);
    fx_destroy();
    render_destroy();
    window_destroy(&win);
    free(text);
    return 0;
}
