#define GL_LOADER_IMPLEMENTATION
#include "gl_loader.h"
#include "window.h"
#include "camera.h"
#include "font.h"
#include "text.h"
#include "render.h"
#include "fx.h"
#include "source.h"
#include "markdown.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

#define MOUSE_SENSITIVITY 0.002f

int main(int argc, char **argv) {
    /* 1. Parse args */
    const char *font_path = "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf";
    const char *file_arg = NULL;
    int force_markdown = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            font_path = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0) {
            force_markdown = 1;
        } else if (!file_arg) {
            file_arg = argv[i];
        }
    }

    /* 2. Source */
    Source src;
    int have_source = 0;

    if (file_arg) {
        if (source_from_file(&src, file_arg) == 0)
            have_source = 1;
    }
    if (!have_source && !isatty(0)) {
        /* Read an initial batch blocking, then switch to non-blocking for streaming */
        if (source_from_fd(&src, STDIN_FILENO) == 0) {
            have_source = 1;
            /* Do a blocking drain to get initial content */
            size_t total = 0;
            for (;;) {
                if (src.len >= src.cap) {
                    size_t new_cap = src.cap * 2;
                    uint8_t *tmp = realloc(src.buf, new_cap);
                    if (!tmp) break;
                    src.buf = tmp;
                    src.cap = new_cap;
                }
                /* Temporarily blocking read for initial content */
                int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
                fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
                ssize_t r = read(STDIN_FILENO, src.buf + src.len, src.cap - src.len);
                fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
                if (r <= 0) {
                    if (r == 0) src.eof = 1;
                    break;
                }
                src.len += r;
                total += r;
                /* If we got data and there's a pause, render what we have */
                if (total > 0) {
                    /* Check if more is immediately available */
                    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
                    ssize_t peek = read(STDIN_FILENO, src.buf + src.len,
                                        src.cap - src.len > 0 ? src.cap - src.len : 0);
                    if (peek > 0) {
                        src.len += peek;
                        total += peek;
                        fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
                        continue;
                    }
                    /* Nothing immediately available — we have enough to start */
                    break;
                }
            }
        }
    }

    if (!have_source || src.len == 0) {
        fprintf(stderr, "usage: utterance [-f font.ttf] [-m] [file]\n       echo 'hello' | utterance\n");
        if (have_source) source_destroy(&src);
        return 1;
    }

    /* Markdown transform: -m flag, .md extension, or auto-detect */
    uint8_t *md_buf = NULL;
    int is_markdown = force_markdown;
    if (!is_markdown && file_arg) {
        size_t flen = strlen(file_arg);
        if (flen > 3 && strcmp(file_arg + flen - 3, ".md") == 0) is_markdown = 1;
    }
    if (!is_markdown && src.eof)
        is_markdown = markdown_detect(src.buf, src.len);
    if (is_markdown) {
        size_t md_len = 0;
        md_buf = markdown_to_ansi(src.buf, src.len, &md_len);
        if (md_buf) {
            /* Replace source buffer with transformed markdown */
            free(src.buf);
            src.buf = md_buf;
            src.len = md_len;
            src.cap = md_len;
            src.eof = 1;  /* no streaming for transformed markdown */
            fprintf(stderr, "utterance: markdown detected, %zu bytes\n", md_len);
        }
    }

    /* 3. Window + GL — if no display, just be cat */
    Window win;
    if (window_init(&win, 1920, 1080, "utterance") < 0) {
        fwrite(src.buf, 1, src.len, stdout);
        source_destroy(&src);
        return 0;
    }
    gl_load_all();

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* 4. Font */
    Font font;
    double t0 = glfwGetTime();
    if (font_load(&font, font_path, 48.0f) < 0) {
        source_destroy(&src);
        window_destroy(&win);
        return 1;
    }
    fprintf(stderr, "utterance: font loaded in %.2fs\n", glfwGetTime() - t0);

    /* 5. Prepare + Layout (two-phase) */
    static const float text_color[3] = {0.85f, 0.9f, 0.95f};

    PreparedText prepared;
    text_prepare(&prepared, &font, src.buf, src.len);

    TextMesh mesh;
    text_layout(&mesh, &prepared, &font, 0.0f, text_color);
    text_upload(&mesh);

    /* 6. Renderer */
    render_init();
    fx_init(win.width, win.height);

    /* 7. Camera — start at top-left of text, pulled back */
    Camera cam;
    camera_init(&cam, 0.0f, 0.0f, 200.0f);

    /* FPS counter */
    int fps_frames = 0;
    double fps_time = glfwGetTime();
    char fps_str[16] = "0 fps";
    int fps_len = 5;

    static const float fps_color[3]       = {0.4f, 0.4f, 0.45f};
    static const float crosshair_color[3] = {0.6f, 0.6f, 0.65f};
    float speed_mult = 1.0f;
    float blink_dist = 200.0f;
    int prev_plus = GLFW_RELEASE, prev_minus = GLFW_RELEASE;
    int prev_blink = GLFW_RELEASE;
    int prev_bslash = GLFW_RELEASE;
    int prev_fw = 0, prev_fh = 0;

    /* Blink animation state */
    int blink_phase = 0;
    float blink_origin[3] = {0};
    float blink_target[3] = {0};
    float blink_t = 0.0f;
    int blink_has_text = 0;
    float blink_hit[2] = {0};
    float blink_start_yaw = 0, blink_start_pitch = 0;
    float blink_target_yaw = 0, blink_target_pitch = 0;

    /* Word highlight state */
    float hl_bounds[4] = {0};
    float hl_timer = 0.0f;
    int   hl_active = 0;
    #define HL_DURATION 2.5f

    /* Selection state */
    int sel_active = 0;
    int sel_dragging = 0;
    int sel_anchor = -1, sel_cursor = -1;
    double sel_down_x = 0, sel_down_y = 0;
    int prev_ctrl_c = GLFW_RELEASE;
    #define SEL_DRAG_THRESHOLD 5.0

    /* Main loop */
    while (!window_should_close(&win)) {
        window_poll(&win);

        /* --- Streaming: poll source for new data --- */
        if (!src.eof) {
            size_t prev_len = src.len;
            size_t new_bytes = source_poll(&src);
            if (new_bytes > 0) {
                /* Source buffer may have been reallocated — update prepared text pointer */
                prepared.text = src.buf;
                prepared.text_len = src.len;

                int prev_seg_count = prepared.segs.count;
                segment_analyze_append(&prepared.segs, src.buf, src.len, prev_len);
                segment_measure_from(&prepared.segs, &font, src.buf, prev_seg_count > 0 ? prev_seg_count - 1 : 0);

                /* Force relayout */
                prepared.wrap_width = -1.0f;
                text_relayout(&mesh, &prepared, &font, prepared.wrap_width + 1.0f, text_color);
            }
        }

        /* Speed adjustment: +/- keys, edge-triggered */
        int cur_plus = glfwGetKey(win.handle, GLFW_KEY_EQUAL);
        int cur_minus = glfwGetKey(win.handle, GLFW_KEY_MINUS);
        if (cur_plus == GLFW_PRESS && prev_plus == GLFW_RELEASE)
            speed_mult *= 1.25f;
        if (cur_minus == GLFW_PRESS && prev_minus == GLFW_RELEASE)
            speed_mult *= 0.80f;
        prev_plus = cur_plus;
        prev_minus = cur_minus;

        /* Blink FX: \ key cycles effect */
        int cur_bslash = glfwGetKey(win.handle, GLFW_KEY_BACKSLASH);
        if (cur_bslash == GLFW_PRESS && prev_bslash == GLFW_RELEASE)
            fx_cycle();
        prev_bslash = cur_bslash;

        /* Input */
        float speed = 200.0f * speed_mult * win.dt;
        if (glfwGetKey(win.handle, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
            speed *= 5.0f;

        float fwd[3], right[3];
        camera_forward(&cam, fwd);
        camera_right(&cam, right);

        float dx = 0, dy = 0, dz = 0;

        /* Scroll wheel */
        if (win.scroll_y != 0.0f) {
            float scroll_speed = (font.ascent - font.descent + font.line_gap) * 3.0f;
            dy += win.scroll_y * scroll_speed;
        }

        float fspeed = speed * 4.0f;
        float bspeed = speed * 2.5f;
        if (glfwGetKey(win.handle, GLFW_KEY_W) == GLFW_PRESS) { dx += fwd[0]*fspeed; dy += fwd[1]*fspeed; dz += fwd[2]*fspeed; }
        if (glfwGetKey(win.handle, GLFW_KEY_S) == GLFW_PRESS) { dx -= fwd[0]*bspeed; dy -= fwd[1]*bspeed; dz -= fwd[2]*bspeed; }
        float sspeed = speed * 3.0f;
        if (glfwGetKey(win.handle, GLFW_KEY_A) == GLFW_PRESS) { dx -= right[0]*sspeed; dy -= right[1]*sspeed; dz -= right[2]*sspeed; }
        if (glfwGetKey(win.handle, GLFW_KEY_D) == GLFW_PRESS) { dx += right[0]*sspeed; dy += right[1]*sspeed; dz += right[2]*sspeed; }
        if (glfwGetKey(win.handle, GLFW_KEY_SPACE) == GLFW_PRESS) dy += sspeed;
        if (glfwGetKey(win.handle, GLFW_KEY_LEFT_ALT) == GLFW_PRESS) dy -= sspeed;

        /* --- Selection: click-drag to select, click to blink --- */
        if (win.lmb_pressed) {
            sel_down_x = win.mouse_x;
            sel_down_y = win.mouse_y;
            sel_dragging = 0;
            float ndc_x = (float)(2.0 * win.mouse_x / win.width - 1.0);
            float ndc_y = (float)(1.0 - 2.0 * win.mouse_y / win.height);
            float sel_ray[3];
            camera_screen_ray(&cam, ndc_x, ndc_y, sel_ray);
            float sel_t = (sel_ray[2] != 0.0f) ? -cam.pos[2] / sel_ray[2] : -1.0f;
            if (sel_t > 0.0f) {
                float hx = cam.pos[0] + sel_t * sel_ray[0];
                float hy = cam.pos[1] + sel_t * sel_ray[1];
                sel_anchor = text_glyph_at(&mesh, hx, hy);
                sel_cursor = sel_anchor;
            } else {
                sel_anchor = -1;
            }
        }

        if (win.lmb_held && sel_anchor >= 0) {
            double ddx = win.mouse_x - sel_down_x;
            double ddy = win.mouse_y - sel_down_y;
            if (!sel_dragging && (ddx*ddx + ddy*ddy > SEL_DRAG_THRESHOLD * SEL_DRAG_THRESHOLD)) {
                sel_dragging = 1;
                sel_active = 1;
            }
            if (sel_dragging) {
                float ndc_x = (float)(2.0 * win.mouse_x / win.width - 1.0);
                float ndc_y = (float)(1.0 - 2.0 * win.mouse_y / win.height);
                float sel_ray[3];
                camera_screen_ray(&cam, ndc_x, ndc_y, sel_ray);
                float sel_t = (sel_ray[2] != 0.0f) ? -cam.pos[2] / sel_ray[2] : -1.0f;
                if (sel_t > 0.0f) {
                    float hx = cam.pos[0] + sel_t * sel_ray[0];
                    float hy = cam.pos[1] + sel_t * sel_ray[1];
                    sel_cursor = text_glyph_at(&mesh, hx, hy);
                }
            }
        }

        /* Ctrl+C — copy selection */
        int cur_ctrl_c = glfwGetKey(win.handle, GLFW_KEY_C);
        if (sel_active && sel_anchor >= 0 && sel_cursor >= 0 &&
            cur_ctrl_c == GLFW_PRESS && prev_ctrl_c == GLFW_RELEASE &&
            glfwGetKey(win.handle, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) {
            int lo = sel_anchor < sel_cursor ? sel_anchor : sel_cursor;
            int hi = sel_anchor < sel_cursor ? sel_cursor : sel_anchor;
            int start = mesh.instances[lo].text_offset;
            int end_off = mesh.instances[hi].text_offset;
            uint8_t lead = src.buf[end_off];
            int clen = (lead < 0x80) ? 1 : (lead < 0xE0) ? 2 : (lead < 0xF0) ? 3 : 4;
            int end = end_off + clen;
            char *buf = malloc(end - start + 1);
            if (buf) {
                memcpy(buf, src.buf + start, end - start);
                buf[end - start] = '\0';
                glfwSetClipboardString(win.handle, buf);
                free(buf);
                fprintf(stderr, "utterance: copied %d bytes\n", end - start);
            }
        }
        prev_ctrl_c = cur_ctrl_c;

        /* Mouse-up: blink (click) or finalize selection (drag) */
        int click_blink = 0;
        float click_ray[3] = {0};
        if (win.lmb_released) {
            if (sel_dragging) {
                sel_dragging = 0;
            } else {
                sel_active = 0;
                click_blink = 1;
                float ndc_x = (float)(2.0 * win.mouse_x / win.width - 1.0);
                float ndc_y = (float)(1.0 - 2.0 * win.mouse_y / win.height);
                camera_screen_ray(&cam, ndc_x, ndc_y, click_ray);
            }
        }

        /* Blink: click or F key */
        int cur_blink = glfwGetKey(win.handle, GLFW_KEY_F);
        int do_blink = click_blink ||
                       (cur_blink == GLFW_PRESS && prev_blink == GLFW_RELEASE);
        if (do_blink && blink_phase == 0) {
            blink_origin[0] = cam.pos[0];
            blink_origin[1] = cam.pos[1];
            blink_origin[2] = cam.pos[2];

            float ray[3];
            if (click_blink) {
                ray[0] = click_ray[0]; ray[1] = click_ray[1]; ray[2] = click_ray[2];
            } else {
                ray[0] = fwd[0]; ray[1] = fwd[1]; ray[2] = fwd[2];
            }

            float t = (ray[2] != 0.0f) ? -cam.pos[2] / ray[2] : -1.0f;
            if (t > 0.0f) {
                float hit_x = cam.pos[0] + t * ray[0];
                float hit_y = cam.pos[1] + t * ray[1];
                float line_h = font.ascent - font.descent + font.line_gap;
                float target_h = 25.0f * line_h;
                float standoff = target_h / (2.0f * tanf(cam.fov / 2.0f));
                blink_target[0] = hit_x - fwd[0] * standoff;
                blink_target[1] = hit_y - fwd[1] * standoff;
                blink_target[2] = 0.0f  - fwd[2] * standoff;
                blink_has_text = text_hit_test(&mesh, hit_x, hit_y, standoff * 0.5f);
                if (blink_has_text && text_word_bounds(&mesh, &prepared, hit_x, hit_y, hl_bounds)) {
                    hl_active = 1;
                    hl_timer = HL_DURATION;
                }
            } else {
                float dist = blink_dist * speed_mult;
                blink_target[0] = cam.pos[0] + fwd[0] * dist;
                blink_target[1] = cam.pos[1] + fwd[1] * dist;
                blink_target[2] = cam.pos[2] + fwd[2] * dist;
                blink_has_text = 0;
            }
            blink_start_yaw = cam.yaw;
            blink_start_pitch = cam.pitch;
            if (t > 0.0f) {
                blink_hit[0] = cam.pos[0] + t * ray[0];
                blink_hit[1] = cam.pos[1] + t * ray[1];
                float dx_h = blink_hit[0] - blink_target[0];
                float dy_h = blink_hit[1] - blink_target[1];
                float dz_h = 0.0f - blink_target[2];
                float ideal_yaw = atan2f(dz_h, dx_h);
                float horiz = sqrtf(dx_h*dx_h + dz_h*dz_h);
                float ideal_pitch = atan2f(dy_h, horiz);
                blink_target_yaw = blink_start_yaw + (ideal_yaw - blink_start_yaw) * 0.7f;
                blink_target_pitch = blink_start_pitch + (ideal_pitch - blink_start_pitch) * 0.7f;
            } else {
                blink_target_yaw = blink_start_yaw;
                blink_target_pitch = blink_start_pitch;
            }
            blink_phase = 1;
            blink_t = 0.0f;
        }
        prev_blink = cur_blink;

        /* Animate blink */
        if (blink_phase > 0) {
            float dur = (blink_phase == 1) ? 0.18f : 0.22f;
            blink_t += win.dt / dur;
            if (blink_t > 1.0f) blink_t = 1.0f;

            float e;
            if (blink_phase == 1) {
                e = blink_t * blink_t * (3.0f - 2.0f * blink_t);
            } else {
                float inv = 1.0f - blink_t;
                e = 1.0f - inv * inv;
            }

            float anim_src[3], anim_dst[3];
            if (blink_phase == 1) {
                anim_src[0] = blink_origin[0]; anim_src[1] = blink_origin[1]; anim_src[2] = blink_origin[2];
                anim_dst[0] = blink_target[0]; anim_dst[1] = blink_target[1]; anim_dst[2] = blink_target[2];
            } else {
                anim_src[0] = blink_target[0]; anim_src[1] = blink_target[1]; anim_src[2] = blink_target[2];
                anim_dst[0] = blink_origin[0]; anim_dst[1] = blink_origin[1]; anim_dst[2] = blink_origin[2];
            }

            cam.pos[0] = anim_src[0] + (anim_dst[0] - anim_src[0]) * e;
            cam.pos[1] = anim_src[1] + (anim_dst[1] - anim_src[1]) * e;
            cam.pos[2] = anim_src[2] + (anim_dst[2] - anim_src[2]) * e;

            if (blink_phase == 1) {
                cam.yaw = blink_start_yaw + (blink_target_yaw - blink_start_yaw) * e;
                cam.pitch = blink_start_pitch + (blink_target_pitch - blink_start_pitch) * e;
            }

            dx = 0; dy = 0; dz = 0;

            if (blink_t >= 1.0f) {
                if (blink_phase == 1 && !blink_has_text) {
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

        /* --- Dynamic relayout based on camera distance --- */
        if (cam.pos[2] > 0.0f) {
            float half_w = cam.pos[2] * tanf(cam.fov / 2.0f) * cam.aspect;
            float effective_wrap = half_w * 2.0f * 0.9f;
            text_relayout(&mesh, &prepared, &font, effective_wrap, text_color);
        }

        float mvp[16];
        camera_mvp(&cam, mvp);

        /* Render */
        if (win.width != prev_fw || win.height != prev_fh) {
            fx_resize(win.width, win.height);
            prev_fw = win.width;
            prev_fh = win.height;
        }

        float fx_progress = 0.0f;
        if (blink_phase == 1)
            fx_progress = blink_t;
        else if (blink_phase == 2)
            fx_progress = 1.0f - blink_t * 0.5f;

        fx_begin(fx_progress);
        glViewport(0, 0, win.width, win.height);
        glClearColor(0.02f, 0.02f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        /* Selection highlight */
        if (sel_active && sel_anchor >= 0 && sel_cursor >= 0) {
            float sel_rects[256][4];
            int nrects = text_selection_rects(&mesh, sel_anchor, sel_cursor, sel_rects, 256);
            static const float sel_color[3] = {0.15f, 0.35f, 0.75f};
            for (int i = 0; i < nrects; i++)
                render_highlight(mvp, sel_rects[i], sel_color, 0.35f);
        }

        /* Word blink highlight */
        if (hl_active) {
            hl_timer -= win.dt;
            if (hl_timer <= 0.0f) {
                hl_active = 0;
            } else {
                float t_norm = hl_timer / HL_DURATION;
                float alpha = t_norm * t_norm * 0.3f;
                static const float hl_color[3] = {0.3f, 0.5f, 1.0f};
                render_highlight(mvp, hl_bounds, hl_color, alpha);
            }
        }

        render_text(&mesh, &font, mvp);
        fx_end(win.width, win.height, fx_progress);

        /* FPS overlay */
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

        /* Crosshair */
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
    prepared_text_destroy(&prepared);
    font_destroy(&font);
    fx_destroy();
    render_destroy();
    window_destroy(&win);
    source_destroy(&src);
    return 0;
}
