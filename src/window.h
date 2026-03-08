#ifndef WINDOW_H
#define WINDOW_H

#include <GLFW/glfw3.h>

typedef struct {
    GLFWwindow *handle;
    int width, height;
    double mouse_x, mouse_y;
    double last_mouse_x, last_mouse_y;
    int mouse_captured;
    float dt;
    double last_time;
} Window;

int  window_init(Window *w, int width, int height, const char *title);
void window_poll(Window *w);
int  window_should_close(Window *w);
void window_swap(Window *w);
void window_destroy(Window *w);

#endif
