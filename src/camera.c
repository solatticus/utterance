#include "camera.h"
#include <math.h>
#include <string.h>

#define PI_F       3.14159265f
#define HALF_PI_F  1.57079632f
#define DEG60_RAD  1.04719755f
#define DEG50_RAD  0.87266463f
#define PITCH_LIMIT 1.5f

void camera_init(Camera *c, float x, float y, float z) {
    c->pos[0] = x; c->pos[1] = y; c->pos[2] = z;
    c->yaw = -HALF_PI_F; /* looking down -Z */
    c->pitch = 0.0f;
    c->fov = DEG50_RAD;
    c->aspect = 16.0f / 9.0f;
    c->near_plane = 0.1f;
    c->far_plane = 10000.0f;
}

void camera_forward(const Camera *c, float out[3]) {
    out[0] = cosf(c->pitch) * cosf(c->yaw);
    out[1] = sinf(c->pitch);
    out[2] = cosf(c->pitch) * sinf(c->yaw);
}

void camera_right(const Camera *c, float out[3]) {
    out[0] = cosf(c->yaw + HALF_PI_F);
    out[1] = 0.0f;
    out[2] = sinf(c->yaw + HALF_PI_F);
}

void camera_update(Camera *c, float dx, float dy, float dz, float dyaw, float dpitch) {
    c->pos[0] += dx;
    c->pos[1] += dy;
    c->pos[2] += dz;
    c->yaw += dyaw;
    c->pitch -= dpitch;
    if (c->pitch >  PITCH_LIMIT) c->pitch =  PITCH_LIMIT;
    if (c->pitch < -PITCH_LIMIT) c->pitch = -PITCH_LIMIT;
}

static void mat4_zero(float m[16]) { memset(m, 0, 16 * sizeof(float)); }

static void mat4_perspective(float m[16], float fov, float aspect, float near, float far) {
    mat4_zero(m);
    float t = tanf(fov / 2.0f);
    m[0]  = 1.0f / (aspect * t);
    m[5]  = 1.0f / t;
    m[10] = -(far + near) / (far - near);
    m[11] = -1.0f;
    m[14] = -(2.0f * far * near) / (far - near);
}

static void mat4_look(float m[16], const float eye[3], const float fwd[3], const float up[3]) {
    /* right = fwd x up */
    float r[3] = {
        fwd[1]*up[2] - fwd[2]*up[1],
        fwd[2]*up[0] - fwd[0]*up[2],
        fwd[0]*up[1] - fwd[1]*up[0]
    };
    /* true up = right x fwd */
    float u[3] = {
        r[1]*fwd[2] - r[2]*fwd[1],
        r[2]*fwd[0] - r[0]*fwd[2],
        r[0]*fwd[1] - r[1]*fwd[0]
    };

    mat4_zero(m);
    m[0] = r[0];  m[4] = r[1];  m[8]  = r[2];
    m[1] = u[0];  m[5] = u[1];  m[9]  = u[2];
    m[2] = -fwd[0]; m[6] = -fwd[1]; m[10] = -fwd[2];
    m[12] = -(r[0]*eye[0] + r[1]*eye[1] + r[2]*eye[2]);
    m[13] = -(u[0]*eye[0] + u[1]*eye[1] + u[2]*eye[2]);
    m[14] = fwd[0]*eye[0] + fwd[1]*eye[1] + fwd[2]*eye[2];
    m[15] = 1.0f;
}

static void mat4_mul(float out[16], const float a[16], const float b[16]) {
    float tmp[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            tmp[c*4+r] = 0;
            for (int k = 0; k < 4; k++)
                tmp[c*4+r] += a[k*4+r] * b[c*4+k];
        }
    memcpy(out, tmp, sizeof(tmp));
}

void camera_mvp(const Camera *c, float out[16]) {
    float fwd[3], up[3] = {0, 1, 0};
    camera_forward(c, fwd);

    float view[16], proj[16];
    mat4_perspective(proj, c->fov, c->aspect, c->near_plane, c->far_plane);
    mat4_look(view, c->pos, fwd, up);
    mat4_mul(out, proj, view);
}

void camera_screen_ray(const Camera *c, float ndc_x, float ndc_y, float dir[3]) {
    /* Unproject NDC point through inverse projection into view space,
       then rotate by camera orientation to get world-space ray direction. */
    float t = tanf(c->fov / 2.0f);
    /* View-space offset from center (NDC → view plane at z=-1) */
    float vx = ndc_x * c->aspect * t;
    float vy = ndc_y * t;

    /* Camera basis vectors */
    float fwd[3], right[3], up[3];
    camera_forward(c, fwd);
    camera_right(c, right);
    /* up = right x fwd */
    up[0] = right[1]*fwd[2] - right[2]*fwd[1];
    up[1] = right[2]*fwd[0] - right[0]*fwd[2];
    up[2] = right[0]*fwd[1] - right[1]*fwd[0];

    /* fwd already points where camera looks — add view-plane offset */
    dir[0] = fwd[0] + right[0]*vx + up[0]*vy;
    dir[1] = fwd[1] + right[1]*vx + up[1]*vy;
    dir[2] = fwd[2] + right[2]*vx + up[2]*vy;

    /* Normalize */
    float len = sqrtf(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
    if (len > 0.0f) { dir[0] /= len; dir[1] /= len; dir[2] /= len; }
}
