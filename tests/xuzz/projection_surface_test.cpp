/**
 * @file projection_surface_test.cpp
 * @brief zigzag::ProjectionSurface -- the arena/core/session bundle whose
 *        reset() is the lens-swap the xuzz LensSurfaceContributor drives on
 *        a lens.open-source pick (apps/common/xanadu/zigzag/
 *        projection_surface.hpp).
 */
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

#include "common/xanadu/zigzag/config_lens.hpp"
#include "common/xanadu/zigzag/filesystem_lens.hpp"
#include "common/xanadu/zigzag/projection_surface.hpp"

namespace {

namespace fs = std::filesystem;

// root/
//   a.txt
//   sub/
//     b.txt
struct Fixture {
  fs::path root;
  Fixture() {
    root = fs::temp_directory_path() /
           ("proj_surface_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "sub");
    std::ofstream(root / "a.txt") << "a\n";
    std::ofstream(root / "sub" / "b.txt") << "b\n";
  }
  ~Fixture() { fs::remove_all(root); }
};

TEST(ProjectionSurfaceTest, ProjectsTheInitialLens) {
  Fixture fix;
  zigzag::ProjectionSurface surface(
      std::make_unique<zigzag::FilesystemLens>(fix.root));
  // root + a.txt + sub + b.txt.
  EXPECT_EQ(surface.session().cellCount(), 4U);
  EXPECT_EQ(surface.session().roots().size(), 1U);
}

// The swapLens seam: a reset swaps the session's lens and rebuilds the
// projection under a fresh arena, so the new cell count reflects the new
// source -- not the old surface's cells.
TEST(ProjectionSurfaceTest, ResetSwapsTheLens) {
  Fixture fix;
  zigzag::ProjectionSurface surface(
      std::make_unique<zigzag::FilesystemLens>(fix.root));
  ASSERT_EQ(surface.session().cellCount(), 4U);

  auto lens = zigzag::ConfigLens::fromText(
      "/cfg.json", R"json({"a": 1, "b": [true, "x"]})json");
  ASSERT_TRUE(lens.has_value()) << lens.error();
  surface.reset(std::move(*lens));

  EXPECT_NE(dynamic_cast<const zigzag::ConfigLens *>(&surface.session().lens()),
            nullptr);
  // cfg.file + a + b + b[0] + b[1] = 5 projected nodes.
  EXPECT_EQ(surface.session().cellCount(), 5U);
  // The arena was rebuilt with the session: it holds only the fresh
  // projection's cells, so old refs cannot straddle the swap.
  EXPECT_TRUE(surface.arena().contains(surface.session().roots().front()));
}

TEST(ProjectionSurfaceTest, SecondResetSwapsAgain) {
  Fixture fix;
  zigzag::ProjectionSurface surface(
      std::make_unique<zigzag::FilesystemLens>(fix.root));
  surface.reset(std::make_unique<zigzag::FilesystemLens>(fix.root / "sub"));
  EXPECT_NE(
      dynamic_cast<const zigzag::FilesystemLens *>(&surface.session().lens()),
      nullptr);
  // The sub/ root plus b.txt.
  EXPECT_EQ(surface.session().cellCount(), 2U);
}

} // namespace
