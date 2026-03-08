#include "window.h"
#include <stdio.h>
#include <stdlib.h>

static void fb_resize_cb(GLFWwindow *win, int w, int h) {
    Window *uw = glfwGetWindowUserPointer(win);
    uw->width = w;
    uw->height = h;
}

int window_init(Window *w, int width, int height, const char *title) {
    if (!glfwInit()) {
        fprintf(stderr, "utterance: glfwInit failed\n");
        return -1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
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

    glfwGetFramebufferSize(w->handle, &w->width, &w->height);
    glfwGetCursorPos(w->handle, &w->mouse_x, &w->mouse_y);
    w->last_mouse_x = w->mouse_x;
    w->last_mouse_y = w->mouse_y;
    w->mouse_captured = 0;
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
    glfwPollEvents();
    glfwGetCursorPos(w->handle, &w->mouse_x, &w->mouse_y);

    /* Click to capture, Escape to release */
    if (glfwGetMouseButton(w->handle, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS && !w->mouse_captured) {
        glfwSetInputMode(w->handle, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        w->mouse_captured = 1;
        w->last_mouse_x = w->mouse_x;
        w->last_mouse_y = w->mouse_y;
    }
    if (glfwGetKey(w->handle, GLFW_KEY_ESCAPE) == GLFW_PRESS && w->mouse_captured) {
        glfwSetInputMode(w->handle, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        w->mouse_captured = 0;
    }
    if (glfwGetKey(w->handle, GLFW_KEY_Q) == GLFW_PRESS) {
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
