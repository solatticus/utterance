#define _POSIX_C_SOURCE 200809L
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
#include "image.h"
#include <libgen.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

#define MOUSE_SENSITIVITY 0.002f

/* --- Key event queue for cursor navigation with repeat --- */
#define MAX_KEY_EVENTS 64
static struct { int key; int action; int mods; } g_key_events[MAX_KEY_EVENTS];
static int g_key_count = 0;

static void key_callback(GLFWwindow *win, int key, int scancode, int action, int mods) {
    (void)win; (void)scancode;
    if (g_key_count < MAX_KEY_EVENTS && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        g_key_events[g_key_count].key = key;
        g_key_events[g_key_count].action = action;
        g_key_events[g_key_count].mods = mods;
        g_key_count++;
    }
}

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
    MdImageList md_images = {0};
    MdLinkList md_links = {0};
    MdCodeBlockList md_codeblocks = {0};
    int is_markdown = force_markdown;
    if (!is_markdown && file_arg) {
        size_t flen = strlen(file_arg);
        if (flen > 3 && strcmp(file_arg + flen - 3, ".md") == 0) is_markdown = 1;
    }
    if (!is_markdown && src.eof)
        is_markdown = markdown_detect(src.buf, src.len);
    if (is_markdown) {
        size_t md_len = 0;
        md_buf = markdown_to_ansi(src.buf, src.len, &md_len, &md_images,
                                  &md_links, &md_codeblocks);
        if (md_buf) {
            free(src.buf);
            src.buf = md_buf;
            src.len = md_len;
            src.cap = md_len;
            src.eof = 1;
            fprintf(stderr, "utterance: markdown detected, %zu bytes, %d images\n",
                    md_len, md_images.count);
        }
    }

    /* Resolve base directory for relative image paths */
    char *base_dir = NULL;
    if (file_arg) {
        char *tmp = strdup(file_arg);
        base_dir = strdup(dirname(tmp));
        free(tmp);
    }

    /* 3. Window + GL — if no display, just be cat */
    Window win;
    if (window_init(&win, 1920, 1080, "utterance") < 0) {
        fwrite(src.buf, 1, src.len, stdout);
        source_destroy(&src);
        return 0;
    }
    gl_load_all();
    glfwSetKeyCallback(win.handle, key_callback);

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
    image_init_renderer();
    fx_init(win.width, win.height);

    /* 6b. Load images from markdown and place at placeholder positions */
    ImageList images = {0};
    if (md_images.count > 0) {
        float line_h = font.ascent - font.descent + font.line_gap;
        for (int i = 0; i < md_images.count; i++) {
            int idx = image_load(&images, md_images.images[i].path, base_dir);
            if (idx >= 0) {
                Image *img = &images.items[idx];
                /* Find the placeholder glyph's position in the mesh */
                int ph_offset = md_images.images[i].placeholder;
                for (int g = 0; g < mesh.count; g++) {
                    if (mesh.instances[g].text_offset >= ph_offset) {
                        /* Place image below this glyph, spanning the text width */
                        float max_w = 600.0f; /* max image width in world units */
                        float aspect = (float)img->width / (float)img->height;
                        img->world_w = max_w < (aspect * line_h * 8) ? max_w : aspect * line_h * 8;
                        img->world_h = img->world_w / aspect;
                        img->world_x = mesh.instances[g].x;
                        img->world_y = mesh.instances[g].y - img->world_h - line_h * 0.5f;
                        img->placed = 1;
                        break;
                    }
                }
            }
        }
        md_image_list_destroy(&md_images);
    }
    free(base_dir);

    /* 7. Camera — start with ~15 readable lines centered on top portion */
    Camera cam;
    {
        float line_h = font.ascent - font.descent + font.line_gap;
        float target_lines = 20.0f;
        float half_fov_tan = 0.46631f; /* tan(25°) for 50° FOV */
        float aspect = (float)win.width / (float)win.height;
        float start_z = (target_lines * line_h) / (2.0f * half_fov_tan);
        float start_y = -(target_lines * 0.25f) * line_h;
        /* Center on the text: wrap width = 2 * z * tan * aspect * 0.9 */
        float start_x = start_z * half_fov_tan * aspect * 0.9f;
        camera_init(&cam, start_x, start_y, start_z);
        cam.pitch = -0.06f; /* slight downward tilt — looking at the page */
    }

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

    /* Text cursor state */
    int cursor_pos = -1;           /* glyph index, -1 = no cursor */
    float cursor_blink = 0.0f;
    float cursor_target_x = -1.0f; /* column memory for up/down */

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

        /* --- Keyboard cursor navigation --- */
        {
            float line_h_nav = font.ascent - font.descent + font.line_gap;
            int page_lines = 20;

            for (int ki = 0; ki < g_key_count; ki++) {
                int key = g_key_events[ki].key;
                int mods = g_key_events[ki].mods;
                int shift = (mods & GLFW_MOD_SHIFT) != 0;
                int ctrl = (mods & GLFW_MOD_CONTROL) != 0;
                int old_cursor = cursor_pos;
                int moved = 0;

                if (cursor_pos < 0 && mesh.count > 0) cursor_pos = 0;
                if (cursor_pos < 0) continue;

                switch (key) {
                case GLFW_KEY_LEFT:
                    if (cursor_pos > 0) { cursor_pos--; moved = 1; }
                    cursor_target_x = -1;
                    break;
                case GLFW_KEY_RIGHT:
                    if (cursor_pos < mesh.count - 1) { cursor_pos++; moved = 1; }
                    cursor_target_x = -1;
                    break;
                case GLFW_KEY_UP:
                    if (cursor_target_x < 0 && cursor_pos < mesh.count)
                        cursor_target_x = mesh.instances[cursor_pos].x;
                    { int np = text_line_up(&mesh, cursor_pos, cursor_target_x);
                      if (np != cursor_pos) { cursor_pos = np; moved = 1; } }
                    break;
                case GLFW_KEY_DOWN:
                    if (cursor_target_x < 0 && cursor_pos < mesh.count)
                        cursor_target_x = mesh.instances[cursor_pos].x;
                    { int np = text_line_down(&mesh, cursor_pos, cursor_target_x);
                      if (np != cursor_pos) { cursor_pos = np; moved = 1; } }
                    break;
                case GLFW_KEY_PAGE_UP:
                    if (cursor_target_x < 0 && cursor_pos < mesh.count)
                        cursor_target_x = mesh.instances[cursor_pos].x;
                    for (int pl = 0; pl < page_lines; pl++) {
                        int np = text_line_up(&mesh, cursor_pos, cursor_target_x);
                        if (np == cursor_pos) break;
                        cursor_pos = np;
                    }
                    moved = (cursor_pos != old_cursor);
                    /* Also scroll camera */
                    dy += line_h_nav * page_lines;
                    break;
                case GLFW_KEY_PAGE_DOWN:
                    if (cursor_target_x < 0 && cursor_pos < mesh.count)
                        cursor_target_x = mesh.instances[cursor_pos].x;
                    for (int pl = 0; pl < page_lines; pl++) {
                        int np = text_line_down(&mesh, cursor_pos, cursor_target_x);
                        if (np == cursor_pos) break;
                        cursor_pos = np;
                    }
                    moved = (cursor_pos != old_cursor);
                    dy -= line_h_nav * page_lines;
                    break;
                case GLFW_KEY_HOME:
                    if (ctrl) { cursor_pos = 0; }
                    else { cursor_pos = text_line_start(&mesh, cursor_pos); }
                    moved = (cursor_pos != old_cursor);
                    cursor_target_x = -1;
                    break;
                case GLFW_KEY_END:
                    if (ctrl) { cursor_pos = mesh.count > 0 ? mesh.count - 1 : 0; }
                    else { cursor_pos = text_line_end(&mesh, cursor_pos); }
                    moved = (cursor_pos != old_cursor);
                    cursor_target_x = -1;
                    break;
                }

                if (moved) {
                    cursor_blink = 0.0f; /* reset blink on movement */
                    if (shift) {
                        /* Extend selection */
                        if (sel_anchor < 0) sel_anchor = old_cursor;
                        sel_cursor = cursor_pos;
                        sel_active = (sel_anchor != sel_cursor);
                    } else {
                        /* Clear selection */
                        sel_active = 0;
                        sel_anchor = -1;
                        sel_cursor = -1;
                    }
                }
            }
            g_key_count = 0;
        }

        /* Mouse click sets cursor position */
        if (win.lmb_pressed && sel_anchor >= 0) {
            cursor_pos = sel_anchor;
            cursor_blink = 0.0f;
            cursor_target_x = -1;
        }

        /* Mouse-up: blink (click on text) or deselect (click on empty) */
        int click_blink = 0;
        float click_ray[3] = {0};
        if (win.lmb_released) {
            if (sel_dragging) {
                sel_dragging = 0;
            } else {
                sel_active = 0;
                sel_anchor = -1;
                sel_cursor = -1;
                /* Only blink if click hits near a glyph */
                float ndc_x = (float)(2.0 * win.mouse_x / win.width - 1.0);
                float ndc_y = (float)(1.0 - 2.0 * win.mouse_y / win.height);
                camera_screen_ray(&cam, ndc_x, ndc_y, click_ray);
                float ct = (click_ray[2] != 0.0f) ? -cam.pos[2] / click_ray[2] : -1.0f;
                if (ct > 0.0f) {
                    float hx = cam.pos[0] + ct * click_ray[0];
                    float hy = cam.pos[1] + ct * click_ray[1];
                    float line_h = font.ascent - font.descent + font.line_gap;
                    if (text_hit_test(&mesh, hx, hy, line_h * 2.0f)) {
                        /* Ctrl+click: open URL */
                        if (glfwGetKey(win.handle, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS &&
                            md_links.count > 0) {
                            int gi = text_glyph_at(&mesh, hx, hy);
                            if (gi >= 0 && gi < mesh.count) {
                                int off = mesh.instances[gi].text_offset;
                                for (int li = 0; li < md_links.count; li++) {
                                    if (off >= md_links.links[li].start_offset &&
                                        off < md_links.links[li].end_offset) {
                                        char cmd[2048];
                                        snprintf(cmd, sizeof(cmd), "xdg-open '%s' &",
                                                 md_links.links[li].url);
                                        if (system(cmd) == 0)
                                            fprintf(stderr, "utterance: opening %s\n",
                                                    md_links.links[li].url);
                                        break;
                                    }
                                }
                            }
                        } else {
                            click_blink = 1;
                        }
                    }
                }
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

        /* Code block backgrounds */
        for (int cbi = 0; cbi < md_codeblocks.count; cbi++) {
            MdCodeBlock *cb = &md_codeblocks.blocks[cbi];
            float cbx0 = 1e30f, cby0 = 1e30f, cbx1 = -1e30f, cby1 = -1e30f;
            for (int g = 0; g < mesh.count; g++) {
                int off = mesh.instances[g].text_offset;
                if (off >= cb->start_offset && off < cb->end_offset) {
                    GlyphInstance *gi = &mesh.instances[g];
                    if (gi->x < cbx0) cbx0 = gi->x;
                    if (gi->y < cby0) cby0 = gi->y;
                    if (gi->x + gi->w > cbx1) cbx1 = gi->x + gi->w;
                    if (gi->y + gi->h > cby1) cby1 = gi->y + gi->h;
                }
            }
            if (cbx0 < 1e29f) {
                float pad = 8.0f;
                float cbb[4] = { cbx0 - pad, cby0 - pad, cbx1 + pad, cby1 + pad };
                static const float cb_color[3] = {0.12f, 0.12f, 0.16f};
                render_highlight(mvp, cbb, cb_color, 0.9f);
            }
        }

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
        image_render(&images, mvp);

        /* Text cursor — blinking vertical bar */
        if (cursor_pos >= 0 && cursor_pos < mesh.count) {
            cursor_blink += win.dt;
            if (cursor_blink > 1.0f) cursor_blink -= 1.0f;
            if (cursor_blink < 0.53f) {
                GlyphInstance *cg = &mesh.instances[cursor_pos];
                float cb[4] = { cg->x - 1.0f, cg->y - 1.0f,
                                cg->x + 1.5f, cg->y + cg->h + 1.0f };
                static const float cursor_color[3] = {0.9f, 0.9f, 0.95f};
                render_highlight(mvp, cb, cursor_color, 0.85f);
            }
        }

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
    image_list_destroy(&images);
    image_destroy_renderer();
    md_link_list_destroy(&md_links);
    md_codeblock_list_destroy(&md_codeblocks);
    font_destroy(&font);
    fx_destroy();
    render_destroy();
    window_destroy(&win);
    source_destroy(&src);
    return 0;
}
