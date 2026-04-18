#ifndef GEOM_H
#define GEOM_H

#include <stdint.h>

typedef struct { float x, y; } GVec2;
typedef struct { float x, y, z; } GVec3;

/* --- Bezier flattening ---------------------------------------------------
 * Recursive subdivision until the control polygon is within `flatness` of
 * the straight chord. Endpoints are NOT emitted — the caller owns the start
 * and end points (typical pattern: emit p0 once at path start, then for each
 * curve call flatten which appends intermediate samples, then emit p3). */
void geom_flatten_cubic(GVec2 p0, GVec2 p1, GVec2 p2, GVec2 p3, float flatness,
                        GVec2 **out, uint32_t *out_count, uint32_t *out_cap);

void geom_flatten_quadratic(GVec2 p0, GVec2 p1, GVec2 p2, float flatness,
                            GVec2 **out, uint32_t *out_count, uint32_t *out_cap);

/* --- Triangulation -------------------------------------------------------
 * Ear clipping for a simple polygon. Assumes no self-intersections and no
 * holes (Phase 1 will swap for an earcut-with-holes when we ingest real SVG
 * paths). Orientation is auto-detected; CCW fill is emitted.
 *
 * `out_indices` must hold at least 3*(n-2) uint32_t slots. Returns the
 * number of triangles written (0 on degenerate input). */
uint32_t geom_triangulate(const GVec2 *pts, uint32_t n, uint32_t *out_indices);

/* --- Extrusion ----------------------------------------------------------
 * Extrude a 2D polygon (CCW or CW — auto-detected) along +z into a prism.
 * Back cap sits at z=0, front cap at z=depth. Side quads face outward.
 *
 * Verts are split per-face so flat per-face normals can be authored (no
 * normal averaging at cap/side seams). Buffer layout:
 *   [0..n)            back cap   (z=0,     normal = 0,0,-1)
 *   [n..2n)           front cap  (z=depth, normal = 0,0,+1)
 *   [2n..6n)          side quads — 4 verts per edge, each with the outward
 *                     face normal
 *
 * out_verts and out_normals must hold at least 6*n slots.
 * out_indices must hold at least 6*(n-2) + 6*n slots (back + front + sides).
 *
 * On success *out_vert_count is set to the exact number of verts written
 * (≤ 6*n) and the function returns the triangle count. 0 on degenerate input. */
uint32_t geom_extrude_polygon(const GVec2 *pts, uint32_t n, float depth,
                              GVec3 *out_verts, GVec3 *out_normals,
                              uint32_t *out_indices, uint32_t *out_vert_count);

/* --- Cuboid -------------------------------------------------------------
 * Build a rectangular box from 4 bottom-face corners (world-space, y-up)
 * extruded along +z by depth. Emits 24 verts (4 per face × 6 faces) with flat
 * per-face normals, 36 indices (2 tris × 6 faces). Corners should be ordered
 * CCW around the bottom face when viewed from +z.
 *
 * out_verts/out_normals: ≥ 24 slots. out_indices: ≥ 36 slots.
 * Returns tri count (12) on success, 0 on degenerate input. */
uint32_t geom_build_cuboid(GVec2 p0, GVec2 p1, GVec2 p2, GVec2 p3, float depth,
                           GVec3 *out_verts, GVec3 *out_normals,
                           uint32_t *out_indices);

/* --- Cylinder -----------------------------------------------------------
 * Build an n-sided prism ("cylinder") from a pre-computed bottom ring in
 * world-space y-up. Flat caps (±z normals), smooth sides — side vertex
 * normals are perpendicular to the local tangent (centered finite
 * difference), so ellipses and any rotated ring get the right silhouette.
 *
 * The bottom ring should be CCW looking down +z; if signed area is negative
 * the builder reverses internally so callers don't have to worry about a
 * negative-scale transform flipping orientation.
 *
 * Vert layout (2 + 4n total):
 *   [0]               bottom cap center   (normal 0,0,-1)
 *   [1 .. 1+n)        bottom cap ring     (normal 0,0,-1)
 *   [1+n]             top    cap center   (normal 0,0,+1)
 *   [2+n .. 2+2n)     top    cap ring     (normal 0,0,+1)
 *   [2+2n .. 2+3n)    side bottom ring    (radial/perpendicular normal)
 *   [2+3n .. 2+4n)    side top    ring    (same normal as matching bottom)
 *
 * out_verts/out_normals: ≥ 2 + 4n slots. out_indices: ≥ 12n slots.
 * Returns tri count (4n) on success, 0 on degenerate input. */
uint32_t geom_build_cylinder(const GVec2 *ring, uint32_t n, float depth,
                             GVec3 *out_verts, GVec3 *out_normals,
                             uint32_t *out_indices);

/* --- Stroke ribbon -------------------------------------------------------
 * Build a triangle strip offset ±half_width from a polyline. Writes 2*n
 * vertices to out_verts: pairs (left, right) per input point. Joins are
 * straight offsets (no miter/bevel yet — Phase 1 upgrade). */
void geom_stroke_ribbon(const GVec2 *pts, uint32_t n, float half_width,
                        int closed, GVec2 *out_verts);

#endif
