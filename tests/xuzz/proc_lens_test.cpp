/**
 * @file proc_lens_test.cpp
 * @brief Unit tests for ProcLens process tree projection.
 */
#include <gtest/gtest.h>

#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/crdt_zz.hpp"
#include "common/xanadu/zigzag/proc_lens.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"

namespace {

using namespace zigzag;

TEST(ProcLensTest, ProcessTreeHierarchyAndSiblingChains) {
  // Mock process tree:
  // init(1)
  //   ├── bash(100)
  //   │     └── make(200)
  //   ├── sshd(101)
  //   └── nginx(102)
  const std::vector<ProcessSnapshot> tree = {
      {.pid        = 1,
       .ppid       = 0,
       .comm       = "systemd",
       .state      = 'S',
       .cpuPercent = 0.1,
       .rssBytes   = 10000,
       .threadTids = {1}},
      {.pid        = 100,
       .ppid       = 1,
       .comm       = "bash",
       .state      = 'S',
       .cpuPercent = 0.5,
       .rssBytes   = 5000,
       .threadTids = {100}},
      {.pid        = 101,
       .ppid       = 1,
       .comm       = "sshd",
       .state      = 'S',
       .cpuPercent = 0.0,
       .rssBytes   = 4000,
       .threadTids = {101}},
      {.pid        = 102,
       .ppid       = 1,
       .comm       = "nginx",
       .state      = 'S',
       .cpuPercent = 1.2,
       .rssBytes   = 25000,
       .threadTids = {102, 103, 104}},
      {.pid        = 200,
       .ppid       = 100,
       .comm       = "make",
       .state      = 'R',
       .cpuPercent = 95.0,
       .rssBytes   = 30000,
       .threadTids = {200}},
  };

  auto lens = std::make_unique<ProcLens>(tree);
  ArenaManifold arena;
  vortex::VortexCore core(arena);
  ProjectionSession session(std::move(lens), core);
  EXPECT_TRUE(crdt::CRDTEngine::verifyManifoldInvariants(arena));

  // Find cells
  const auto cInit  = session.cellFor("proc:1");
  const auto cBash  = session.cellFor("proc:100");
  const auto cSshd  = session.cellFor("proc:101");
  const auto cNginx = session.cellFor("proc:102");
  const auto cMake  = session.cellFor("proc:200");

  ASSERT_TRUE(cInit.has_value());
  ASSERT_TRUE(cBash.has_value());
  ASSERT_TRUE(cSshd.has_value());
  ASSERT_TRUE(cNginx.has_value());
  ASSERT_TRUE(cMake.has_value());

  const DimRef dChild   = core.findOrMintDimension("d.child");
  const DimRef dSibling = core.findOrMintDimension("d.sibling");
  const DimRef dThreads = core.findOrMintDimension("d.threads");

  // init(1) -> firstborn child bash(100) along d.child
  EXPECT_EQ(arena.linked(*cInit, dChild, DimVector::POS), *cBash);

  // bash(100) -> sshd(101) -> nginx(102) along d.sibling
  EXPECT_EQ(arena.linked(*cBash, dSibling, DimVector::POS), *cSshd);
  EXPECT_EQ(arena.linked(*cSshd, dSibling, DimVector::POS), *cNginx);
  EXPECT_EQ(arena.linked(*cNginx, dSibling, DimVector::POS), noCell);

  // bash(100) -> firstborn child make(200) along d.child
  EXPECT_EQ(arena.linked(*cBash, dChild, DimVector::POS), *cMake);

  // nginx(102) has 2 extra threads (103, 104) along d.threads
  const auto t103 = session.cellFor("thread:103");
  const auto t104 = session.cellFor("thread:104");
  ASSERT_TRUE(t103.has_value());
  ASSERT_TRUE(t104.has_value());

  EXPECT_EQ(arena.linked(*cNginx, dThreads, DimVector::POS), *t103);
  EXPECT_EQ(arena.linked(*t103, dThreads, DimVector::POS), *t104);
}

TEST(ProcLensTest, CommandDispatch) {
  ProcLens lens({
      {.pid        = 42,
       .ppid       = 1,
       .comm       = "worker",
       .state      = 'R',
       .cpuPercent = 50.0,
       .rssBytes   = 1024,
       .threadTids = {42}},
  });

  // Pause
  auto res = lens.dispatchCommand(42, "pause");
  EXPECT_TRUE(res.has_value());

  // Kill
  res = lens.dispatchCommand(42, "kill");
  EXPECT_TRUE(res.has_value());

  // Invalid PID
  res = lens.dispatchCommand(9999, "kill");
  EXPECT_FALSE(res.has_value());

  // Unknown command
  res = lens.dispatchCommand(42, "dance");
  EXPECT_FALSE(res.has_value());
}

} // namespace
