/* SPDX-FileCopyrightText: Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "DNA_meshdata_types.h"
#include "DNA_pointcloud_types.h"

#include "BLI_math_geom.h"
#include "BLI_task.h"

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_editmesh.hh"
#include "BKE_mesh.hh"

using blender::BitSpan;
using blender::BitVector;
using blender::float3;
using blender::IndexRange;
using blender::int2;
using blender::int3;
using blender::Span;
using blender::VArray;

/* -------------------------------------------------------------------- */
/** \name BVHCache
 * \{ */

namespace blender::bke {

BVHCacheItem::BVHCacheItem() = default;
BVHCacheItem::~BVHCacheItem()
{
  BLI_bvhtree_free(this->tree);
}

}  // namespace blender::bke

using blender::bke::BVHCacheItem;

static void bvhtree_balance(BVHTree *tree)
{
  if (tree) {
    BLI_bvhtree_balance(tree);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Local Callbacks
 * \{ */

/* Math stuff for ray casting on mesh faces and for nearest surface */

float bvhtree_ray_tri_intersection(const BVHTreeRay *ray,
                                   const float /*m_dist*/,
                                   const float v0[3],
                                   const float v1[3],
                                   const float v2[3])
{
  float dist;

#ifdef USE_KDOPBVH_WATERTIGHT
  if (isect_ray_tri_watertight_v3(ray->origin, ray->isect_precalc, v0, v1, v2, &dist, nullptr))
#else
  if (isect_ray_tri_epsilon_v3(
          ray->origin, ray->direction, v0, v1, v2, &dist, nullptr, FLT_EPSILON))
#endif
  {
    return dist;
  }

  return FLT_MAX;
}

float bvhtree_sphereray_tri_intersection(const BVHTreeRay *ray,
                                         float radius,
                                         const float m_dist,
                                         const float v0[3],
                                         const float v1[3],
                                         const float v2[3])
{

  float idist;
  float p1[3];
  float hit_point[3];

  madd_v3_v3v3fl(p1, ray->origin, ray->direction, m_dist);
  if (isect_sweeping_sphere_tri_v3(ray->origin, p1, radius, v0, v1, v2, &idist, hit_point)) {
    return idist * m_dist;
  }

  return FLT_MAX;
}

/*
 * BVH from meshes callbacks
 */

/**
 * Callback to BVH-tree nearest point.
 * The tree must have been built using #bvhtree_from_mesh_faces.
 *
 * \param userdata: Must be a #BVHMeshCallbackUserdata built from the same mesh as the tree.
 */
static void mesh_faces_nearest_point(void *userdata,
                                     int index,
                                     const float co[3],
                                     BVHTreeNearest *nearest)
{
  const BVHTreeFromMesh *data = (BVHTreeFromMesh *)userdata;
  const MFace *face = data->face + index;

  const float *t0, *t1, *t2, *t3;
  t0 = data->vert_positions[face->v1];
  t1 = data->vert_positions[face->v2];
  t2 = data->vert_positions[face->v3];
  t3 = face->v4 ? &data->vert_positions[face->v4].x : nullptr;

  do {
    float nearest_tmp[3], dist_sq;

    closest_on_tri_to_point_v3(nearest_tmp, co, t0, t1, t2);
    dist_sq = len_squared_v3v3(co, nearest_tmp);

    if (dist_sq < nearest->dist_sq) {
      nearest->index = index;
      nearest->dist_sq = dist_sq;
      copy_v3_v3(nearest->co, nearest_tmp);
      normal_tri_v3(nearest->no, t0, t1, t2);
    }

    t1 = t2;
    t2 = t3;
    t3 = nullptr;

  } while (t2);
}
/* copy of function above */
static void mesh_corner_tris_nearest_point(void *userdata,
                                           int index,
                                           const float co[3],
                                           BVHTreeNearest *nearest)
{
  const BVHTreeFromMesh *data = (BVHTreeFromMesh *)userdata;
  const int3 &tri = data->corner_tris[index];
  const float *vtri_co[3] = {
      data->vert_positions[data->corner_verts[tri[0]]],
      data->vert_positions[data->corner_verts[tri[1]]],
      data->vert_positions[data->corner_verts[tri[2]]],
  };
  float nearest_tmp[3], dist_sq;

  closest_on_tri_to_point_v3(nearest_tmp, co, UNPACK3(vtri_co));
  dist_sq = len_squared_v3v3(co, nearest_tmp);

  if (dist_sq < nearest->dist_sq) {
    nearest->index = index;
    nearest->dist_sq = dist_sq;
    copy_v3_v3(nearest->co, nearest_tmp);
    normal_tri_v3(nearest->no, UNPACK3(vtri_co));
  }
}

/**
 * Callback to BVH-tree ray-cast.
 * The tree must have been built using bvhtree_from_mesh_faces.
 *
 * \param userdata: Must be a #BVHMeshCallbackUserdata built from the same mesh as the tree.
 */
static void mesh_faces_spherecast(void *userdata,
                                  int index,
                                  const BVHTreeRay *ray,
                                  BVHTreeRayHit *hit)
{
  const BVHTreeFromMesh *data = (BVHTreeFromMesh *)userdata;
  const MFace *face = &data->face[index];

  const float *t0, *t1, *t2, *t3;
  t0 = data->vert_positions[face->v1];
  t1 = data->vert_positions[face->v2];
  t2 = data->vert_positions[face->v3];
  t3 = face->v4 ? &data->vert_positions[face->v4].x : nullptr;

  do {
    float dist;
    if (ray->radius == 0.0f) {
      dist = bvhtree_ray_tri_intersection(ray, hit->dist, t0, t1, t2);
    }
    else {
      dist = bvhtree_sphereray_tri_intersection(ray, ray->radius, hit->dist, t0, t1, t2);
    }

    if (dist >= 0 && dist < hit->dist) {
      hit->index = index;
      hit->dist = dist;
      madd_v3_v3v3fl(hit->co, ray->origin, ray->direction, dist);

      normal_tri_v3(hit->no, t0, t1, t2);
    }

    t1 = t2;
    t2 = t3;
    t3 = nullptr;

  } while (t2);
}
/* copy of function above */
static void mesh_corner_tris_spherecast(void *userdata,
                                        int index,
                                        const BVHTreeRay *ray,
                                        BVHTreeRayHit *hit)
{
  const BVHTreeFromMesh *data = (BVHTreeFromMesh *)userdata;
  const Span<float3> positions = data->vert_positions;
  const int3 &tri = data->corner_tris[index];
  const float *vtri_co[3] = {
      positions[data->corner_verts[tri[0]]],
      positions[data->corner_verts[tri[1]]],
      positions[data->corner_verts[tri[2]]],
  };
  float dist;

  if (ray->radius == 0.0f) {
    dist = bvhtree_ray_tri_intersection(ray, hit->dist, UNPACK3(vtri_co));
  }
  else {
    dist = bvhtree_sphereray_tri_intersection(ray, ray->radius, hit->dist, UNPACK3(vtri_co));
  }

  if (dist >= 0 && dist < hit->dist) {
    hit->index = index;
    hit->dist = dist;
    madd_v3_v3v3fl(hit->co, ray->origin, ray->direction, dist);

    normal_tri_v3(hit->no, UNPACK3(vtri_co));
  }
}

/**
 * Callback to BVH-tree nearest point.
 * The tree must have been built using #bvhtree_from_mesh_edges.
 *
 * \param userdata: Must be a #BVHMeshCallbackUserdata built from the same mesh as the tree.
 */
static void mesh_edges_nearest_point(void *userdata,
                                     int index,
                                     const float co[3],
                                     BVHTreeNearest *nearest)
{
  const BVHTreeFromMesh *data = (BVHTreeFromMesh *)userdata;
  const Span<float3> positions = data->vert_positions;
  const blender::int2 edge = data->edges[index];
  float nearest_tmp[3], dist_sq;

  const float *t0, *t1;
  t0 = positions[edge[0]];
  t1 = positions[edge[1]];

  closest_to_line_segment_v3(nearest_tmp, co, t0, t1);
  dist_sq = len_squared_v3v3(nearest_tmp, co);

  if (dist_sq < nearest->dist_sq) {
    nearest->index = index;
    nearest->dist_sq = dist_sq;
    copy_v3_v3(nearest->co, nearest_tmp);
    sub_v3_v3v3(nearest->no, t0, t1);
    normalize_v3(nearest->no);
  }
}

/* Helper, does all the point-sphere-cast work actually. */
static void mesh_verts_spherecast_do(int index,
                                     const float v[3],
                                     const BVHTreeRay *ray,
                                     BVHTreeRayHit *hit)
{
  float dist;
  const float *r1;
  float r2[3], i1[3];
  r1 = ray->origin;
  add_v3_v3v3(r2, r1, ray->direction);

  closest_to_line_segment_v3(i1, v, r1, r2);

  /* No hit if closest point is 'behind' the origin of the ray, or too far away from it. */
  if ((dot_v3v3v3(r1, i1, r2) >= 0.0f) && ((dist = len_v3v3(r1, i1)) < hit->dist)) {
    hit->index = index;
    hit->dist = dist;
    copy_v3_v3(hit->co, i1);
  }
}

/**
 * Callback to BVH-tree ray-cast.
 * The tree must have been built using bvhtree_from_mesh_verts.
 *
 * \param userdata: Must be a #BVHMeshCallbackUserdata built from the same mesh as the tree.
 */
static void mesh_verts_spherecast(void *userdata,
                                  int index,
                                  const BVHTreeRay *ray,
                                  BVHTreeRayHit *hit)
{
  const BVHTreeFromMesh *data = (BVHTreeFromMesh *)userdata;
  const float *v = data->vert_positions[index];

  mesh_verts_spherecast_do(index, v, ray, hit);
}

/**
 * Callback to BVH-tree ray-cast.
 * The tree must have been built using bvhtree_from_mesh_edges.
 *
 * \param userdata: Must be a #BVHMeshCallbackUserdata built from the same mesh as the tree.
 */
static void mesh_edges_spherecast(void *userdata,
                                  int index,
                                  const BVHTreeRay *ray,
                                  BVHTreeRayHit *hit)
{
  const BVHTreeFromMesh *data = (BVHTreeFromMesh *)userdata;
  const Span<float3> positions = data->vert_positions;
  const blender::int2 edge = data->edges[index];

  const float radius_sq = square_f(ray->radius);
  float dist;
  const float *v1, *v2, *r1;
  float r2[3], i1[3], i2[3];
  v1 = positions[edge[0]];
  v2 = positions[edge[1]];

  /* In case we get a zero-length edge, handle it as a point! */
  if (equals_v3v3(v1, v2)) {
    mesh_verts_spherecast_do(index, v1, ray, hit);
    return;
  }

  r1 = ray->origin;
  add_v3_v3v3(r2, r1, ray->direction);

  if (isect_line_line_v3(v1, v2, r1, r2, i1, i2)) {
    /* No hit if intersection point is 'behind' the origin of the ray, or too far away from it. */
    if ((dot_v3v3v3(r1, i2, r2) >= 0.0f) && ((dist = len_v3v3(r1, i2)) < hit->dist)) {
      const float e_fac = line_point_factor_v3(i1, v1, v2);
      if (e_fac < 0.0f) {
        copy_v3_v3(i1, v1);
      }
      else if (e_fac > 1.0f) {
        copy_v3_v3(i1, v2);
      }
      /* Ensure ray is really close enough from edge! */
      if (len_squared_v3v3(i1, i2) <= radius_sq) {
        hit->index = index;
        hit->dist = dist;
        copy_v3_v3(hit->co, i2);
      }
    }
  }
}

/** \} */

/*
 * BVH builders
 */

/* -------------------------------------------------------------------- */
/** \name Common Utils
 * \{ */

static BVHTreeFromMesh bvhtree_from_mesh_setup_data(BVHTree *tree,
                                                    const BVHCacheType bvh_cache_type,
                                                    const Span<float3> positions,
                                                    const Span<blender::int2> edges,
                                                    const Span<int> corner_verts,
                                                    const Span<int3> corner_tris,
                                                    const MFace *face)
{
  BVHTreeFromMesh data{};

  data.tree = tree;

  data.vert_positions = positions;
  data.edges = edges;
  data.face = face;
  data.corner_verts = corner_verts;
  data.corner_tris = corner_tris;

  switch (bvh_cache_type) {
    case BVHTREE_FROM_VERTS:
    case BVHTREE_FROM_LOOSEVERTS:
    case BVHTREE_FROM_LOOSEVERTS_NO_HIDDEN:
      /* a nullptr nearest callback works fine
       * remember the min distance to point is the same as the min distance to BV of point */
      data.nearest_callback = nullptr;
      data.raycast_callback = mesh_verts_spherecast;
      break;
    case BVHTREE_FROM_EDGES:
    case BVHTREE_FROM_LOOSEEDGES:
    case BVHTREE_FROM_LOOSEEDGES_NO_HIDDEN:
      data.nearest_callback = mesh_edges_nearest_point;
      data.raycast_callback = mesh_edges_spherecast;
      break;
    case BVHTREE_FROM_FACES:
      data.nearest_callback = mesh_faces_nearest_point;
      data.raycast_callback = mesh_faces_spherecast;
      break;
    case BVHTREE_FROM_CORNER_TRIS:
    case BVHTREE_FROM_CORNER_TRIS_NO_HIDDEN:
      data.nearest_callback = mesh_corner_tris_nearest_point;
      data.raycast_callback = mesh_corner_tris_spherecast;
      break;
    case BVHTREE_MAX_ITEM:
      BLI_assert(false);
      break;
  }
  return data;
}

static BVHTree *bvhtree_new_common(
    float epsilon, int tree_type, int axis, int elems_num, int &elems_num_active)
{
  if (elems_num_active != -1) {
    BLI_assert(IN_RANGE_INCL(elems_num_active, 0, elems_num));
  }
  else {
    elems_num_active = elems_num;
  }

  if (elems_num_active == 0) {
    return nullptr;
  }

  return BLI_bvhtree_new(elems_num_active, epsilon, tree_type, axis);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Builder
 * \{ */

static BVHTree *bvhtree_from_mesh_verts_create_tree(float epsilon,
                                                    int tree_type,
                                                    int axis,
                                                    const Span<float3> positions,
                                                    const BitSpan verts_mask,
                                                    int verts_num_active)
{
  BVHTree *tree = bvhtree_new_common(epsilon, tree_type, axis, positions.size(), verts_num_active);
  if (!tree) {
    return nullptr;
  }

  for (const int i : positions.index_range()) {
    if (!verts_mask.is_empty() && !verts_mask[i]) {
      continue;
    }
    BLI_bvhtree_insert(tree, i, positions[i], 1);
  }
  BLI_assert(BLI_bvhtree_get_len(tree) == verts_num_active);

  return tree;
}

BVHTree *bvhtree_from_mesh_verts_ex(BVHTreeFromMesh *data,
                                    const Span<float3> vert_positions,
                                    const BitSpan verts_mask,
                                    int verts_num_active,
                                    float epsilon,
                                    int tree_type,
                                    int axis)
{
  BVHTree *tree = bvhtree_from_mesh_verts_create_tree(
      epsilon, tree_type, axis, vert_positions, verts_mask, verts_num_active);

  bvhtree_balance(tree);

  if (data) {
    /* Setup BVHTreeFromMesh */
    *data = bvhtree_from_mesh_setup_data(tree, BVHTREE_FROM_VERTS, vert_positions, {}, {}, {}, {});
  }

  return tree;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edge Builder
 * \{ */

static BVHTree *bvhtree_from_mesh_edges_create_tree(const Span<float3> positions,
                                                    const blender::Span<blender::int2> edges,
                                                    const BitSpan edges_mask,
                                                    int edges_num_active,
                                                    float epsilon,
                                                    int tree_type,
                                                    int axis)
{
  BVHTree *tree = bvhtree_new_common(epsilon, tree_type, axis, edges.size(), edges_num_active);
  if (!tree) {
    return nullptr;
  }

  for (const int i : edges.index_range()) {
    if (!edges_mask.is_empty() && !edges_mask[i]) {
      continue;
    }
    float co[2][3];
    copy_v3_v3(co[0], positions[edges[i][0]]);
    copy_v3_v3(co[1], positions[edges[i][1]]);

    BLI_bvhtree_insert(tree, i, co[0], 2);
  }

  return tree;
}

BVHTree *bvhtree_from_mesh_edges_ex(BVHTreeFromMesh *data,
                                    const Span<float3> vert_positions,
                                    const Span<blender::int2> edges,
                                    const BitSpan edges_mask,
                                    int edges_num_active,
                                    float epsilon,
                                    int tree_type,
                                    int axis)
{
  BVHTree *tree = bvhtree_from_mesh_edges_create_tree(
      vert_positions, edges, edges_mask, edges_num_active, epsilon, tree_type, axis);

  bvhtree_balance(tree);

  if (data) {
    /* Setup BVHTreeFromMesh */
    *data = bvhtree_from_mesh_setup_data(
        tree, BVHTREE_FROM_EDGES, vert_positions, edges, {}, {}, {});
  }

  return tree;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tessellated Face Builder
 * \{ */

static BVHTree *bvhtree_from_mesh_faces_create_tree(float epsilon,
                                                    int tree_type,
                                                    int axis,
                                                    const Span<float3> positions,
                                                    const MFace *face,
                                                    const int faces_num,
                                                    const BitSpan faces_mask,
                                                    int faces_num_active)
{
  BVHTree *tree = bvhtree_new_common(epsilon, tree_type, axis, faces_num, faces_num_active);
  if (!tree) {
    return nullptr;
  }

  if (!positions.is_empty() && face) {
    for (int i = 0; i < faces_num; i++) {
      float co[4][3];
      if (!faces_mask.is_empty() && !faces_mask[i]) {
        continue;
      }

      copy_v3_v3(co[0], positions[face[i].v1]);
      copy_v3_v3(co[1], positions[face[i].v2]);
      copy_v3_v3(co[2], positions[face[i].v3]);
      if (face[i].v4) {
        copy_v3_v3(co[3], positions[face[i].v4]);
      }

      BLI_bvhtree_insert(tree, i, co[0], face[i].v4 ? 4 : 3);
    }
  }
  BLI_assert(BLI_bvhtree_get_len(tree) == faces_num_active);

  return tree;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name corner_tri Face Builder
 * \{ */

static BVHTree *bvhtree_from_mesh_corner_tris_create_tree(float epsilon,
                                                          int tree_type,
                                                          int axis,
                                                          const Span<float3> positions,
                                                          const Span<int> corner_verts,
                                                          const Span<int3> corner_tris,
                                                          const BitSpan corner_tris_mask,
                                                          int corner_tris_num_active)
{
  if (positions.is_empty()) {
    return nullptr;
  }

  BVHTree *tree = bvhtree_new_common(
      epsilon, tree_type, axis, corner_tris.size(), corner_tris_num_active);

  if (!tree) {
    return nullptr;
  }

  for (const int i : corner_tris.index_range()) {
    float co[3][3];
    if (!corner_tris_mask.is_empty() && !corner_tris_mask[i]) {
      continue;
    }

    copy_v3_v3(co[0], positions[corner_verts[corner_tris[i][0]]]);
    copy_v3_v3(co[1], positions[corner_verts[corner_tris[i][1]]]);
    copy_v3_v3(co[2], positions[corner_verts[corner_tris[i][2]]]);

    BLI_bvhtree_insert(tree, i, co[0], 3);
  }

  BLI_assert(BLI_bvhtree_get_len(tree) == corner_tris_num_active);

  return tree;
}

BVHTree *bvhtree_from_mesh_corner_tris_ex(BVHTreeFromMesh *data,
                                          const Span<float3> vert_positions,
                                          const Span<int> corner_verts,
                                          const Span<int3> corner_tris,
                                          const BitSpan corner_tris_mask,
                                          int corner_tris_num_active,
                                          float epsilon,
                                          int tree_type,
                                          int axis)
{
  BVHTree *tree = bvhtree_from_mesh_corner_tris_create_tree(epsilon,
                                                            tree_type,
                                                            axis,
                                                            vert_positions,
                                                            corner_verts,
                                                            corner_tris,
                                                            corner_tris_mask,
                                                            corner_tris_num_active);

  bvhtree_balance(tree);

  if (data) {
    /* Setup BVHTreeFromMesh */
    *data = bvhtree_from_mesh_setup_data(
        tree, BVHTREE_FROM_CORNER_TRIS, vert_positions, {}, corner_verts, corner_tris, nullptr);
  }

  return tree;
}

static BitVector<> loose_verts_no_hidden_mask_get(const Mesh &mesh, int *r_elem_active_len)
{
  using namespace blender;
  using namespace blender::bke;

  int count = mesh.verts_num;
  BitVector<> verts_mask(count, true);

  const AttributeAccessor attributes = mesh.attributes();
  const Span<int2> edges = mesh.edges();
  const VArray<bool> hide_edge = *attributes.lookup_or_default(
      ".hide_edge", AttrDomain::Edge, false);
  const VArray<bool> hide_vert = *attributes.lookup_or_default(
      ".hide_vert", AttrDomain::Point, false);

  for (const int i : edges.index_range()) {
    if (hide_edge[i]) {
      continue;
    }
    for (const int vert : {edges[i][0], edges[i][1]}) {
      if (verts_mask[vert]) {
        verts_mask[vert].reset();
        count--;
      }
    }
  }

  if (count) {
    for (const int vert : verts_mask.index_range()) {
      if (verts_mask[vert] && hide_vert[vert]) {
        verts_mask[vert].reset();
        count--;
      }
    }
  }

  *r_elem_active_len = count;

  return verts_mask;
}

static BitVector<> loose_edges_no_hidden_mask_get(const Mesh &mesh, int *r_elem_active_len)
{
  using namespace blender;
  using namespace blender::bke;

  int count = mesh.edges_num;
  BitVector<> edge_mask(count, true);

  const AttributeAccessor attributes = mesh.attributes();
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_edges = mesh.corner_edges();
  const VArray<bool> hide_poly = *attributes.lookup_or_default(
      ".hide_poly", AttrDomain::Face, false);
  const VArray<bool> hide_edge = *attributes.lookup_or_default(
      ".hide_edge", AttrDomain::Edge, false);

  for (const int i : faces.index_range()) {
    if (hide_poly[i]) {
      continue;
    }
    for (const int edge : corner_edges.slice(faces[i])) {
      if (edge_mask[edge]) {
        edge_mask[edge].reset();
        count--;
      }
    }
  }

  if (count) {
    for (const int edge : edge_mask.index_range()) {
      if (edge_mask[edge] && hide_edge[edge]) {
        edge_mask[edge].reset();
        count--;
      }
    }
  }

  *r_elem_active_len = count;

  return edge_mask;
}

static BitVector<> corner_tris_no_hidden_map_get(const blender::OffsetIndices<int> faces,
                                                 const VArray<bool> &hide_poly,
                                                 const int corner_tris_len,
                                                 int *r_corner_tris_active_len)
{
  if (hide_poly.is_single() && !hide_poly.get_internal_single()) {
    return {};
  }
  BitVector<> corner_tris_mask(corner_tris_len);

  int corner_tris_no_hidden_len = 0;
  int tri_index = 0;
  for (const int64_t i : faces.index_range()) {
    const int triangles_num = blender::bke::mesh::face_triangles_num(faces[i].size());
    if (hide_poly[i]) {
      tri_index += triangles_num;
    }
    else {
      for (const int i : IndexRange(triangles_num)) {
        UNUSED_VARS(i);
        corner_tris_mask[tri_index].set();
        tri_index++;
        corner_tris_no_hidden_len++;
      }
    }
  }

  *r_corner_tris_active_len = corner_tris_no_hidden_len;

  return corner_tris_mask;
}

BVHTreeFromMesh Mesh::bvh_loose_verts() const
{
  using namespace blender::bke;
  const Span<float3> positions = this->vert_positions();
  this->runtime->bvh_cache_loose_verts.ensure([&](BVHCacheItem &data) {
    const LooseVertCache &loose_verts = this->loose_verts();
    data.tree = bvhtree_from_mesh_verts_create_tree(
        0.0f, 2, 6, positions, loose_verts.is_loose_bits, loose_verts.count);
    if (data.tree) {
      BLI_bvhtree_balance(data.tree);
    }
  });
  const BVHCacheItem &tree = this->runtime->bvh_cache_loose_verts.data();
  return bvhtree_from_mesh_setup_data(
      tree.tree, BVHTREE_FROM_LOOSEVERTS, positions, {}, {}, {}, nullptr);
}

BVHTreeFromMesh Mesh::bvh_loose_no_hidden_verts() const
{
  const Span<float3> positions = this->vert_positions();
  this->runtime->bvh_cache_loose_verts_no_hidden.ensure([&](BVHCacheItem &data) {
    int mask_bits_act_len = -1;
    const BitVector<> mask = loose_verts_no_hidden_mask_get(*this, &mask_bits_act_len);
    data.tree = bvhtree_from_mesh_verts_create_tree(0.0f, 2, 6, positions, {}, -1);
    if (data.tree) {
      BLI_bvhtree_balance(data.tree);
    }
  });
  const BVHCacheItem &tree = this->runtime->bvh_cache_loose_verts_no_hidden.data();
  return bvhtree_from_mesh_setup_data(
      tree.tree, BVHTREE_FROM_LOOSEVERTS_NO_HIDDEN, positions, {}, {}, {}, nullptr);
}

BVHTreeFromMesh Mesh::bvh_verts() const
{
  const Span<float3> positions = this->vert_positions();
  this->runtime->bvh_cache_verts.ensure([&](BVHCacheItem &data) {
    data.tree = bvhtree_from_mesh_verts_create_tree(0.0f, 2, 6, positions, {}, -1);
    if (data.tree) {
      BLI_bvhtree_balance(data.tree);
    }
  });
  const BVHCacheItem &tree = this->runtime->bvh_cache_verts.data();
  return bvhtree_from_mesh_setup_data(
      tree.tree, BVHTREE_FROM_VERTS, positions, {}, {}, {}, nullptr);
}

BVHTreeFromMesh Mesh::bvh_loose_edges() const
{
  using namespace blender::bke;
  const Span<float3> positions = this->vert_positions();
  const Span<int2> edges = this->edges();
  this->runtime->bvh_cache_loose_edges.ensure([&](BVHCacheItem &data) {
    const LooseEdgeCache &loose_edges = this->loose_edges();
    data.tree = bvhtree_from_mesh_edges_create_tree(
        positions, edges, loose_edges.is_loose_bits, loose_edges.count, 0.0f, 2, 6);
    if (data.tree) {
      BLI_bvhtree_balance(data.tree);
    }
  });
  const BVHCacheItem &tree = this->runtime->bvh_cache_loose_edges.data();
  BVHTreeFromMesh data;
  return bvhtree_from_mesh_setup_data(
      tree.tree, BVHTREE_FROM_LOOSEEDGES, positions, edges, {}, {}, nullptr);
}

BVHTreeFromMesh Mesh::bvh_loose_no_hidden_edges() const
{
  const Span<float3> positions = this->vert_positions();
  const Span<int2> edges = this->edges();
  this->runtime->bvh_cache_loose_edges_no_hidden.ensure([&](BVHCacheItem &data) {
    int mask_bits_act_len = -1;
    const BitVector<> mask = loose_edges_no_hidden_mask_get(*this, &mask_bits_act_len);
    data.tree = bvhtree_from_mesh_edges_create_tree(
        positions, edges, mask, mask_bits_act_len, 0.0f, 2, 6);
    if (data.tree) {
      BLI_bvhtree_balance(data.tree);
    }
  });
  const BVHCacheItem &tree = this->runtime->bvh_cache_loose_edges_no_hidden.data();
  return bvhtree_from_mesh_setup_data(
      tree.tree, BVHTREE_FROM_LOOSEEDGES_NO_HIDDEN, positions, {}, {}, {}, nullptr);
}

BVHTreeFromMesh Mesh::bvh_edges() const
{
  const Span<float3> positions = this->vert_positions();
  const Span<int2> edges = this->edges();
  this->runtime->bvh_cache_edges.ensure([&](BVHCacheItem &data) {
    data.tree = bvhtree_from_mesh_edges_create_tree(positions, edges, {}, -1, 0.0f, 2, 6);
    if (data.tree) {
      BLI_bvhtree_balance(data.tree);
    }
  });
  const BVHCacheItem &tree = this->runtime->bvh_cache_edges.data();
  return bvhtree_from_mesh_setup_data(
      tree.tree, BVHTREE_FROM_EDGES, positions, edges, {}, {}, nullptr);
}

BVHTreeFromMesh Mesh::bvh_legacy_faces() const
{
  BLI_assert(!(this->totface_legacy == 0 && this->faces_num != 0));
  const Span<float3> positions = this->vert_positions();
  this->runtime->bvh_cache_faces.ensure([&](BVHCacheItem &data) {
    data.tree = bvhtree_from_mesh_faces_create_tree(
        0.0f,
        2,
        6,
        positions,
        (const MFace *)CustomData_get_layer(&this->fdata_legacy, CD_MFACE),
        this->totface_legacy,
        {},
        -1);
    if (data.tree) {
      BLI_bvhtree_balance(data.tree);
    }
  });
  const BVHCacheItem &tree = this->runtime->bvh_cache_faces.data();
  return bvhtree_from_mesh_setup_data(
      tree.tree, BVHTREE_FROM_FACES, positions, {}, {}, {}, nullptr);
}

BVHTreeFromMesh Mesh::bvh_corner_tris_no_hidden() const
{
  using namespace blender::bke;
  const Span<float3> positions = this->vert_positions();
  const Span<int> corner_verts = this->corner_verts();
  const Span<int3> corner_tris = this->corner_tris();
  this->runtime->bvh_cache_verts.ensure([&](BVHCacheItem &data) {
    AttributeAccessor attributes = this->attributes();
    int mask_bits_act_len = -1;
    const BitVector<> mask = corner_tris_no_hidden_map_get(
        this->faces(),
        *attributes.lookup_or_default(".hide_poly", AttrDomain::Face, false),
        corner_tris.size(),
        &mask_bits_act_len);
    data.tree = bvhtree_from_mesh_corner_tris_create_tree(
        0.0f, 2, 6, positions, corner_verts, corner_tris, mask, mask_bits_act_len);
    if (data.tree) {
      BLI_bvhtree_balance(data.tree);
    }
  });
  const BVHCacheItem &tree = this->runtime->bvh_cache_verts.data();
  return bvhtree_from_mesh_setup_data(tree.tree,
                                      BVHTREE_FROM_CORNER_TRIS_NO_HIDDEN,
                                      positions,
                                      {},
                                      corner_verts,
                                      corner_tris,
                                      nullptr);
}

BVHTreeFromMesh Mesh::bvh_corner_tris() const
{
  const Span<float3> positions = this->vert_positions();
  const Span<int> corner_verts = this->corner_verts();
  const Span<int3> corner_tris = this->corner_tris();
  this->runtime->bvh_cache_corner_tris.ensure([&](BVHCacheItem &data) {
    data.tree = bvhtree_from_mesh_corner_tris_create_tree(
        0.0f, 2, 6, positions, corner_verts, corner_tris, {}, -1);
    if (data.tree) {
      BLI_bvhtree_balance(data.tree);
    }
  });
  const BVHCacheItem &tree = this->runtime->bvh_cache_corner_tris.data();
  return bvhtree_from_mesh_setup_data(
      tree.tree, BVHTREE_FROM_CORNER_TRIS, positions, {}, corner_verts, corner_tris, nullptr);
}

void BKE_bvhtree_from_mesh_tris_init(const Mesh &mesh,
                                     const blender::IndexMask &faces_mask,
                                     BVHTreeFromMesh &r_data)
{
  using namespace blender;
  using namespace blender::bke;

  if (faces_mask.size() == mesh.faces_num) {
    /* Can use cache if all faces are in the bvh tree. */
    r_data = mesh.bvh_corner_tris();
    return;
  }

  const Span<float3> positions = mesh.vert_positions();
  const Span<int2> edges = mesh.edges();
  const Span<int> corner_verts = mesh.corner_verts();
  const OffsetIndices faces = mesh.faces();
  const Span<int3> corner_tris = mesh.corner_tris();
  r_data = bvhtree_from_mesh_setup_data(
      nullptr, BVHTREE_FROM_CORNER_TRIS, positions, edges, corner_verts, corner_tris, nullptr);

  int tris_num = 0;
  faces_mask.foreach_index(
      [&](const int i) { tris_num += mesh::face_triangles_num(faces[i].size()); });

  int active_num = -1;
  BVHTree *tree = bvhtree_new_common(0.0f, 2, 6, tris_num, active_num);
  r_data.owned_tree = std::unique_ptr<BVHTree, BVHTreeDeleter>(tree);
  r_data.tree = tree;
  if (tree == nullptr) {
    return;
  }

  faces_mask.foreach_index([&](const int face_i) {
    const IndexRange triangles_range = mesh::face_triangles_range(faces, face_i);
    for (const int tri_i : triangles_range) {
      float co[3][3];
      copy_v3_v3(co[0], positions[corner_verts[corner_tris[tri_i][0]]]);
      copy_v3_v3(co[1], positions[corner_verts[corner_tris[tri_i][1]]]);
      copy_v3_v3(co[2], positions[corner_verts[corner_tris[tri_i][2]]]);

      BLI_bvhtree_insert(tree, tri_i, co[0], 3);
    }
  });

  BLI_bvhtree_balance(tree);
}

void BKE_bvhtree_from_mesh_edges_init(const Mesh &mesh,
                                      const blender::IndexMask &edges_mask,
                                      BVHTreeFromMesh &r_data)
{
  using namespace blender;
  using namespace blender::bke;

  if (edges_mask.size() == mesh.edges_num) {
    /* Can use cache if all edges are in the bvh tree. */
    r_data = mesh.bvh_edges();
    return;
  }

  const Span<float3> positions = mesh.vert_positions();
  const Span<int2> edges = mesh.edges();
  r_data = bvhtree_from_mesh_setup_data(
      nullptr, BVHTREE_FROM_EDGES, positions, edges, {}, {}, nullptr);

  int active_num = -1;
  BVHTree *tree = bvhtree_new_common(0.0f, 2, 6, edges_mask.size(), active_num);
  r_data.owned_tree = std::unique_ptr<BVHTree, BVHTreeDeleter>(tree);
  r_data.tree = tree;
  if (tree == nullptr) {
    return;
  }

  edges_mask.foreach_index([&](const int edge_i) {
    const int2 &edge = edges[edge_i];
    float co[2][3];
    copy_v3_v3(co[0], positions[edge[0]]);
    copy_v3_v3(co[1], positions[edge[1]]);
    BLI_bvhtree_insert(tree, edge_i, co[0], 2);
  });

  BLI_bvhtree_balance(tree);
}

void BKE_bvhtree_from_mesh_verts_init(const Mesh &mesh,
                                      const blender::IndexMask &verts_mask,
                                      BVHTreeFromMesh &r_data)
{
  using namespace blender;
  using namespace blender::bke;

  if (verts_mask.size() == mesh.verts_num) {
    /* Can use cache if all vertices are in the bvh tree. */
    r_data = mesh.bvh_verts();
    return;
  }

  const Span<float3> positions = mesh.vert_positions();
  r_data = bvhtree_from_mesh_setup_data(
      nullptr, BVHTREE_FROM_VERTS, positions, {}, {}, {}, nullptr);

  int active_num = -1;
  BVHTree *tree = bvhtree_new_common(0.0f, 2, 6, verts_mask.size(), active_num);
  r_data.owned_tree = std::unique_ptr<BVHTree, BVHTreeDeleter>(tree);
  r_data.tree = tree;
  if (tree == nullptr) {
    return;
  }

  verts_mask.foreach_index([&](const int vert_i) {
    const float3 &position = positions[vert_i];
    BLI_bvhtree_insert(tree, vert_i, position, 1);
  });

  BLI_bvhtree_balance(tree);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Point Cloud BVH Building
 * \{ */

void BKE_bvhtree_from_pointcloud_get(const PointCloud &pointcloud,
                                     const blender::IndexMask &points_mask,
                                     BVHTreeFromPointCloud &r_data)
{
  int active_num = -1;
  BVHTree *tree = bvhtree_new_common(0.0f, 2, 6, points_mask.size(), active_num);
  r_data.tree = tree;
  if (!tree) {
    return;
  }

  const Span<float3> positions = pointcloud.positions();
  points_mask.foreach_index([&](const int i) { BLI_bvhtree_insert(tree, i, positions[i], 1); });

  BLI_bvhtree_balance(tree);

  r_data.coords = (const float(*)[3])positions.data();
  r_data.tree = tree;
  r_data.nearest_callback = nullptr;
}

void free_bvhtree_from_pointcloud(BVHTreeFromPointCloud *data)
{
  if (data->tree) {
    BLI_bvhtree_free(data->tree);
  }
  memset(data, 0, sizeof(*data));
}

/** \} */
