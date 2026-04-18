#ifndef SCENE_H
#define SCENE_H

#include "gl_loader.h"
#include <stdint.h>

/* SVG provenance — lets Phase 2 extrusion recognize primitives and enables
 * optional SVG round-trip export. 0 = unknown / non-SVG source. */
typedef enum {
    SVG_ELEM_NONE = 0,
    SVG_ELEM_RECT,
    SVG_ELEM_CIRCLE,
    SVG_ELEM_ELLIPSE,
    SVG_ELEM_LINE,
    SVG_ELEM_POLYGON,
    SVG_ELEM_POLYLINE,
    SVG_ELEM_PATH,
    SVG_ELEM_TEXT,
} SvgElemKind;

/* Superset vertex. SVG fills pos + color. GLB/FBX use all four. VOX uses
 * pos + color (palette-indexed). Single format avoids reformatting between
 * importers and lets a single shader cover everything. */
typedef struct {
    float pos[3];
    float normal[3];
    float uv[2];
    float color[4];
} SceneVertex;

typedef struct {
    uint32_t vb_offset, vb_count;
    uint32_t ib_offset, ib_count;
    float    bounds_min[3], bounds_max[3];  /* local AABB */
} SceneMesh;

typedef struct {
    float  fill_rgba[4];
    float  stroke_rgba[4];
    float  stroke_width;
    GLuint shader;        /* 0 = default flat shader */
    GLuint albedo_tex;    /* reserved for textured/PBR materials */
    GLuint normal_tex;
    GLuint mr_tex;        /* metal/roughness */
} SceneMaterial;

#define SCENE_NODE_VISIBLE    (1u << 0)
#define SCENE_NODE_SELECTABLE (1u << 1)

typedef struct {
    float       local_xform[16];   /* local-to-parent (column-major) */
    float       world_xform[16];   /* cached; built by scene_bake_transforms */
    int         parent;            /* -1 = root */
    int         mesh_id;           /* -1 = group-only node */
    int         material_id;       /* -1 = default (white fill) */
    char       *name;              /* owned, optional */
    char       *href;              /* owned; ctrl-click link target */
    uint32_t    flags;
    SvgElemKind svg_elem;
    float       svg_params[6];     /* rect: x,y,w,h / circle: cx,cy,r / etc */
    float       depth;             /* extrusion along +z; 0 = flat */
    int         group_id;          /* index of containing rect lane, -1 = none */
} SceneNode;

typedef struct {
    SceneNode     *nodes;     int node_count,  node_cap;
    SceneMesh     *meshes;    int mesh_count,  mesh_cap;
    SceneMaterial *materials; int mat_count,   mat_cap;
    SceneVertex   *verts;     uint32_t vert_count, vert_cap;
    uint32_t      *indices;   uint32_t idx_count,  idx_cap;

    GLuint vao, vbo, ibo;
    GLuint default_shader;
    GLint  loc_mvp, loc_model, loc_fill, loc_light_dir, loc_tint;
    float  bounds_min[3], bounds_max[3];
    int    uploaded;
    int    depth_enabled;   /* 1 when nodes have real z extent */

    /* Selection state — indices into nodes[], or -1 for none. Main pass tints
     * the matching node's fill; scene_pick() sets these. */
    int    selected_node;
    int    hover_node;

    /* GPU id-buffer pick pass. FBO is allocated lazily on first pick and
     * resized when the target framebuffer size changes. R32UI color attachment
     * stores (node_index + 1) per pixel; 0 means miss. */
    GLuint pick_fbo;
    GLuint pick_color_tex;
    GLuint pick_depth_rbo;
    int    pick_w, pick_h;
    GLuint pick_shader;
    GLint  pick_loc_mvp, pick_loc_model, pick_loc_id;
} Scene;

void scene_init(Scene *s);
void scene_destroy(Scene *s);

int  scene_add_material(Scene *s, const SceneMaterial *m);
int  scene_add_mesh(Scene *s, const SceneVertex *verts, uint32_t v_count,
                    const uint32_t *indices, uint32_t i_count);
int  scene_add_node(Scene *s, const SceneNode *n);

/* Upload current vert+idx buffers to GPU. Idempotent; call after all adds
 * or whenever geometry changes. */
void scene_upload(Scene *s);

/* Walk hierarchy, compute world_xform from local_xform * parent.world. Call
 * after any local transform edit. */
void scene_bake_transforms(Scene *s);

/* Fill Scene.bounds_{min,max} from all placed nodes' mesh bounds in world
 * space. Call after scene_bake_transforms. */
void scene_compute_bounds(Scene *s);

/* For each non-rect node, find the smallest RECT node whose world-space XY
 * AABB contains the node's center. The rect's index is stored in node.group_id
 * so selection of a lane rect can co-highlight its contained events. Call
 * after scene_bake_transforms. */
void scene_compute_groups(Scene *s);

/* One draw per node (material batching is a later optimization). */
void scene_render(const Scene *s, const float mvp[16]);

/* GPU id-buffer pick. Renders every SCENE_NODE_SELECTABLE node into an R32UI
 * FBO with (node_index + 1) as its id, reads back the pixel at (px, py) in
 * OpenGL framebuffer coords (origin bottom-left), and returns the node index
 * under the cursor — or -1 on miss. Cost: one extra draw per node, executed
 * only on the frame this is called. fb_w / fb_h must match the current window
 * framebuffer size so the FBO stays correctly sized. */
int  scene_pick(Scene *s, const float mvp[16], int px, int py, int fb_w, int fb_h);

/* Utilities — identity / simple TRS for use when populating nodes. */
void scene_mat_identity(float out[16]);
void scene_mat_translate(float out[16], float x, float y, float z);
void scene_mat_mul(const float a[16], const float b[16], float out[16]);

#endif
