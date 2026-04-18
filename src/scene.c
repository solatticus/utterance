#define _POSIX_C_SOURCE 200809L
#include "scene.h"

#include <math.h>
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
    "out vec3 v_normal;\n"
    "void main() {\n"
    "    v_color = a_color * u_fill;\n"
    "    v_normal = mat3(u_model) * a_normal;\n"
    "    gl_Position = u_mvp * u_model * vec4(a_pos, 1.0);\n"
    "}\n";

/* Pick pass: minimal transform, writes the per-draw node id straight to a
 * uint render target. No shading, no color attribute read — keeps the shader
 * tiny and the frag invocations cheap. */
static const char *scene_pick_vert_src =
    "#version 330 core\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_model;\n"
    "void main() {\n"
    "    gl_Position = u_mvp * u_model * vec4(a_pos, 1.0);\n"
    "}\n";

static const char *scene_pick_frag_src =
    "#version 330 core\n"
    "uniform uint u_id;\n"
    "out uint frag_id;\n"
    "void main() { frag_id = u_id; }\n";

/* Authored per-vertex normals. Zero normal (length < 1e-6) falls through to
 * unlit shade=1.0 so flat 2D fills and stroke ribbons — which never need
 * directional lighting — stay full-bright. Extruded prisms and native
 * primitives (cuboid, cylinder) author real face normals for Lambert shade. */
static const char *scene_frag_src =
    "#version 330 core\n"
    "in vec4 v_color;\n"
    "in vec3 v_normal;\n"
    "uniform vec3 u_light_dir;\n"
    "uniform vec3 u_tint;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "    if (v_color.a < 0.01) discard;\n"
    "    vec3 n = v_normal;\n"
    "    float nl = length(n);\n"
    "    float shade = 1.0;\n"
    "    if (nl > 1e-6) {\n"
    "        n /= nl;\n"
    "        float ndl = max(dot(n, normalize(u_light_dir)), 0.0);\n"
    "        shade = 0.35 + 0.65 * ndl;\n"
    "    }\n"
    "    frag_color = vec4(v_color.rgb * shade + u_tint, v_color.a);\n"
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
    s->loc_tint      = glGetUniformLocation(s->default_shader, "u_tint");

    /* Pick shader — built up-front so the first pick call has no latency
     * spike. The FBO and its textures are lazy because they need a size. */
    GLuint pvs = compile_shader(GL_VERTEX_SHADER,   scene_pick_vert_src);
    GLuint pfs = compile_shader(GL_FRAGMENT_SHADER, scene_pick_frag_src);
    s->pick_shader = glCreateProgram();
    glAttachShader(s->pick_shader, pvs);
    glAttachShader(s->pick_shader, pfs);
    glLinkProgram(s->pick_shader);
    glDeleteShader(pvs);
    glDeleteShader(pfs);
    s->pick_loc_mvp   = glGetUniformLocation(s->pick_shader, "u_mvp");
    s->pick_loc_model = glGetUniformLocation(s->pick_shader, "u_model");
    s->pick_loc_id    = glGetUniformLocation(s->pick_shader, "u_id");

    s->selected_node = -1;
    s->hover_node    = -1;

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
    if (s->pick_shader)    glDeleteProgram(s->pick_shader);
    if (s->pick_fbo)       glDeleteFramebuffers(1, &s->pick_fbo);
    if (s->pick_color_tex) glDeleteTextures(1, &s->pick_color_tex);
    if (s->pick_depth_rbo) glDeleteRenderbuffers(1, &s->pick_depth_rbo);
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

/* Compute per-node world-space XY AABBs by transforming each node's local
 * mesh AABB corners through its world_xform. Caller owns the returned buffer.
 * box[i] = {x0, y0, x1, y1, valid}. */
typedef struct { float x0, y0, x1, y1; int valid; } NodeAABB2;

static NodeAABB2 *compute_node_aabbs(Scene *s) {
    NodeAABB2 *boxes = calloc((size_t)s->node_count, sizeof(NodeAABB2));
    if (!boxes) return NULL;
    for (int i = 0; i < s->node_count; i++) {
        SceneNode *n = &s->nodes[i];
        if (n->mesh_id < 0 || n->mesh_id >= s->mesh_count) continue;
        SceneMesh *m = &s->meshes[n->mesh_id];
        float mn[2] = { 1e30f,  1e30f};
        float mx[2] = {-1e30f, -1e30f};
        for (int c = 0; c < 8; c++) {
            float p[3] = {
                (c & 1) ? m->bounds_max[0] : m->bounds_min[0],
                (c & 2) ? m->bounds_max[1] : m->bounds_min[1],
                (c & 4) ? m->bounds_max[2] : m->bounds_min[2],
            };
            float w[3];
            transform_point(n->world_xform, p, w);
            if (w[0] < mn[0]) mn[0] = w[0];
            if (w[1] < mn[1]) mn[1] = w[1];
            if (w[0] > mx[0]) mx[0] = w[0];
            if (w[1] > mx[1]) mx[1] = w[1];
        }
        boxes[i].x0 = mn[0]; boxes[i].y0 = mn[1];
        boxes[i].x1 = mx[0]; boxes[i].y1 = mx[1];
        boxes[i].valid = 1;
    }
    return boxes;
}

void scene_compute_groups(Scene *s) {
    if (s->node_count <= 0) return;
    NodeAABB2 *boxes = compute_node_aabbs(s);
    if (!boxes) return;

    /* Canonical peer per rect. A stroked SVG rect emits both a fill cuboid
     * and a stroke ribbon — both svg_elem=RECT, both with the same world
     * AABB. Collapsing them to a single canonical index means "same lane"
     * is a single integer compare at render time regardless of which peer
     * the user happened to click. */
    int *canon = malloc((size_t)s->node_count * sizeof(int));
    if (!canon) { free(boxes); return; }
    for (int i = 0; i < s->node_count; i++) canon[i] = -1;
    for (int i = 0; i < s->node_count; i++) {
        if (s->nodes[i].svg_elem != SVG_ELEM_RECT || !boxes[i].valid) continue;
        canon[i] = i;
        for (int j = 0; j < i; j++) {
            if (s->nodes[j].svg_elem != SVG_ELEM_RECT || !boxes[j].valid) continue;
            float eps = 1.0f;
            if (fabsf(boxes[i].x0 - boxes[j].x0) < eps &&
                fabsf(boxes[i].y0 - boxes[j].y0) < eps &&
                fabsf(boxes[i].x1 - boxes[j].x1) < eps &&
                fabsf(boxes[i].y1 - boxes[j].y1) < eps) {
                canon[i] = canon[j];
                break;
            }
        }
    }

    /* Each node adopts the canonical peer of the smallest rect it lives in.
     * Rects resolve to their own canonical so group_id == group_id works
     * uniformly in the focus test. Lines use AABB intersection instead of
     * center-inside — a connector stem's center often lies above/below the
     * lane strip it belongs to. Events (circles/ellipses/paths) still use
     * center-inside since they're usually drawn inside their lane. */
    for (int i = 0; i < s->node_count; i++) {
        SceneNode *n = &s->nodes[i];
        n->group_id = (n->svg_elem == SVG_ELEM_RECT) ? canon[i] : -1;
        if (!boxes[i].valid || n->svg_elem == SVG_ELEM_RECT) continue;

        float cx = (boxes[i].x0 + boxes[i].x1) * 0.5f;
        float cy = (boxes[i].y0 + boxes[i].y1) * 0.5f;

        int   best = -1;
        float best_area = 1e30f;
        for (int j = 0; j < s->node_count; j++) {
            if (i == j) continue;
            if (s->nodes[j].svg_elem != SVG_ELEM_RECT) continue;
            if (!boxes[j].valid) continue;
            int inside;
            if (n->svg_elem == SVG_ELEM_LINE) {
                inside = !(boxes[i].x1 < boxes[j].x0 ||
                           boxes[i].x0 > boxes[j].x1 ||
                           boxes[i].y1 < boxes[j].y0 ||
                           boxes[i].y0 > boxes[j].y1);
            } else {
                inside = (cx >= boxes[j].x0 && cx <= boxes[j].x1 &&
                          cy >= boxes[j].y0 && cy <= boxes[j].y1);
            }
            if (!inside) continue;
            float area = (boxes[j].x1 - boxes[j].x0) * (boxes[j].y1 - boxes[j].y0);
            if (area < best_area) { best_area = area; best = j; }
        }
        n->group_id = (best >= 0) ? canon[best] : -1;
    }

    free(canon);
    free(boxes);
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

    /* Focus gating: when the user has picked something, every other node
     * dims to push the selection to the front. Lines (connector stems) are
     * visual clutter at full brightness, so they're always faded a bit and
     * dim further when they're not the focus. Selecting or hovering a lane
     * rect pulls every grouped child into focus too, so a whole timeline
     * lane brightens at once. */
    int has_selection = (s->selected_node >= 0 && s->selected_node < s->node_count);
    int has_hover     = (s->hover_node    >= 0 && s->hover_node    < s->node_count);
    int sel_is_group  = has_selection &&
                        s->nodes[s->selected_node].svg_elem == SVG_ELEM_RECT;
    int hov_is_group  = has_hover &&
                        s->nodes[s->hover_node].svg_elem == SVG_ELEM_RECT;
    int sel_group_key = sel_is_group ? s->nodes[s->selected_node].group_id : -1;
    int hov_group_key = hov_is_group ? s->nodes[s->hover_node].group_id    : -1;

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

        int is_focus = (i == s->selected_node) || (i == s->hover_node);
        if (!is_focus && sel_is_group && n->group_id == sel_group_key) is_focus = 1;
        if (!is_focus && hov_is_group && n->group_id == hov_group_key) is_focus = 1;
        int is_line  = (n->svg_elem == SVG_ELEM_LINE);

        /* Base attenuation: stems are always softer than fills. */
        float rgb_mul = 1.0f, alpha_mul = 1.0f;
        if (is_line && !is_focus) { rgb_mul = 0.55f; alpha_mul = 0.40f; }

        /* Stronger dim when there's a selection: pushes non-focus nodes into
         * the background so the selection reads as the subject. */
        if (has_selection && !is_focus) {
            rgb_mul   *= 0.30f;
            alpha_mul *= 0.55f;
        }

        fill[0] *= rgb_mul; fill[1] *= rgb_mul; fill[2] *= rgb_mul;
        fill[3] *= alpha_mul;

        /* Highlight selected / hovered nodes with an additive tint. Selected
         * reads brighter than hover so the two states are distinguishable
         * when the cursor lingers on the active selection. */
        float tint[3] = {0, 0, 0};
        if (i == s->selected_node) { tint[0] = 0.25f; tint[1] = 0.35f; tint[2] = 0.55f; }
        else if (i == s->hover_node) { tint[0] = 0.10f; tint[1] = 0.15f; tint[2] = 0.25f; }

        glUniformMatrix4fv(s->loc_model, 1, GL_FALSE, n->world_xform);
        glUniform4fv(s->loc_fill, 1, fill);
        if (s->loc_tint >= 0) glUniform3fv(s->loc_tint, 1, tint);
        glDrawElements(GL_TRIANGLES, (GLsizei)m->ib_count, GL_UNSIGNED_INT,
                       (void *)(uintptr_t)(m->ib_offset * sizeof(uint32_t)));
    }
    glBindVertexArray(0);
}

/* ---------------------------------------------------------------- pick */

static void pick_fbo_resize(Scene *s, int w, int h) {
    if (s->pick_fbo && s->pick_w == w && s->pick_h == h) return;

    if (!s->pick_fbo) {
        glGenFramebuffers(1, &s->pick_fbo);
        glGenTextures(1, &s->pick_color_tex);
        glGenRenderbuffers(1, &s->pick_depth_rbo);
    }

    glBindTexture(GL_TEXTURE_2D, s->pick_color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, w, h, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindRenderbuffer(GL_RENDERBUFFER, s->pick_depth_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, s->pick_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, s->pick_color_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, s->pick_depth_rbo);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "scene: pick fbo incomplete: 0x%x\n", status);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    s->pick_w = w;
    s->pick_h = h;
}

int scene_pick(Scene *s, const float mvp[16], int px, int py, int fb_w, int fb_h) {
    if (!s->uploaded || s->node_count == 0) return -1;
    if (fb_w <= 0 || fb_h <= 0) return -1;
    if (px < 0 || py < 0 || px >= fb_w || py >= fb_h) return -1;

    pick_fbo_resize(s, fb_w, fb_h);
    if (!s->pick_fbo) return -1;

    glBindFramebuffer(GL_FRAMEBUFFER, s->pick_fbo);
    glViewport(0, 0, fb_w, fb_h);
    /* Clear to 0 = "miss". integer clear — floats would be normalized. */
    GLuint clear_zero[4] = {0, 0, 0, 0};
    glClearBufferuiv(GL_COLOR, 0, clear_zero);
    glClear(GL_DEPTH_BUFFER_BIT);

    if (s->depth_enabled) glEnable(GL_DEPTH_TEST);
    else                  glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);    /* blending on GL_R32UI is undefined */

    glUseProgram(s->pick_shader);
    glUniformMatrix4fv(s->pick_loc_mvp, 1, GL_FALSE, mvp);
    glBindVertexArray(s->vao);

    for (int i = 0; i < s->node_count; i++) {
        const SceneNode *n = &s->nodes[i];
        if (n->mesh_id < 0 || !(n->flags & SCENE_NODE_VISIBLE)) continue;
        if (!(n->flags & SCENE_NODE_SELECTABLE)) continue;
        if (n->mesh_id >= s->mesh_count) continue;
        const SceneMesh *m = &s->meshes[n->mesh_id];
        if (m->ib_count == 0) continue;

        glUniformMatrix4fv(s->pick_loc_model, 1, GL_FALSE, n->world_xform);
        glUniform1ui(s->pick_loc_id, (GLuint)(i + 1));
        glDrawElements(GL_TRIANGLES, (GLsizei)m->ib_count, GL_UNSIGNED_INT,
                       (void *)(uintptr_t)(m->ib_offset * sizeof(uint32_t)));
    }
    glBindVertexArray(0);

    GLuint id = 0;
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(px, py, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &id);

    glEnable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (id == 0) return -1;
    int idx = (int)id - 1;
    if (idx < 0 || idx >= s->node_count) return -1;
    return idx;
}
