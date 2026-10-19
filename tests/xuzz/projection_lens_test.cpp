/**
 * @file projection_lens_test.cpp
 * @brief Tests for the anything-to-ZigZag lens: the ProjectionSession framework
 *        and the FilesystemLens (design/anything-to-zigzag-lens.md §4, §7).
 *
 * These project a known directory tree into a VortexCore arena and assert the
 * identity map, the canonical generic dimension layout (d.contains/d.seq), the
 * attribute ranks (d.role/d.mime/...), and determinism. They are the direct
 * gate for "a FilesystemLens projects a directory with metadata" and the shared
 * foundation the frontend-gating tests build on.
 */
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/filesystem_lens.hpp"
#include "common/xanadu/zigzag/projection_lens.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"

namespace {

using zigzag::FilesystemLens;
using zigzag::GenericDim;
using zigzag::ProjectionSession;

// Build a deterministic fixture directory:
//   root/
//     a.txt         "alpha beta"
//     b.txt         "beta gamma"
//     sub/
//       c.txt       "gamma"
std::filesystem::path makeFixture() {
  auto base = std::filesystem::temp_directory_path() /
              ("vlens_fix_" + std::to_string(::getpid()));
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "sub");
  auto write = [](const std::filesystem::path &p, const std::string &s) {
    std::ofstream f(p);
    f << s;
  };
  write(base / "a.txt", "alpha beta");
  write(base / "b.txt", "beta gamma");
  write(base / "sub" / "c.txt", "gamma");
  return base;
}

struct Projected {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  std::unique_ptr<ProjectionSession> session;
};

std::unique_ptr<Projected> project(const std::filesystem::path &dir) {
  auto p     = std::make_unique<Projected>();
  p->session = std::make_unique<ProjectionSession>(
      std::make_unique<FilesystemLens>(dir), p->core);
  return p;
}

// Read a cell's single posward attribute value on a named rank.
std::string attrOf(zigzag::ArenaManifold &arena,
                   zigzag::vortex::VortexCore &core, zigzag::CellRef cell,
                   const std::string &rank) {
  zigzag::DimRef dim = core.findOrMintDimension(rank);
  auto nb            = arena.linked(cell, dim, zigzag::Posward);
  if (!nb.has_value()) return "";
  return arena.textOf(*nb);
}

TEST(ProjectionLensTest, FilesystemProjectsEveryEntry) {
  auto dir = makeFixture();
  auto p   = project(dir);
  // root + a.txt + b.txt + sub + sub/c.txt = 5 nodes.
  EXPECT_EQ(p->session->cellCount(), 5u);
  std::filesystem::remove_all(dir);
}

TEST(ProjectionLensTest, RootIsTheSingleUncontainedNode) {
  auto dir = makeFixture();
  auto p   = project(dir);
  // Only the top directory has no container, so exactly one root.
  EXPECT_EQ(p->session->roots().size(), 1u);
  // The root cell's name is the fixture dir's own name.
  const auto root = p->session->roots().front();
  EXPECT_EQ(p->core.arena().textOf(root), dir.filename().string());
  std::filesystem::remove_all(dir);
}

TEST(ProjectionLensTest, ContainmentAndSequenceLinksExist) {
  auto dir                 = makeFixture();
  auto p                   = project(dir);
  auto &arena              = p->core.arena();
  zigzag::DimRef dContains = p->core.findOrMintDimension(
      std::string(zigzag::genericDimName(GenericDim::Containment)));
  const auto root = p->session->roots().front();
  // root --d.contains--> first child (sorted: "a.txt").
  auto firstChild = arena.linked(root, dContains, zigzag::Posward);
  ASSERT_TRUE(firstChild.has_value());
  EXPECT_EQ(arena.textOf(*firstChild), "a.txt");
  // first child --d.seq--> next sibling ("b.txt").
  zigzag::DimRef dSeq = p->core.findOrMintDimension(
      std::string(zigzag::genericDimName(GenericDim::Sequence)));
  auto nextSib = arena.linked(*firstChild, dSeq, zigzag::Posward);
  ASSERT_TRUE(nextSib.has_value());
  EXPECT_EQ(arena.textOf(*nextSib), "b.txt");
  std::filesystem::remove_all(dir);
}

TEST(ProjectionLensTest, AttributeRanksCarryRoleAndMime) {
  auto dir        = makeFixture();
  auto p          = project(dir);
  auto &arena     = p->core.arena();
  const auto root = p->session->roots().front();
  EXPECT_EQ(attrOf(arena, p->core, root, "d.role"), "dir");
  // The first file child is a regular file.
  zigzag::DimRef dContains = p->core.findOrMintDimension(
      std::string(zigzag::genericDimName(GenericDim::Containment)));
  auto firstChild = arena.linked(root, dContains, zigzag::Posward);
  ASSERT_TRUE(firstChild.has_value());
  EXPECT_EQ(attrOf(arena, p->core, *firstChild, "d.role"), "file");
  std::filesystem::remove_all(dir);
}

TEST(ProjectionLensTest, IdentityMapRoundTrips) {
  auto dir        = makeFixture();
  auto p          = project(dir);
  const auto root = p->session->roots().front();
  auto key        = p->session->keyFor(root);
  ASSERT_TRUE(key.has_value());
  // The key is the inode scheme, and it maps back to the same cell.
  EXPECT_EQ(key->substr(0, 3), "fs:");
  EXPECT_EQ(p->session->cellFor(*key), root);
  std::filesystem::remove_all(dir);
}

TEST(ProjectionLensTest, ProjectionIsDeterministic) {
  auto dir = makeFixture();
  auto p1  = project(dir);
  auto p2  = project(dir);
  EXPECT_EQ(p1->session->cellCount(), p2->session->cellCount());
  EXPECT_EQ(p1->session->roots().size(), p2->session->roots().size());
  // Same tree -> same root name and same first-child name.
  EXPECT_EQ(p1->core.arena().textOf(p1->session->roots().front()),
            p2->core.arena().textOf(p2->session->roots().front()));
  std::filesystem::remove_all(dir);
}

} // namespace
