/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "GPU_index_buffer.hh"

#include "draw_subdivision.hh"
#include "extract_mesh.hh"

namespace blender::draw {

/* ---------------------------------------------------------------------- */
/** \name Extract Point Indices
 * \{ */

static void extract_points_init(const MeshRenderData &mr,
                                MeshBatchCache & /*cache*/,
                                void * /*buf*/,
                                void *tls_data)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(tls_data);
  GPU_indexbuf_init(elb, GPU_PRIM_POINTS, mr.verts_num, mr.corners_num + mr.loose_indices_num);
}

BLI_INLINE void vert_set_bm(GPUIndexBufBuilder *elb, const BMVert *eve, int l_index)
{
  const int v_index = BM_elem_index_get(eve);
  if (!BM_elem_flag_test(eve, BM_ELEM_HIDDEN)) {
    GPU_indexbuf_set_point_vert(elb, v_index, l_index);
  }
  else {
    GPU_indexbuf_set_point_restart(elb, v_index);
  }
}

BLI_INLINE void vert_set_mesh(GPUIndexBufBuilder *elb,
                              const MeshRenderData &mr,
                              const int v_index,
                              const int l_index)
{
  const bool hidden = mr.use_hide && !mr.hide_vert.is_empty() && mr.hide_vert[v_index];

  if (!(hidden || ((mr.v_origindex) && (mr.v_origindex[v_index] == ORIGINDEX_NONE)))) {
    GPU_indexbuf_set_point_vert(elb, v_index, l_index);
  }
  else {
    GPU_indexbuf_set_point_restart(elb, v_index);
  }
}

static void extract_points_iter_face_bm(const MeshRenderData & /*mr*/,
                                        const BMFace *f,
                                        const int /*f_index*/,
                                        void *_userdata)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_userdata);
  BMLoop *l_iter, *l_first;
  l_iter = l_first = BM_FACE_FIRST_LOOP(f);
  do {
    const int l_index = BM_elem_index_get(l_iter);

    vert_set_bm(elb, l_iter->v, l_index);
  } while ((l_iter = l_iter->next) != l_first);
}

static void extract_points_iter_face_mesh(const MeshRenderData &mr,
                                          const int face_index,
                                          void *_userdata)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_userdata);
  for (const int corner : mr.faces[face_index]) {
    vert_set_mesh(elb, mr, mr.corner_verts[corner], corner);
  }
}

static void extract_points_iter_loose_edge_bm(const MeshRenderData &mr,
                                              const BMEdge *eed,
                                              const int loose_edge_i,
                                              void *_userdata)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_userdata);
  vert_set_bm(elb, eed->v1, mr.corners_num + (loose_edge_i * 2));
  vert_set_bm(elb, eed->v2, mr.corners_num + (loose_edge_i * 2) + 1);
}

static void extract_points_iter_loose_edge_mesh(const MeshRenderData &mr,
                                                const int2 edge,
                                                const int loose_edge_i,
                                                void *_userdata)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_userdata);
  vert_set_mesh(elb, mr, edge[0], mr.corners_num + (loose_edge_i * 2));
  vert_set_mesh(elb, mr, edge[1], mr.corners_num + (loose_edge_i * 2) + 1);
}

static void extract_points_iter_loose_vert_bm(const MeshRenderData &mr,
                                              const BMVert *eve,
                                              const int loose_vert_i,
                                              void *_userdata)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_userdata);
  const int offset = mr.corners_num + (mr.loose_edges_num * 2);
  vert_set_bm(elb, eve, offset + loose_vert_i);
}

static void extract_points_iter_loose_vert_mesh(const MeshRenderData &mr,
                                                const int loose_vert_i,
                                                void *_userdata)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_userdata);
  const int offset = mr.corners_num + (mr.loose_edges_num * 2);
  vert_set_mesh(elb, mr, mr.loose_verts[loose_vert_i], offset + loose_vert_i);
}

static void extract_points_task_reduce(void *_userdata_to, void *_userdata_from)
{
  GPUIndexBufBuilder *elb_to = static_cast<GPUIndexBufBuilder *>(_userdata_to);
  GPUIndexBufBuilder *elb_from = static_cast<GPUIndexBufBuilder *>(_userdata_from);
  GPU_indexbuf_join(elb_to, elb_from);
}

static void extract_points_finish(const MeshRenderData & /*mr*/,
                                  MeshBatchCache & /*cache*/,
                                  void *buf,
                                  void *_userdata)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_userdata);
  gpu::IndexBuf *ibo = static_cast<gpu::IndexBuf *>(buf);
  GPU_indexbuf_build_in_place(elb, ibo);
}

static void extract_points_init_subdiv(const DRWSubdivCache &subdiv_cache,
                                       const MeshRenderData &mr,
                                       MeshBatchCache & /*cache*/,
                                       void * /*buffer*/,
                                       void *data)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(data);
  GPU_indexbuf_init(elb, GPU_PRIM_POINTS, mr.verts_num, subdiv_full_vbo_size(mr, subdiv_cache));
}

static void extract_points_iter_subdiv_common(GPUIndexBufBuilder *elb,
                                              const MeshRenderData &mr,
                                              const DRWSubdivCache &subdiv_cache,
                                              uint subdiv_quad_index,
                                              bool for_bmesh)
{
  int *subdiv_loop_vert_index = (int *)GPU_vertbuf_get_data(subdiv_cache.verts_orig_index);
  uint start_loop_idx = subdiv_quad_index * 4;
  uint end_loop_idx = (subdiv_quad_index + 1) * 4;
  for (uint i = start_loop_idx; i < end_loop_idx; i++) {
    int coarse_vertex_index = subdiv_loop_vert_index[i];

    if (coarse_vertex_index == -1) {
      continue;
    }

    if (mr.v_origindex && mr.v_origindex[coarse_vertex_index] == -1) {
      continue;
    }

    if (for_bmesh) {
      const BMVert *mv = BM_vert_at_index(mr.bm, coarse_vertex_index);
      if (BM_elem_flag_test(mv, BM_ELEM_HIDDEN)) {
        GPU_indexbuf_set_point_restart(elb, coarse_vertex_index);
        continue;
      }
    }
    else {
      if (mr.use_hide && !mr.hide_vert.is_empty() && mr.hide_vert[coarse_vertex_index]) {
        GPU_indexbuf_set_point_restart(elb, coarse_vertex_index);
        continue;
      }
    }

    GPU_indexbuf_set_point_vert(elb, coarse_vertex_index, i);
  }
}

static void extract_points_iter_subdiv_bm(const DRWSubdivCache &subdiv_cache,
                                          const MeshRenderData &mr,
                                          void *_data,
                                          uint subdiv_quad_index,
                                          const BMFace * /*coarse_quad*/)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_data);
  extract_points_iter_subdiv_common(elb, mr, subdiv_cache, subdiv_quad_index, true);
}

static void extract_points_iter_subdiv_mesh(const DRWSubdivCache &subdiv_cache,
                                            const MeshRenderData &mr,
                                            void *_data,
                                            uint subdiv_quad_index,
                                            const int /*coarse_quad_index*/)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_data);
  extract_points_iter_subdiv_common(elb, mr, subdiv_cache, subdiv_quad_index, false);
}

static void extract_points_loose_geom_subdiv(const DRWSubdivCache &subdiv_cache,
                                             const MeshRenderData &mr,
                                             void * /*buffer*/,
                                             void *data)
{
  const Span<int> loose_verts = mr.loose_verts;
  const Span<int> loose_edges = mr.loose_edges;
  if (loose_verts.is_empty() && loose_edges.is_empty()) {
    return;
  }

  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(data);

  const Span<int2> edges = mr.edges;
  const int loose_start = subdiv_cache.num_subdiv_loops;
  const int verts_per_edge = subdiv_verts_per_coarse_edge(subdiv_cache);
  const int loose_verts_start = loose_start + verts_per_edge * loose_edges.size();

  if (mr.extract_type != MR_EXTRACT_BMESH) {
    threading::parallel_for(loose_edges.index_range(), 2048, [&](const IndexRange range) {
      for (const int i : range) {
        const IndexRange edge_vbo_range(loose_start + i * verts_per_edge, verts_per_edge);
        vert_set_mesh(elb, mr, edges[loose_edges[i]][0], edge_vbo_range.first());
        vert_set_mesh(elb, mr, edges[loose_edges[i]][1], edge_vbo_range.last());
      }
    });

    const Span<int> loose_verts = mr.loose_verts;
    threading::parallel_for(loose_verts.index_range(), 2048, [&](const IndexRange range) {
      for (const int i : range) {
        vert_set_mesh(elb, mr, loose_verts[i], loose_verts_start + i);
      }
    });
  }
  else {
    threading::parallel_for(loose_edges.index_range(), 2048, [&](const IndexRange range) {
      for (const int i : range) {
        const IndexRange edge_vbo_range(loose_start + i * verts_per_edge, verts_per_edge);
        BMVert *vert_0 = mr.v_origindex ? bm_original_vert_get(mr, edges[loose_edges[i]][0]) :
                                          BM_vert_at_index(mr.bm, edges[loose_edges[i]][0]);
        BMVert *vert_1 = mr.v_origindex ? bm_original_vert_get(mr, edges[loose_edges[i]][1]) :
                                          BM_vert_at_index(mr.bm, edges[loose_edges[i]][1]);
        vert_set_bm(elb, vert_0, edge_vbo_range.first());
        vert_set_bm(elb, vert_1, edge_vbo_range.last());
      }
    });

    threading::parallel_for(loose_verts.index_range(), 2048, [&](const IndexRange range) {
      for (const int i : range) {
        BMVert *vert = mr.v_origindex ? bm_original_vert_get(mr, loose_verts[i]) :
                                        BM_vert_at_index(mr.bm, loose_verts[i]);
        vert_set_bm(elb, vert, loose_verts_start + i);
      }
    });
  }
}

static void extract_points_finish_subdiv(const DRWSubdivCache & /*subdiv_cache*/,
                                         const MeshRenderData & /*mr*/,
                                         MeshBatchCache & /*cache*/,
                                         void *buf,
                                         void *_userdata)
{
  GPUIndexBufBuilder *elb = static_cast<GPUIndexBufBuilder *>(_userdata);
  gpu::IndexBuf *ibo = static_cast<gpu::IndexBuf *>(buf);
  GPU_indexbuf_build_in_place(elb, ibo);
}

constexpr MeshExtract create_extractor_points()
{
  MeshExtract extractor = {nullptr};
  extractor.init = extract_points_init;
  extractor.iter_face_bm = extract_points_iter_face_bm;
  extractor.iter_face_mesh = extract_points_iter_face_mesh;
  extractor.iter_loose_edge_bm = extract_points_iter_loose_edge_bm;
  extractor.iter_loose_edge_mesh = extract_points_iter_loose_edge_mesh;
  extractor.iter_loose_vert_bm = extract_points_iter_loose_vert_bm;
  extractor.iter_loose_vert_mesh = extract_points_iter_loose_vert_mesh;
  extractor.task_reduce = extract_points_task_reduce;
  extractor.finish = extract_points_finish;
  extractor.init_subdiv = extract_points_init_subdiv;
  extractor.iter_subdiv_bm = extract_points_iter_subdiv_bm;
  extractor.iter_subdiv_mesh = extract_points_iter_subdiv_mesh;
  extractor.iter_loose_geom_subdiv = extract_points_loose_geom_subdiv;
  extractor.finish_subdiv = extract_points_finish_subdiv;
  extractor.use_threading = true;
  extractor.data_type = MR_DATA_NONE;
  extractor.data_size = sizeof(GPUIndexBufBuilder);
  extractor.mesh_buffer_offset = offsetof(MeshBufferList, ibo.points);
  return extractor;
}

/** \} */

const MeshExtract extract_points = create_extractor_points();

}  // namespace blender::draw
