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
 * Back cap sits at z=0, front cap at z=depth. Side quads face outward with
 * consistent winding.
 *
 * Vertex layout in `out_verts` (length 2*n):
 *   [0..n)   back cap   (z=0)
 *   [n..2n)  front cap  (z=depth)
 *
 * Index layout in `out_indices`:
 *   cap triangles first (2 * 3*(n-2) indices for back + front), then 3*2*n
 *   indices for the n side quads as two triangles each.
 *
 *   out_indices must hold at least 6*(n-2) + 6*n uint32_t slots.
 *
 * The caller owns both buffers. The only heap allocation inside is a single
 * geom_triangulate call (for the cap), which uses its own scratch.
 *
 * Returns the number of triangles written, 0 on degenerate input. */
uint32_t geom_extrude_polygon(const GVec2 *pts, uint32_t n, float depth,
                              GVec3 *out_verts, uint32_t *out_indices);

/* --- Stroke ribbon -------------------------------------------------------
 * Build a triangle strip offset ±half_width from a polyline. Writes 2*n
 * vertices to out_verts: pairs (left, right) per input point. Joins are
 * straight offsets (no miter/bevel yet — Phase 1 upgrade). */
void geom_stroke_ribbon(const GVec2 *pts, uint32_t n, float half_width,
                        int closed, GVec2 *out_verts);

#endif
