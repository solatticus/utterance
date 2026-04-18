#include "window.h"
#include <stdio.h>
#include <stdlib.h>

static void fb_resize_cb(GLFWwindow *win, int w, int h) {
    Window *uw = glfwGetWindowUserPointer(win);
    uw->width = w;
    uw->height = h;
}

static void scroll_cb(GLFWwindow *win, double xoff, double yoff) {
    (void)xoff;
    Window *uw = glfwGetWindowUserPointer(win);
    uw->scroll_y += (float)yoff;
}

static void glfw_error_cb(int code, const char *msg) {
    fprintf(stderr, "utterance: glfw error %d: %s\n", code, msg);
}

int window_init(Window *w, int width, int height, const char *title) {
    glfwSetErrorCallback(glfw_error_cb);
    if (!glfwInit()) {
        fprintf(stderr, "utterance: glfwInit failed\n");
        return -1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_SAMPLES, 4);

    w->handle = glfwCreateWindow(width, height, title, NULL, NULL);
    if (!w->handle) {
        fprintf(stderr, "utterance: window creation failed\n");
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(w->handle);
    glfwSwapInterval(1);
    glfwSetWindowUserPointer(w->handle, w);
    glfwSetFramebufferSizeCallback(w->handle, fb_resize_cb);
    glfwSetScrollCallback(w->handle, scroll_cb);

    glfwGetFramebufferSize(w->handle, &w->width, &w->height);
    glfwGetCursorPos(w->handle, &w->mouse_x, &w->mouse_y);
    w->last_mouse_x = w->mouse_x;
    w->last_mouse_y = w->mouse_y;
    w->mouse_captured = 0;
    w->lmb_pressed = 0;
    w->lmb_released = 0;
    w->lmb_held = 0;
    w->scroll_y = 0.0f;
    w->last_time = glfwGetTime();
    w->dt = 0.0f;
    return 0;
}

void window_poll(Window *w) {
    double now = glfwGetTime();
    w->dt = (float)(now - w->last_time);
    w->last_time = now;

    w->last_mouse_x = w->mouse_x;
    w->last_mouse_y = w->mouse_y;
    w->scroll_y = 0.0f;
    glfwPollEvents();
    glfwGetCursorPos(w->handle, &w->mouse_x, &w->mouse_y);

    /* Right-click hold = mouselook */
    int rmb = glfwGetMouseButton(w->handle, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    if (rmb && !w->mouse_captured) {
        glfwSetInputMode(w->handle, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        w->mouse_captured = 1;
        w->last_mouse_x = w->mouse_x;
        w->last_mouse_y = w->mouse_y;
    } else if (!rmb && w->mouse_captured) {
        glfwSetInputMode(w->handle, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        w->mouse_captured = 0;
    }

    /* Left mouse button state */
    static int prev_lmb = GLFW_RELEASE;
    int lmb = glfwGetMouseButton(w->handle, GLFW_MOUSE_BUTTON_LEFT);
    w->lmb_pressed = (lmb == GLFW_PRESS && prev_lmb == GLFW_RELEASE);
    w->lmb_released = (lmb == GLFW_RELEASE && prev_lmb == GLFW_PRESS);
    w->lmb_held = (lmb == GLFW_PRESS);
    prev_lmb = lmb;

    if (glfwGetKey(w->handle, GLFW_KEY_ESCAPE) == GLFW_PRESS ||
        glfwGetKey(w->handle, GLFW_KEY_Q) == GLFW_PRESS) {
        glfwSetWindowShouldClose(w->handle, GLFW_TRUE);
    }
}

int window_should_close(Window *w) {
    return glfwWindowShouldClose(w->handle);
}

void window_swap(Window *w) {
    glfwSwapBuffers(w->handle);
}

void window_destroy(Window *w) {
    glfwDestroyWindow(w->handle);
    glfwTerminate();
}
