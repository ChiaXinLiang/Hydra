// The original implementation from https://github.com/personalrobotics/OpenChisel and
// subsequent modifications falls under the following license:
//
// The MIT License (MIT)
// Copyright (c) 2014 Matthew Klingensmith and Ivan Dryanovski
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Modifications are also subject to the following copyright and disclaimer:
//
// Copyright 2022 Massachusetts Institute of Technology.
// All Rights Reserved
//
// Research was sponsored by the United States Air Force Research Laboratory and
// the United States Air Force Artificial Intelligence Accelerator and was
// accomplished under Cooperative Agreement Number FA8750-19-2-1000. The views
// and conclusions contained in this document are those of the authors and should
// not be interpreted as representing the official policies, either expressed or
// implied, of the United States Air Force or the U.S. Government. The U.S.
// Government is authorized to reproduce and distribute reprints for Government
// purposes notwithstanding any copyright notation herein.
#include "hydra/reconstruction/mesh_integrator.h"

#include <glog/logging.h>
#include <voxblox/utils/meshing_utils.h>

#include <iomanip>

#include "hydra/reconstruction/marching_cubes.h"
#include "hydra/reconstruction/voxblox_utilities.h"

namespace hydra {

using kimera::SemanticVoxel;
using places::VertexVoxel;
using voxblox::Block;
using voxblox::BlockIndex;
using voxblox::BlockIndexList;
using voxblox::FloatingPoint;
using voxblox::IndexElement;
using voxblox::Layer;
using voxblox::Mesh;
using voxblox::MixedThreadSafeIndex;
using voxblox::Point;
using voxblox::ThreadSafeIndex;
using voxblox::TsdfVoxel;
using voxblox::VertexIndex;
using voxblox::VoxelIndex;
namespace vutils = voxblox::utils;

using TsdfLayer = Layer<TsdfVoxel>;
using TsdfBlock = Block<TsdfVoxel>;
using VertexLayer = Layer<VertexVoxel>;
using SemanticLayer = Layer<SemanticVoxel>;

MeshIntegrator::MeshIntegrator(const MeshIntegratorConfig& config,
                               const TsdfLayer::Ptr& sdf_layer,
                               const VertexLayer::Ptr& vertex_layer,
                               const SemanticMeshLayer::Ptr& mesh_layer,
                               const SemanticLayer::Ptr& semantic_layer)
    : config_(config),
      sdf_layer_(sdf_layer),
      mesh_layer_(mesh_layer),
      vertex_layer_(vertex_layer),
      semantic_layer_(semantic_layer) {
  DCHECK(sdf_layer_ != nullptr);
  DCHECK(mesh_layer_ != nullptr);
  DCHECK(vertex_layer_ != nullptr);

  voxel_size_ = sdf_layer_->voxel_size();
  block_size_ = sdf_layer_->block_size();
  voxels_per_side_ = sdf_layer_->voxels_per_side();

  // clang-format off
  cube_index_offsets_ << 0, 1, 1, 0, 0, 1, 1, 0,
                         0, 0, 1, 1, 0, 0, 1, 1,
                         0, 0, 0, 0, 1, 1, 1, 1;
  // clang-format on
  cube_coord_offsets_ = cube_index_offsets_.cast<FloatingPoint>() * voxel_size_;
}

void MeshIntegrator::launchThreads(const BlockIndexList& blocks, bool interior_pass) {
  std::unique_ptr<ThreadSafeIndex> index_getter(
      new MixedThreadSafeIndex(blocks.size()));

  std::list<std::thread> threads;
  for (size_t i = 0; i < config_.integrator_threads; ++i) {
    if (interior_pass) {
      threads.emplace_back(
          &MeshIntegrator::processInterior, this, blocks, index_getter.get());
    } else {
      threads.emplace_back(
          &MeshIntegrator::processExterior, this, blocks, index_getter.get());
    }
  }

  for (std::thread& thread : threads) {
    thread.join();
  }
}

void MeshIntegrator::generateMesh(bool only_mesh_updated_blocks,
                                  bool clear_updated_flag) {
  BlockIndexList blocks;
  if (only_mesh_updated_blocks) {
    sdf_layer_->getAllUpdatedBlocks(voxblox::Update::kMesh, &blocks);
  } else {
    sdf_layer_->getAllAllocatedBlocks(&blocks);
  }

  for (const BlockIndex& block_index : blocks) {
    auto mesh = mesh_layer_->allocateBlock(block_index, semantic_layer_ != nullptr);
    mesh->clear();

    // also allocate gvd blocks
    auto vertex_block = vertex_layer_->allocateBlockPtrByIndex(block_index);

    // we need to reset these so that marching cubes can assign them correctly
    for (size_t idx = 0u; idx < vertex_block->num_voxels(); ++idx) {
      vertex_block->getVoxelByLinearIndex(idx).on_surface = false;
    }
  }

  // interior then exterior, but order shouldn't matter
  launchThreads(blocks, true);
  launchThreads(blocks, false);

  if (VLOG_IS_ON(10)) {
    VLOG(10) << "Updated blocks:";
    for (const auto& idx : blocks) {
      const auto& block = mesh_layer_->getMeshBlock(idx);
      VLOG(10) << "  - " << std::setw(4) << std::setfill(' ') << block->vertices.size()
               << " vertices @ " << showIndex(idx);
    }
  }

  if (clear_updated_flag) {
    for (const auto& block_idx : blocks) {
      auto block = sdf_layer_->getBlockPtrByIndex(block_idx);
      block->updated().reset(voxblox::Update::kMesh);
    }
  }
}

void MeshIntegrator::updateMeshColor(const Block<TsdfVoxel>& block,
                                     Mesh* mesh,
                                     const BlockIndex& index) {
  DCHECK(mesh != nullptr);
  // TODO(nathan) this is very ugly; we should just grab this info from marching
  // cubes...

  mesh->colors.clear();
  mesh->colors.resize(mesh->indices.size());

  std::vector<uint32_t>* mesh_semantics_block = nullptr;
  if (semantic_layer_) {
    mesh_semantics_block = mesh_layer_->getSemanticBlock(index);
    mesh_semantics_block->resize(mesh->indices.size());
  }

  const TsdfVoxel* voxel = nullptr;
  const SemanticVoxel* semantic_voxel = nullptr;
  // Use nearest-neighbor search.
  for (size_t i = 0; i < mesh->vertices.size(); i++) {
    const auto& vertex = mesh->vertices[i];
    const auto voxel_index = block.computeVoxelIndexFromCoordinates(vertex);

    if (block.isValidVoxelIndex(voxel_index)) {
      voxel = &block.getVoxelByVoxelIndex(voxel_index);
      if (semantic_layer_) {
        const auto semantic_block = semantic_layer_->getBlockPtrByIndex(index);
        semantic_voxel = &semantic_block->getVoxelByVoxelIndex(voxel_index);
      }
    } else {
      const auto neighbor_block = sdf_layer_->getBlockPtrByCoordinates(vertex);
      voxel = &neighbor_block->getVoxelByCoordinates(vertex);
      if (semantic_layer_) {
        const auto semantic_block = semantic_layer_->getBlockPtrByCoordinates(vertex);
        semantic_voxel = &semantic_block->getVoxelByCoordinates(vertex);
      }
    }

    DCHECK(voxel != nullptr);
    vutils::getColorIfValid(*voxel, config_.min_weight, &(mesh->colors[i]));
    if (semantic_voxel) {
      mesh_semantics_block->at(i) = semantic_voxel->semantic_label;
    }
  }
}

void MeshIntegrator::processInterior(const BlockIndexList& blocks,
                                     ThreadSafeIndex* index_getter) {
  DCHECK(index_getter != nullptr);

  size_t list_idx;
  while (index_getter->getNextIndex(&list_idx)) {
    updateBlockInterior(blocks[list_idx]);
  }
}

void MeshIntegrator::processExterior(const BlockIndexList& blocks,
                                     ThreadSafeIndex* index_getter) {
  DCHECK(index_getter != nullptr);

  size_t list_idx;
  while (index_getter->getNextIndex(&list_idx)) {
    updateBlockExterior(blocks[list_idx]);
  }
}

void MeshIntegrator::updateBlockInterior(const BlockIndex& block_index) {
  VLOG(10) << "Extracting interior for block: " << showIndex(block_index);
  auto mesh = mesh_layer_->getMeshBlock(block_index);
  auto block = sdf_layer_->getBlockPtrByIndex(block_index);
  DCHECK(block) << "invalid SDF block for mesh";

  IndexElement vps = block->voxels_per_side();
  VertexIndex next_mesh_index = 0;

  VoxelIndex voxel_index;
  for (voxel_index.x() = 0; voxel_index.x() < vps - 1; ++voxel_index.x()) {
    for (voxel_index.y() = 0; voxel_index.y() < vps - 1; ++voxel_index.y()) {
      for (voxel_index.z() = 0; voxel_index.z() < vps - 1; ++voxel_index.z()) {
        Point coords = block->computeCoordinatesFromVoxelIndex(voxel_index);
        extractMeshInsideBlock(
            *block, voxel_index, coords, &next_mesh_index, mesh.get());
      }
    }
  }
}

void MeshIntegrator::updateBlockExterior(const BlockIndex& block_index) {
  VLOG(10) << "Extracting exterior for block: " << showIndex(block_index);
  auto mesh = mesh_layer_->getMeshBlock(block_index);
  auto block = sdf_layer_->getBlockPtrByIndex(block_index);

  IndexElement vps = block->voxels_per_side();
  VertexIndex next_mesh_index = mesh->size();

  VoxelIndex voxel_index;

  // Max X plane
  // takes care of edge (x_max, y_max, z), takes care of edge (x_max, y, z_max).
  voxel_index.x() = vps - 1;
  for (voxel_index.z() = 0; voxel_index.z() < vps; voxel_index.z()++) {
    for (voxel_index.y() = 0; voxel_index.y() < vps; voxel_index.y()++) {
      Point coords = block->computeCoordinatesFromVoxelIndex(voxel_index);
      extractMeshOnBorder(*block, voxel_index, coords, &next_mesh_index, mesh.get());
    }
  }

  // Max Y plane.
  // takes care of edge (x, y_max, z_max) without corner (x_max, y_max, z_max).
  voxel_index.y() = vps - 1;
  for (voxel_index.z() = 0; voxel_index.z() < vps; voxel_index.z()++) {
    for (voxel_index.x() = 0; voxel_index.x() < vps - 1; voxel_index.x()++) {
      Point coords = block->computeCoordinatesFromVoxelIndex(voxel_index);
      extractMeshOnBorder(*block, voxel_index, coords, &next_mesh_index, mesh.get());
    }
  }

  // Max Z plane.
  voxel_index.z() = vps - 1;
  for (voxel_index.y() = 0; voxel_index.y() < vps - 1; voxel_index.y()++) {
    for (voxel_index.x() = 0; voxel_index.x() < vps - 1; voxel_index.x()++) {
      Point coords = block->computeCoordinatesFromVoxelIndex(voxel_index);
      extractMeshOnBorder(*block, voxel_index, coords, &next_mesh_index, mesh.get());
    }
  }

  updateMeshColor(*block, mesh.get(), block_index);
  mesh->updated = true;
}

BlockIndex MeshIntegrator::getNeighborBlockIndex(const BlockIndex& block_idx,
                                                 VoxelIndex& corner_index) {
  BlockIndex block_offset = BlockIndex::Zero();
  for (unsigned int j = 0u; j < 3u; j++) {
    if (corner_index(j) < 0) {
      block_offset(j) = -1;
      corner_index(j) = corner_index(j) + voxels_per_side_;
    } else if (corner_index(j) >= static_cast<IndexElement>(voxels_per_side_)) {
      block_offset(j) = 1;
      corner_index(j) = corner_index(j) - voxels_per_side_;
    }
  }

  return block_idx + block_offset;
}

void MeshIntegrator::extractMeshInsideBlock(const TsdfBlock& block,
                                            const VoxelIndex& index,
                                            const Point& point,
                                            VertexIndex* new_mesh_idx,
                                            Mesh* mesh) {
  DCHECK(new_mesh_idx != nullptr);
  DCHECK(mesh != nullptr);

  VLOG(15) << "[mesh] processing interior voxel: " << index.transpose();

  const BlockIndex block_idx = block.block_index();
  auto vertex_block = vertex_layer_->getBlockPtrByIndex(block_idx);
  DCHECK(vertex_block != nullptr);

  SdfMatrix sdf;
  PointMatrix coords;
  std::vector<VertexVoxel*> voxels(8, nullptr);
  for (int i = 0; i < 8; ++i) {
    VoxelIndex corner_index = index + cube_index_offsets_.col(i);
    const auto& voxel = block.getVoxelByVoxelIndex(corner_index);
    if (!vutils::getSdfIfValid(voxel, config_.min_weight, &(sdf(i)))) {
      return;
    }

    coords.col(i) = point + cube_coord_offsets_.col(i);
    voxels[i] = &vertex_block->getVoxelByVoxelIndex(corner_index);
  }

  ::hydra::MarchingCubes::meshCube(block_idx, coords, sdf, new_mesh_idx, mesh, voxels);
}

void MeshIntegrator::extractMeshOnBorder(const TsdfBlock& block,
                                         const VoxelIndex& index,
                                         const Point& point,
                                         VertexIndex* new_mesh_idx,
                                         Mesh* mesh) {
  DCHECK(new_mesh_idx != nullptr);
  DCHECK(mesh != nullptr);

  VLOG(15) << "[mesh] processing exterior voxel: " << index.transpose();

  const BlockIndex block_idx = block.block_index();
  auto vertex_block = vertex_layer_->getBlockPtrByIndex(block_idx);
  DCHECK(vertex_block != nullptr);

  SdfMatrix sdf;
  PointMatrix coords;
  std::vector<VertexVoxel*> voxels(8, nullptr);
  for (int i = 0; i < 8; ++i) {
    VoxelIndex corner_index = index + cube_index_offsets_.col(i);

    const TsdfVoxel* voxel;
    if (block.isValidVoxelIndex(corner_index)) {
      voxel = &block.getVoxelByVoxelIndex(corner_index);
      voxels[i] = &vertex_block->getVoxelByVoxelIndex(corner_index);
    } else {
      // we have to access a different block
      const auto neighbor_idx = getNeighborBlockIndex(block_idx, corner_index);
      if (!sdf_layer_->hasBlock(neighbor_idx)) {
        return;
      }

      const auto& neighbor_block = sdf_layer_->getBlockByIndex(neighbor_idx);
      CHECK(neighbor_block.isValidVoxelIndex(corner_index));

      voxel = &neighbor_block.getVoxelByVoxelIndex(corner_index);
      // // we can't ensure that neighboring blocks stay in sync with the current mesh
      // // easily, so we don't track nearest surfaces to neighboring blocks for now
      // auto neighbor_vertex_block = vertex_layer_->getBlockPtrByIndex(neighbor_idx);
      // voxels[i] = &neighbor_vertex_block->getVoxelByVoxelIndex(corner_index);
    }

    if (!vutils::getSdfIfValid(*voxel, config_.min_weight, &(sdf(i)))) {
      return;
    }

    coords.col(i) = point + cube_coord_offsets_.col(i);
  }

  ::hydra::MarchingCubes::meshCube(block_idx, coords, sdf, new_mesh_idx, mesh, voxels);
}

}  // namespace hydra
