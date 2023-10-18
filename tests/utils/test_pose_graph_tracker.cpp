/* -----------------------------------------------------------------------------
 * Copyright 2022 Massachusetts Institute of Technology.
 * All Rights Reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Research was sponsored by the United States Air Force Research Laboratory and
 * the United States Air Force Artificial Intelligence Accelerator and was
 * accomplished under Cooperative Agreement Number FA8750-19-2-1000. The views
 * and conclusions contained in this document are those of the authors and should
 * not be interpreted as representing the official policies, either expressed or
 * implied, of the United States Air Force or the U.S. Government. The U.S.
 * Government is authorized to reproduce and distribute reprints for Government
 * purposes notwithstanding any copyright notation herein.
 * -------------------------------------------------------------------------- */
#include <gtest/gtest.h>
#include <hydra/reconstruction/reconstruction_input.h>
#include <hydra/reconstruction/reconstruction_output.h>
#include <hydra/utils/pose_graph_tracker.h>

namespace hydra {

TEST(PoseGraphTracker, EmptyGraphsCorrect) {
  PoseGraphTracker::Config config;
  config.make_pose_graph = false;
  PoseGraphTracker tracker(config);

  ReconstructionInput msg;
  ReconstructionOutput result;

  // no input pose graphs -> no output pose graphs
  tracker.update(msg);
  tracker.fillPoseGraphs(result);
  EXPECT_EQ(result.pose_graphs.size(), 0u);

  // two input pose graphs -> two output pose graphs
  msg.pose_graphs.push_back(nullptr);
  msg.pose_graphs.push_back(nullptr);
  tracker.update(msg);
  tracker.fillPoseGraphs(result);
  EXPECT_EQ(result.pose_graphs.size(), 2u);

  // no input pose graphs -> no change in output
  msg.pose_graphs.clear();
  tracker.update(msg);
  tracker.fillPoseGraphs(result);
  EXPECT_EQ(result.pose_graphs.size(), 2u);

  // one input pose graph -> three total graphs
  msg.pose_graphs.push_back(nullptr);
  tracker.update(msg);
  tracker.fillPoseGraphs(result);
  EXPECT_EQ(result.pose_graphs.size(), 3u);
}

}  // namespace hydra
