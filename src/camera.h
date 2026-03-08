#ifndef CAMERA_H
#define CAMERA_H

typedef struct {
    float pos[3];
    float yaw, pitch;
    float fov;
    float aspect;
    float near, far;
} Camera;

void camera_init(Camera *c, float x, float y, float z);
void camera_update(Camera *c, float dx, float dy, float dz, float dyaw, float dpitch);
void camera_mvp(const Camera *c, float out[16]);
void camera_forward(const Camera *c, float out[3]);
void camera_right(const Camera *c, float out[3]);
void camera_screen_ray(const Camera *c, float ndc_x, float ndc_y, float dir[3]);

#endif
