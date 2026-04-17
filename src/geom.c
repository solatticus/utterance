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
                              GVec3 *out_verts, uint32_t *out_indices) {
    if (n < 3 || !out_verts || !out_indices) return 0;

    /* 0..n-1: back cap at z=0 ; n..2n-1: front cap at z=depth. */
    for (uint32_t i = 0; i < n; i++) {
        out_verts[i].x     = pts[i].x;
        out_verts[i].y     = pts[i].y;
        out_verts[i].z     = 0.0f;
        out_verts[n + i].x = pts[i].x;
        out_verts[n + i].y = pts[i].y;
        out_verts[n + i].z = depth;
    }

    /* Triangulate the cap once — same topology works for both caps because
     * both rings use the same vertex ordering. geom_triangulate already
     * orients to CCW internally (front cap in world space). */
    uint32_t cap_tris = geom_triangulate(pts, n, out_indices);
    if (cap_tris == 0) return 0;

    uint32_t ic = cap_tris * 3;
    /* Back cap: reverse winding so its normal points into -z (outward when the
     * front cap's normal points +z). Also offset back-cap indices stay in the
     * [0, n) range — they're already there from triangulate's direct write. */
    for (uint32_t t = 0; t < cap_tris; t++) {
        uint32_t a = out_indices[t * 3 + 0];
        uint32_t b = out_indices[t * 3 + 1];
        uint32_t c = out_indices[t * 3 + 2];
        /* Flip winding for the back cap. */
        out_indices[t * 3 + 0] = a;
        out_indices[t * 3 + 1] = c;
        out_indices[t * 3 + 2] = b;
    }

    /* Emit a second copy for the front cap — same triangulation, indices
     * offset by n to point at the top ring, with original winding restored. */
    for (uint32_t t = 0; t < cap_tris; t++) {
        uint32_t a = out_indices[t * 3 + 0];
        uint32_t b = out_indices[t * 3 + 2]; /* swap back */
        uint32_t c = out_indices[t * 3 + 1];
        out_indices[ic++] = a + n;
        out_indices[ic++] = b + n;
        out_indices[ic++] = c + n;
    }

    /* Side quads. For each edge (i, i+1) the four corners are:
     *   bi = i, bj = (i+1)%n  on the back ring
     *   ti = n+i, tj = n + (i+1)%n  on the front ring
     *
     * Determine winding so side-quad normals point outward. For CCW polygons
     * (signed area > 0), the outward face is (bi, bj, tj, ti). For CW it's
     * reversed. Using the signed area test keeps this consistent whatever
     * orientation the caller supplied. */
    int ccw = polygon_signed_area(pts, n) > 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j  = (i + 1) % n;
        uint32_t bi = i;
        uint32_t bj = j;
        uint32_t ti = n + i;
        uint32_t tj = n + j;
        if (ccw) {
            out_indices[ic++] = bi; out_indices[ic++] = tj; out_indices[ic++] = ti;
            out_indices[ic++] = bi; out_indices[ic++] = bj; out_indices[ic++] = tj;
        } else {
            out_indices[ic++] = bi; out_indices[ic++] = ti; out_indices[ic++] = tj;
            out_indices[ic++] = bi; out_indices[ic++] = tj; out_indices[ic++] = bj;
        }
    }
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
