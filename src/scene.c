#define _POSIX_C_SOURCE 200809L
#include "scene.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- shader */

static const char *scene_vert_src =
    "#version 330 core\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "layout(location = 1) in vec3 a_normal;\n"
    "layout(location = 2) in vec2 a_uv;\n"
    "layout(location = 3) in vec4 a_color;\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_model;\n"
    "uniform vec4 u_fill;\n"
    "out vec4 v_color;\n"
    "out vec3 v_world_pos;\n"
    "void main() {\n"
    "    v_color = a_color * u_fill;\n"
    "    vec4 w = u_model * vec4(a_pos, 1.0);\n"
    "    v_world_pos = w.xyz;\n"
    "    gl_Position = u_mvp * w;\n"
    "}\n";

/* Derivative-based flat normal: dFdx/dFdy of world position give two edge
 * vectors of the triangle; their cross is the face normal. Works for any
 * mesh without needing authored per-vertex normals. Falls back to pure
 * v_color for degenerate triangles (normal length ~0). */
static const char *scene_frag_src =
    "#version 330 core\n"
    "in vec4 v_color;\n"
    "in vec3 v_world_pos;\n"
    "uniform vec3 u_light_dir;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "    if (v_color.a < 0.01) discard;\n"
    "    vec3 dx = dFdx(v_world_pos);\n"
    "    vec3 dy = dFdy(v_world_pos);\n"
    "    vec3 n  = cross(dx, dy);\n"
    "    float nl = length(n);\n"
    "    float shade = 1.0;\n"
    "    if (nl > 1e-6) {\n"
    "        n /= nl;\n"
    "        float ndl = max(dot(n, normalize(u_light_dir)), 0.0);\n"
    "        shade = 0.35 + 0.65 * ndl;\n"
    "    }\n"
    "    frag_color = vec4(v_color.rgb * shade, v_color.a);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "scene: shader error: %s\n", log);
    }
    return s;
}

/* ---------------------------------------------------------------- matrix */

void scene_mat_identity(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void scene_mat_translate(float m[16], float x, float y, float z) {
    scene_mat_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
}

/* Column-major multiply: out = a * b. */
void scene_mat_mul(const float a[16], const float b[16], float out[16]) {
    float tmp[16];
    for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
        float v = 0;
        for (int k = 0; k < 4; k++) v += a[k * 4 + r] * b[c * 4 + k];
        tmp[c * 4 + r] = v;
    }
    memcpy(out, tmp, sizeof tmp);
}

/* ---------------------------------------------------------------- lifecycle */

void scene_init(Scene *s) {
    memset(s, 0, sizeof *s);

    glGenVertexArrays(1, &s->vao);
    glGenBuffers(1, &s->vbo);
    glGenBuffers(1, &s->ibo);

    glBindVertexArray(s->vao);
    glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->ibo);

    size_t stride = sizeof(SceneVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          (void *)offsetof(SceneVertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          (void *)offsetof(SceneVertex, normal));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          (void *)offsetof(SceneVertex, uv));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
                          (void *)offsetof(SceneVertex, color));
    glBindVertexArray(0);

    GLuint vs = compile_shader(GL_VERTEX_SHADER,   scene_vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, scene_frag_src);
    s->default_shader = glCreateProgram();
    glAttachShader(s->default_shader, vs);
    glAttachShader(s->default_shader, fs);
    glLinkProgram(s->default_shader);
    glDeleteShader(vs);
    glDeleteShader(fs);

    s->loc_mvp       = glGetUniformLocation(s->default_shader, "u_mvp");
    s->loc_model     = glGetUniformLocation(s->default_shader, "u_model");
    s->loc_fill      = glGetUniformLocation(s->default_shader, "u_fill");
    s->loc_light_dir = glGetUniformLocation(s->default_shader, "u_light_dir");

    /* Neutral bounds (min > max means "empty"). */
    for (int i = 0; i < 3; i++) { s->bounds_min[i] = 1e30f; s->bounds_max[i] = -1e30f; }
}

void scene_destroy(Scene *s) {
    if (!s) return;
    for (int i = 0; i < s->node_count; i++) {
        free(s->nodes[i].name);
        free(s->nodes[i].href);
    }
    free(s->nodes);
    free(s->meshes);
    free(s->materials);
    free(s->verts);
    free(s->indices);
    if (s->vao) glDeleteVertexArrays(1, &s->vao);
    if (s->vbo) glDeleteBuffers(1, &s->vbo);
    if (s->ibo) glDeleteBuffers(1, &s->ibo);
    if (s->default_shader) glDeleteProgram(s->default_shader);
    memset(s, 0, sizeof *s);
}

/* ---------------------------------------------------------------- adders */

static void ensure_cap_void(void **buf, int *cap, int need, size_t elem) {
    if (*cap >= need) return;
    int nc = *cap ? *cap : 8;
    while (nc < need) nc *= 2;
    void *tmp = realloc(*buf, (size_t)nc * elem);
    if (!tmp) return;
    *buf = tmp;
    *cap = nc;
}

int scene_add_material(Scene *s, const SceneMaterial *m) {
    ensure_cap_void((void **)&s->materials, &s->mat_cap, s->mat_count + 1,
                    sizeof(SceneMaterial));
    s->materials[s->mat_count] = *m;
    return s->mat_count++;
}

int scene_add_mesh(Scene *s, const SceneVertex *verts, uint32_t v_count,
                   const uint32_t *indices, uint32_t i_count) {
    /* Append verts. */
    if (s->vert_count + v_count > s->vert_cap) {
        uint32_t nc = s->vert_cap ? s->vert_cap : 1024;
        while (nc < s->vert_count + v_count) nc *= 2;
        SceneVertex *tmp = realloc(s->verts, (size_t)nc * sizeof(SceneVertex));
        if (!tmp) return -1;
        s->verts = tmp;
        s->vert_cap = nc;
    }
    uint32_t vb_offset = s->vert_count;
    memcpy(s->verts + vb_offset, verts, (size_t)v_count * sizeof(SceneVertex));
    s->vert_count += v_count;

    /* Append indices — rebased onto the global vertex buffer. */
    if (s->idx_count + i_count > s->idx_cap) {
        uint32_t nc = s->idx_cap ? s->idx_cap : 2048;
        while (nc < s->idx_count + i_count) nc *= 2;
        uint32_t *tmp = realloc(s->indices, (size_t)nc * sizeof(uint32_t));
        if (!tmp) return -1;
        s->indices = tmp;
        s->idx_cap = nc;
    }
    uint32_t ib_offset = s->idx_count;
    for (uint32_t i = 0; i < i_count; i++)
        s->indices[ib_offset + i] = indices[i] + vb_offset;
    s->idx_count += i_count;

    /* Local AABB. */
    float mn[3] = { 1e30f,  1e30f,  1e30f};
    float mx[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < v_count; i++) {
        for (int k = 0; k < 3; k++) {
            float v = verts[i].pos[k];
            if (v < mn[k]) mn[k] = v;
            if (v > mx[k]) mx[k] = v;
        }
    }

    ensure_cap_void((void **)&s->meshes, &s->mesh_cap, s->mesh_count + 1,
                    sizeof(SceneMesh));
    SceneMesh *m = &s->meshes[s->mesh_count];
    m->vb_offset = vb_offset;
    m->vb_count  = v_count;
    m->ib_offset = ib_offset;
    m->ib_count  = i_count;
    memcpy(m->bounds_min, mn, sizeof mn);
    memcpy(m->bounds_max, mx, sizeof mx);

    s->uploaded = 0;
    return s->mesh_count++;
}

int scene_add_node(Scene *s, const SceneNode *n) {
    ensure_cap_void((void **)&s->nodes, &s->node_cap, s->node_count + 1,
                    sizeof(SceneNode));
    s->nodes[s->node_count] = *n;
    /* Duplicate owned strings so the caller can free their originals. */
    if (n->name) s->nodes[s->node_count].name = strdup(n->name);
    if (n->href) s->nodes[s->node_count].href = strdup(n->href);
    return s->node_count++;
}

/* ---------------------------------------------------------------- upload */

void scene_upload(Scene *s) {
    if (s->uploaded) return;
    glBindVertexArray(s->vao);
    glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(s->vert_count * sizeof(SceneVertex)),
                 s->verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)(s->idx_count * sizeof(uint32_t)),
                 s->indices, GL_STATIC_DRAW);
    glBindVertexArray(0);
    s->uploaded = 1;
}

/* ---------------------------------------------------------------- transforms */

void scene_bake_transforms(Scene *s) {
    /* Parents must appear before their children in the node array. We assume
     * the importer inserts them in order (valid for SVG, glTF, FBX, VOX — all
     * of which serialize parents first). */
    for (int i = 0; i < s->node_count; i++) {
        SceneNode *n = &s->nodes[i];
        if (n->parent < 0 || n->parent >= i) {
            memcpy(n->world_xform, n->local_xform, sizeof n->local_xform);
        } else {
            scene_mat_mul(s->nodes[n->parent].world_xform, n->local_xform,
                          n->world_xform);
        }
    }
}

static void transform_point(const float m[16], const float p[3], float out[3]) {
    out[0] = m[0] * p[0] + m[4] * p[1] + m[8]  * p[2] + m[12];
    out[1] = m[1] * p[0] + m[5] * p[1] + m[9]  * p[2] + m[13];
    out[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
}

void scene_compute_bounds(Scene *s) {
    float mn[3] = { 1e30f,  1e30f,  1e30f};
    float mx[3] = {-1e30f, -1e30f, -1e30f};
    int any = 0;
    for (int i = 0; i < s->node_count; i++) {
        SceneNode *n = &s->nodes[i];
        if (n->mesh_id < 0 || !(n->flags & SCENE_NODE_VISIBLE)) continue;
        if (n->mesh_id >= s->mesh_count) continue;
        SceneMesh *m = &s->meshes[n->mesh_id];
        /* Transform each of the 8 AABB corners and re-bound. Cheap and
         * handles rotation correctly. */
        for (int c = 0; c < 8; c++) {
            float p[3] = {
                (c & 1) ? m->bounds_max[0] : m->bounds_min[0],
                (c & 2) ? m->bounds_max[1] : m->bounds_min[1],
                (c & 4) ? m->bounds_max[2] : m->bounds_min[2],
            };
            float w[3];
            transform_point(n->world_xform, p, w);
            for (int k = 0; k < 3; k++) {
                if (w[k] < mn[k]) mn[k] = w[k];
                if (w[k] > mx[k]) mx[k] = w[k];
            }
            any = 1;
        }
    }
    if (any) {
        memcpy(s->bounds_min, mn, sizeof mn);
        memcpy(s->bounds_max, mx, sizeof mx);
    }
}

/* ---------------------------------------------------------------- render */

void scene_render(const Scene *s, const float mvp[16]) {
    if (!s->uploaded || s->node_count == 0) return;

    if (s->depth_enabled) glEnable(GL_DEPTH_TEST);
    else                  glDisable(GL_DEPTH_TEST);

    glUseProgram(s->default_shader);
    glUniformMatrix4fv(s->loc_mvp, 1, GL_FALSE, mvp);
    /* Light rakes across from upper-right/front so extruded faces read clearly;
     * flat shapes still look correct since the fragment's derivative normal
     * comes out as +z and the dot product just scales brightness uniformly. */
    if (s->loc_light_dir >= 0)
        glUniform3f(s->loc_light_dir, 0.4f, 0.6f, 0.8f);
    glBindVertexArray(s->vao);

    for (int i = 0; i < s->node_count; i++) {
        const SceneNode *n = &s->nodes[i];
        if (n->mesh_id < 0 || !(n->flags & SCENE_NODE_VISIBLE)) continue;
        if (n->mesh_id >= s->mesh_count) continue;
        const SceneMesh *m = &s->meshes[n->mesh_id];
        if (m->ib_count == 0) continue;

        const SceneMaterial *mat = (n->material_id >= 0 && n->material_id < s->mat_count)
                                    ? &s->materials[n->material_id] : NULL;
        float fill[4] = {1, 1, 1, 1};
        if (mat) memcpy(fill, mat->fill_rgba, sizeof fill);

        glUniformMatrix4fv(s->loc_model, 1, GL_FALSE, n->world_xform);
        glUniform4fv(s->loc_fill, 1, fill);
        glDrawElements(GL_TRIANGLES, (GLsizei)m->ib_count, GL_UNSIGNED_INT,
                       (void *)(uintptr_t)(m->ib_offset * sizeof(uint32_t)));
    }
    glBindVertexArray(0);
}
