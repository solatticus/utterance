#include "geom.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- utility */

static void vec_append(GVec2 v, GVec2 **out, uint32_t *count, uint32_t *cap) {
    if (*count >= *cap) {
        uint32_t nc = *cap ? *cap * 2 : 64;
        GVec2 *tmp = realloc(*out, (size_t)nc * sizeof(GVec2));
        if (!tmp) return;
        *out = tmp;
        *cap = nc;
    }
    (*out)[(*count)++] = v;
}

static float cross2(GVec2 a, GVec2 b) { return a.x * b.y - a.y * b.x; }
static GVec2 sub2(GVec2 a, GVec2 b)   { return (GVec2){a.x - b.x, a.y - b.y}; }

/* Perpendicular distance from p to the line through a and b, squared scale. */
static float point_line_dist_sq(GVec2 p, GVec2 a, GVec2 b) {
    GVec2 ab = sub2(b, a);
    GVec2 ap = sub2(p, a);
    float c  = cross2(ab, ap);
    float len_sq = ab.x * ab.x + ab.y * ab.y;
    if (len_sq <= 0.0f) {
        float dx = p.x - a.x, dy = p.y - a.y;
        return dx * dx + dy * dy;
    }
    return (c * c) / len_sq;
}

/* ---------------------------------------------------------------- flatten */

static void flatten_cubic_rec(GVec2 p0, GVec2 p1, GVec2 p2, GVec2 p3,
                              float flat_sq, int depth,
                              GVec2 **out, uint32_t *count, uint32_t *cap) {
    if (depth > 16) return; /* bail on pathological curves */
    float d1 = point_line_dist_sq(p1, p0, p3);
    float d2 = point_line_dist_sq(p2, p0, p3);
    float d  = d1 > d2 ? d1 : d2;
    if (d <= flat_sq) {
        /* Close enough — approximate with straight line; emit midpoint so
         * the chord has *some* curvature signal. p3 is the caller's job. */
        return;
    }
    /* de Casteljau split at t=0.5 */
    GVec2 p01  = {(p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f};
    GVec2 p12  = {(p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f};
    GVec2 p23  = {(p2.x + p3.x) * 0.5f, (p2.y + p3.y) * 0.5f};
    GVec2 p012 = {(p01.x + p12.x) * 0.5f, (p01.y + p12.y) * 0.5f};
    GVec2 p123 = {(p12.x + p23.x) * 0.5f, (p12.y + p23.y) * 0.5f};
    GVec2 mid  = {(p012.x + p123.x) * 0.5f, (p012.y + p123.y) * 0.5f};

    flatten_cubic_rec(p0, p01, p012, mid, flat_sq, depth + 1, out, count, cap);
    vec_append(mid, out, count, cap);
    flatten_cubic_rec(mid, p123, p23, p3, flat_sq, depth + 1, out, count, cap);
}

void geom_flatten_cubic(GVec2 p0, GVec2 p1, GVec2 p2, GVec2 p3, float flatness,
                        GVec2 **out, uint32_t *out_count, uint32_t *out_cap) {
    if (flatness <= 0.0f) flatness = 0.25f;
    flatten_cubic_rec(p0, p1, p2, p3, flatness * flatness, 0,
                      out, out_count, out_cap);
}

static void flatten_quad_rec(GVec2 p0, GVec2 p1, GVec2 p2,
                             float flat_sq, int depth,
                             GVec2 **out, uint32_t *count, uint32_t *cap) {
    if (depth > 16) return;
    float d = point_line_dist_sq(p1, p0, p2);
    if (d <= flat_sq) return;
    GVec2 p01 = {(p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f};
    GVec2 p12 = {(p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f};
    GVec2 mid = {(p01.x + p12.x) * 0.5f, (p01.y + p12.y) * 0.5f};

    flatten_quad_rec(p0, p01, mid, flat_sq, depth + 1, out, count, cap);
    vec_append(mid, out, count, cap);
    flatten_quad_rec(mid, p12, p2, flat_sq, depth + 1, out, count, cap);
}

void geom_flatten_quadratic(GVec2 p0, GVec2 p1, GVec2 p2, float flatness,
                            GVec2 **out, uint32_t *out_count, uint32_t *out_cap) {
    if (flatness <= 0.0f) flatness = 0.25f;
    flatten_quad_rec(p0, p1, p2, flatness * flatness, 0,
                     out, out_count, out_cap);
}

/* ---------------------------------------------------------------- triangulate */

static float polygon_signed_area(const GVec2 *p, uint32_t n) {
    float a = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = (i + 1) % n;
        a += p[i].x * p[j].y - p[j].x * p[i].y;
    }
    return a * 0.5f;
}

static int point_in_tri(GVec2 p, GVec2 a, GVec2 b, GVec2 c) {
    /* Barycentric sign test — strict inside only (shared edges are fine). */
    float d1 = (p.x - b.x) * (a.y - b.y) - (a.x - b.x) * (p.y - b.y);
    float d2 = (p.x - c.x) * (b.y - c.y) - (b.x - c.x) * (p.y - c.y);
    float d3 = (p.x - a.x) * (c.y - a.y) - (c.x - a.x) * (p.y - a.y);
    int has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    int has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(has_neg && has_pos);
}

uint32_t geom_triangulate(const GVec2 *pts, uint32_t n, uint32_t *out_indices) {
    if (n < 3) return 0;

    /* Orient CCW — flip traversal if input is CW so ear test is consistent. */
    int reversed = polygon_signed_area(pts, n) < 0.0f;

    uint32_t *idx = malloc(n * sizeof(uint32_t));
    if (!idx) return 0;
    for (uint32_t i = 0; i < n; i++)
        idx[i] = reversed ? (n - 1 - i) : i;

    uint32_t remaining = n;
    uint32_t tri_count = 0;
    uint32_t guard = 0;

    while (remaining > 2) {
        int ear_found = 0;
        for (uint32_t i = 0; i < remaining; i++) {
            uint32_t ia = idx[(i + remaining - 1) % remaining];
            uint32_t ib = idx[i];
            uint32_t ic = idx[(i + 1) % remaining];
            GVec2 a = pts[ia], b = pts[ib], c = pts[ic];

            /* Convex test (CCW polygon → ear has positive cross). */
            float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (cross <= 0.0f) continue;

            /* No other vertex inside triangle abc. */
            int clear = 1;
            for (uint32_t k = 0; k < remaining; k++) {
                if (k == (i + remaining - 1) % remaining ||
                    k == i ||
                    k == (i + 1) % remaining) continue;
                if (point_in_tri(pts[idx[k]], a, b, c)) { clear = 0; break; }
            }
            if (!clear) continue;

            out_indices[tri_count * 3 + 0] = ia;
            out_indices[tri_count * 3 + 1] = ib;
            out_indices[tri_count * 3 + 2] = ic;
            tri_count++;

            /* Remove vertex i from ring. */
            for (uint32_t k = i; k < remaining - 1; k++) idx[k] = idx[k + 1];
            remaining--;
            ear_found = 1;
            break;
        }
        if (!ear_found) break; /* degenerate / self-intersecting — give up */
        if (++guard > n * n) break;
    }

    free(idx);
    return tri_count;
}

/* ---------------------------------------------------------------- extrude */

uint32_t geom_extrude_polygon(const GVec2 *pts, uint32_t n, float depth,
                              GVec3 *out_verts, GVec3 *out_normals,
                              uint32_t *out_indices, uint32_t *out_vert_count) {
    if (n < 3 || !out_verts || !out_normals || !out_indices) return 0;

    /* Cap verts: 0..n-1 back (z=0), n..2n-1 front (z=depth). Per-cap flat
     * normals applied uniformly. */
    for (uint32_t i = 0; i < n; i++) {
        out_verts[i].x = pts[i].x;     out_verts[i].y = pts[i].y;     out_verts[i].z = 0.0f;
        out_verts[n + i].x = pts[i].x; out_verts[n + i].y = pts[i].y; out_verts[n + i].z = depth;
        out_normals[i].x = 0;     out_normals[i].y = 0;     out_normals[i].z = -1.0f;
        out_normals[n + i].x = 0; out_normals[n + i].y = 0; out_normals[n + i].z =  1.0f;
    }

    /* Triangulate the front cap (CCW in world). Reuse that topology for the
     * back cap with flipped winding so its face points into -z. */
    uint32_t cap_tris = geom_triangulate(pts, n, out_indices);
    if (cap_tris == 0) return 0;

    uint32_t ic = cap_tris * 3;
    /* Back cap: reverse winding on the indices already written in place. */
    for (uint32_t t = 0; t < cap_tris; t++) {
        uint32_t a = out_indices[t * 3 + 0];
        uint32_t b = out_indices[t * 3 + 1];
        uint32_t c = out_indices[t * 3 + 2];
        out_indices[t * 3 + 0] = a;
        out_indices[t * 3 + 1] = c;
        out_indices[t * 3 + 2] = b;
    }

    /* Front cap: same triangulation, indices + n to reach the upper ring,
     * original winding restored. */
    for (uint32_t t = 0; t < cap_tris; t++) {
        uint32_t a = out_indices[t * 3 + 0];
        uint32_t b = out_indices[t * 3 + 2];  /* swap back to CCW */
        uint32_t c = out_indices[t * 3 + 1];
        out_indices[ic++] = a + n;
        out_indices[ic++] = b + n;
        out_indices[ic++] = c + n;
    }

    /* Side quads. Four fresh verts per edge so each face gets its own flat
     * normal — no seam averaging with the cap or neighboring walls. */
    int ccw = polygon_signed_area(pts, n) > 0.0f;
    uint32_t base = 2 * n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = (i + 1) % n;
        GVec2 a = pts[i], b = pts[j];
        /* Outward normal of edge (a → b) for a CCW polygon is the right-hand
         * perpendicular; for CW, the left-hand. Normalize; z component is 0
         * (side faces are axis-aligned along +z extrusion). */
        float ex = b.x - a.x, ey = b.y - a.y;
        float nx, ny;
        if (ccw) { nx =  ey; ny = -ex; }
        else     { nx = -ey; ny =  ex; }
        float nl = sqrtf(nx * nx + ny * ny);
        if (nl > 0) { nx /= nl; ny /= nl; }

        uint32_t v0 = base + i * 4 + 0;  /* back-a  */
        uint32_t v1 = base + i * 4 + 1;  /* back-b  */
        uint32_t v2 = base + i * 4 + 2;  /* front-b */
        uint32_t v3 = base + i * 4 + 3;  /* front-a */
        out_verts[v0].x = a.x; out_verts[v0].y = a.y; out_verts[v0].z = 0.0f;
        out_verts[v1].x = b.x; out_verts[v1].y = b.y; out_verts[v1].z = 0.0f;
        out_verts[v2].x = b.x; out_verts[v2].y = b.y; out_verts[v2].z = depth;
        out_verts[v3].x = a.x; out_verts[v3].y = a.y; out_verts[v3].z = depth;
        for (int k = 0; k < 4; k++) {
            out_normals[v0 + k].x = nx;
            out_normals[v0 + k].y = ny;
            out_normals[v0 + k].z = 0.0f;
        }

        /* Winding: quad a-b-b'-a' seen from outside. For both CCW and CW input
         * the tri pair (v0,v1,v2)+(v0,v2,v3) is correct once nx,ny point out. */
        out_indices[ic++] = v0; out_indices[ic++] = v1; out_indices[ic++] = v2;
        out_indices[ic++] = v0; out_indices[ic++] = v2; out_indices[ic++] = v3;
    }

    if (out_vert_count) *out_vert_count = 2 * n + 4 * n;
    return ic / 3;
}

/* ---------------------------------------------------------------- cuboid */

static GVec3 tri_normal(GVec3 a, GVec3 b, GVec3 c) {
    float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
    GVec3 n = {uy * vz - uz * vy,
               uz * vx - ux * vz,
               ux * vy - uy * vx};
    float ln = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
    if (ln > 0) { n.x /= ln; n.y /= ln; n.z /= ln; }
    return n;
}

uint32_t geom_build_cuboid(GVec2 p0, GVec2 p1, GVec2 p2, GVec2 p3, float depth,
                           GVec3 *out_verts, GVec3 *out_normals,
                           uint32_t *out_indices) {
    if (!out_verts || !out_normals || !out_indices) return 0;

    /* Ensure the 4 bottom corners wind CCW looking down +z so face normals
     * computed below point outward. */
    GVec2 q[4] = {p0, p1, p2, p3};
    if (polygon_signed_area(q, 4) < 0.0f) {
        GVec2 t = q[1]; q[1] = q[3]; q[3] = t;
    }

    GVec3 bot[4] = {
        {q[0].x, q[0].y, 0.0f},
        {q[1].x, q[1].y, 0.0f},
        {q[2].x, q[2].y, 0.0f},
        {q[3].x, q[3].y, 0.0f},
    };
    GVec3 top[4] = {
        {q[0].x, q[0].y, depth},
        {q[1].x, q[1].y, depth},
        {q[2].x, q[2].y, depth},
        {q[3].x, q[3].y, depth},
    };

    /* Face table: for each of 6 faces we list 4 verts in CCW order seen from
     * outside, followed by a face-normal source triangle. Layout:
     *   face 0: bottom (-z) — bot reversed
     *   face 1: top    (+z) — top
     *   faces 2..5: sides for edges (0,1), (1,2), (2,3), (3,0)
     */
    GVec3 face_verts[6][4] = {
        {bot[0], bot[3], bot[2], bot[1]},                 /* bottom */
        {top[0], top[1], top[2], top[3]},                 /* top    */
        {bot[0], bot[1], top[1], top[0]},                 /* side 0→1 */
        {bot[1], bot[2], top[2], top[1]},                 /* side 1→2 */
        {bot[2], bot[3], top[3], top[2]},                 /* side 2→3 */
        {bot[3], bot[0], top[0], top[3]},                 /* side 3→0 */
    };

    uint32_t v = 0, ic = 0;
    for (int f = 0; f < 6; f++) {
        GVec3 n = tri_normal(face_verts[f][0], face_verts[f][1], face_verts[f][2]);
        uint32_t base = v;
        for (int k = 0; k < 4; k++) {
            out_verts[v]   = face_verts[f][k];
            out_normals[v] = n;
            v++;
        }
        out_indices[ic++] = base;
        out_indices[ic++] = base + 1;
        out_indices[ic++] = base + 2;
        out_indices[ic++] = base;
        out_indices[ic++] = base + 2;
        out_indices[ic++] = base + 3;
    }
    return ic / 3;
}

/* ---------------------------------------------------------------- cylinder */

uint32_t geom_build_cylinder(const GVec2 *in_ring, uint32_t n, float depth,
                             GVec3 *out_verts, GVec3 *out_normals,
                             uint32_t *out_indices) {
    if (n < 3 || !in_ring || !out_verts || !out_normals || !out_indices) return 0;

    /* Auto-orient to CCW so face winding and radial normals stay consistent
     * regardless of caller sign conventions. One bounded malloc at build time
     * — cheap and matches the rest of the load-time pipeline. */
    int reverse = polygon_signed_area(in_ring, n) < 0.0f;
    GVec2 *ring = malloc((size_t)n * sizeof(GVec2));
    if (!ring) return 0;
    for (uint32_t i = 0; i < n; i++)
        ring[i] = reverse ? in_ring[n - 1 - i] : in_ring[i];

    /* Centroid for cap centers — averages to the geometric center for any
     * uniformly-sampled convex ring. */
    float cx = 0, cy = 0;
    for (uint32_t i = 0; i < n; i++) { cx += ring[i].x; cy += ring[i].y; }
    cx /= (float)n; cy /= (float)n;

    uint32_t v = 0, ic = 0;

    /* Bottom cap: center + ring, normal (0,0,-1). Winding flipped so the face
     * points -z outward. */
    uint32_t bot_center = v;
    out_verts[v]   = (GVec3){cx, cy, 0.0f};
    out_normals[v] = (GVec3){0.0f, 0.0f, -1.0f};
    v++;
    uint32_t bot_ring = v;
    for (uint32_t i = 0; i < n; i++) {
        out_verts[v]   = (GVec3){ring[i].x, ring[i].y, 0.0f};
        out_normals[v] = (GVec3){0.0f, 0.0f, -1.0f};
        v++;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = (i + 1) % n;
        out_indices[ic++] = bot_center;
        out_indices[ic++] = bot_ring + j;
        out_indices[ic++] = bot_ring + i;
    }

    /* Top cap. Normal (0,0,+1), standard CCW winding. */
    uint32_t top_center = v;
    out_verts[v]   = (GVec3){cx, cy, depth};
    out_normals[v] = (GVec3){0.0f, 0.0f, 1.0f};
    v++;
    uint32_t top_ring = v;
    for (uint32_t i = 0; i < n; i++) {
        out_verts[v]   = (GVec3){ring[i].x, ring[i].y, depth};
        out_normals[v] = (GVec3){0.0f, 0.0f, 1.0f};
        v++;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = (i + 1) % n;
        out_indices[ic++] = top_center;
        out_indices[ic++] = top_ring + i;
        out_indices[ic++] = top_ring + j;
    }

    /* Side ring verts — separate from cap verts so side normals can differ.
     * Each vertex gets a smooth outward normal computed from the tangent at
     * that ring position (centered finite difference). Right-perpendicular
     * of the CCW tangent points outward. */
    uint32_t side_bot = v;
    uint32_t side_top = v + n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t prev = (i + n - 1) % n;
        uint32_t next = (i + 1) % n;
        float tx = ring[next].x - ring[prev].x;
        float ty = ring[next].y - ring[prev].y;
        float nx = ty, ny = -tx;
        float nl = sqrtf(nx * nx + ny * ny);
        if (nl > 0) { nx /= nl; ny /= nl; }

        out_verts[side_bot + i]   = (GVec3){ring[i].x, ring[i].y, 0.0f};
        out_normals[side_bot + i] = (GVec3){nx, ny, 0.0f};
        out_verts[side_top + i]   = (GVec3){ring[i].x, ring[i].y, depth};
        out_normals[side_top + i] = (GVec3){nx, ny, 0.0f};
    }
    v = side_top + n;

    /* Side quads — each edge of the ring becomes a quad (2 tris) using the
     * side ring verts. CCW winding when viewed from outside. */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = (i + 1) % n;
        uint32_t bi = side_bot + i, bj = side_bot + j;
        uint32_t ti = side_top + i, tj = side_top + j;
        out_indices[ic++] = bi; out_indices[ic++] = bj; out_indices[ic++] = tj;
        out_indices[ic++] = bi; out_indices[ic++] = tj; out_indices[ic++] = ti;
    }

    free(ring);
    (void)v;
    return ic / 3;
}

/* ---------------------------------------------------------------- stroke */

void geom_stroke_ribbon(const GVec2 *pts, uint32_t n, float half_width,
                        int closed, GVec2 *out_verts) {
    if (n < 2 || !out_verts) return;

    for (uint32_t i = 0; i < n; i++) {
        GVec2 prev, next;
        if (i == 0) {
            prev = closed ? pts[n - 1] : pts[0];
            next = pts[1];
        } else if (i == n - 1) {
            prev = pts[n - 2];
            next = closed ? pts[0] : pts[n - 1];
        } else {
            prev = pts[i - 1];
            next = pts[i + 1];
        }

        /* Vertex normal = normalized (perp(prev→curr) + perp(curr→next)) / 2.
         * For endpoints on an open polyline this collapses to the edge's
         * own perpendicular. */
        GVec2 e1 = sub2(pts[i], prev);
        GVec2 e2 = sub2(next, pts[i]);
        float l1 = sqrtf(e1.x * e1.x + e1.y * e1.y);
        float l2 = sqrtf(e2.x * e2.x + e2.y * e2.y);
        if (l1 > 0) { e1.x /= l1; e1.y /= l1; }
        if (l2 > 0) { e2.x /= l2; e2.y /= l2; }

        /* Perpendicular of each edge (left-hand normal). */
        GVec2 n1 = {-e1.y, e1.x};
        GVec2 n2 = {-e2.y, e2.x};
        GVec2 nrm = {n1.x + n2.x, n1.y + n2.y};
        float ln = sqrtf(nrm.x * nrm.x + nrm.y * nrm.y);
        if (ln > 0) { nrm.x /= ln; nrm.y /= ln; }
        else        { nrm = n1; }

        /* Miter length — 1/cos(half_angle). Clamp to avoid extreme spikes at
         * sharp corners; proper bevel handling is a Phase 1 upgrade. */
        float dot = nrm.x * n1.x + nrm.y * n1.y;
        float miter = dot > 0.1f ? half_width / dot : half_width * 5.0f;

        out_verts[i * 2 + 0] = (GVec2){pts[i].x + nrm.x * miter,
                                       pts[i].y + nrm.y * miter};
        out_verts[i * 2 + 1] = (GVec2){pts[i].x - nrm.x * miter,
                                       pts[i].y - nrm.y * miter};
    }
}
