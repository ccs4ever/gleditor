/**
 * @file lens_vql_gate_test.cpp
 * @brief Gates the REAL VQL engine against a source projected through the
 *        anything-to-ZigZag lens (ProjectionSession + FilesystemLens), proving
 *        a projected directory is traversed by the SAME VQL query strings that
 *        traverse a hand-built native slice of the identical shape
 *        (design/anything-to-zigzag-lens.md §9A.2: project into the bound arena
 *        so the query runtime sees the projected cells).
 *
 * The lens mints the CANONICAL generic dimension names (d.contains / d.seq) and
 * the attribute ranks (d.role / d.mime). A VQL path step names those same
 * strings, so one query string must work regardless of whether the cells came
 * from a lens or were authored by hand. These tests assert exactly that
 * equivalence, driving the MultiStoreCoordinator ctor of VQLEngine so canonical
 * dimension-name resolution (resolveDimension -> findOrMintDimension) is on the
 * live path.
 */
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/filesystem_lens.hpp"
#include "common/xanadu/zigzag/projection_lens.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"

namespace {

namespace fs = std::filesystem;

using xanadu::MultiStoreCoordinator;
using xanadu::vql::VQLEngine;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::FilesystemLens;
using zigzag::GenericDim;
using zigzag::ProjectionSession;

// A deterministic fixture directory:
//   root/
//     a.txt         "alpha"
//     b.txt         "beta"
//     sub/          (a child directory)
//       c.txt       "gamma"
// Sorted-DFS order makes root's children a.txt, b.txt, sub; the root reaches
// a.txt on d.contains, and a.txt reaches its siblings on d.seq.
fs::path makeFixture() {
  auto base = fs::temp_directory_path() /
              ("vql_lens_gate_" + std::to_string(::getpid()));
  fs::remove_all(base);
  fs::create_directories(base / "sub");
  auto write = [](const fs::path &p, const std::string &s) {
    std::ofstream f(p);
    f << s;
  };
  write(base / "a.txt", "alpha");
  write(base / "b.txt", "beta");
  write(base / "sub" / "c.txt", "gamma");
  return base;
}

// Everything a projected source needs: one arena, the VortexCore bound to it,
// a coordinator over that same core (so VQL and the projection share one
// arena), the session, and the engine the coordinator drives.
struct ProjectedSource {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coord{core};
  std::unique_ptr<ProjectionSession> session;
  VQLEngine engine{coord};
  CellRef root{zigzag::noCell};

  explicit ProjectedSource(const fs::path &dir) {
    session = std::make_unique<ProjectionSession>(
        std::make_unique<FilesystemLens>(dir), core);
    root = session->roots().front();
  }
};

// A hand-built NATIVE slice of the identical shape, authored directly into an
// arena with arena.link on the canonical ranks -- the "native slice" the gate
// compares the projection against.
struct NativeSlice {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coord{core};
  VQLEngine engine{coord};
  CellRef root{zigzag::noCell};

  explicit NativeSlice(const std::string &rootName) {
    const DimRef dContains = core.findOrMintDimension(
        std::string(zigzag::genericDimName(GenericDim::Containment)));
    const DimRef dSeq = core.findOrMintDimension(
        std::string(zigzag::genericDimName(GenericDim::Sequence)));
    const DimRef dRole = core.findOrMintDimension("d.role");

    root           = arena.makeCell(std::string_view{rootName});
    CellRef aTxt   = arena.makeCell("a.txt");
    CellRef bTxt   = arena.makeCell("b.txt");
    CellRef subDir = arena.makeCell("sub");

    // root --d.contains--> a.txt --d.seq--> b.txt --d.seq--> sub
    (void)arena.link(root, dContains, DimVector::POS, aTxt);
    (void)arena.link(aTxt, dSeq, DimVector::POS, bTxt);
    (void)arena.link(bTxt, dSeq, DimVector::POS, subDir);

    // Attribute ranks mirror the lens's d.role attachment.
    auto attachRole = [&](CellRef cell, const std::string &role) {
      CellRef r = arena.makeCell(std::string_view{role});
      (void)arena.link(cell, dRole, DimVector::POS, r);
    };
    attachRole(root, "dir");
    attachRole(aTxt, "file");
    attachRole(bTxt, "file");
    attachRole(subDir, "dir");
  }
};

// Run one query string from a bound root variable and return the result names,
// so projection and native results can be compared as plain strings.
std::vector<std::string> names(VQLEngine &engine, zigzag::ArenaManifold &arena,
                               CellRef root, const std::string &query) {
  engine.setVariable("root", root);
  const auto cells = engine.execute(query);
  std::vector<std::string> out;
  out.reserve(cells.size());
  for (CellRef c : cells) out.push_back(arena.textOf(c));
  return out;
}

// --- The gate --------------------------------------------------------------

// The projection is reachable at all: the FilesystemLens minted five nodes and
// a single root, exactly as the shared projection fixture asserts.
TEST(LensVqlGate, ProjectionHasARoot) {
  const auto dir = makeFixture();
  ProjectedSource src(dir);
  EXPECT_EQ(src.session->cellCount(), 5u);
  ASSERT_EQ(src.session->roots().size(), 1u);
  EXPECT_EQ(src.core.arena().textOf(src.root), dir.filename().string());
  fs::remove_all(dir);
}

// The canonical containment step from the root reaches the first child, run by
// the real VQL engine over projected cells.
TEST(LensVqlGate, ContainsStepReachesFirstChild) {
  const auto dir = makeFixture();
  ProjectedSource src(dir);
  const auto hit =
      names(src.engine, src.core.arena(), src.root, "$root/d.contains");
  ASSERT_EQ(hit.size(), 1u);
  EXPECT_EQ(hit[0], "a.txt");
  fs::remove_all(dir);
}

// The canonical sequence step then walks the siblings. VQL's dimension step
// traverses the whole posward run (as ##/d.people does), so from the first
// child the d.seq run is the rest of the siblings in order: b.txt then sub.
TEST(LensVqlGate, ContainsThenSequenceWalksSiblings) {
  const auto dir = makeFixture();
  ProjectedSource src(dir);
  const auto sibs =
      names(src.engine, src.core.arena(), src.root, "$root/d.contains/d.seq");
  ASSERT_EQ(sibs.size(), 2u);
  EXPECT_EQ(sibs[0], "b.txt");
  EXPECT_EQ(sibs[1], "sub");
  fs::remove_all(dir);
}

// The attribute rank the lens attaches is navigable by the same path grammar:
// the first file's d.role is "file".
TEST(LensVqlGate, AttributeRankIsNavigable) {
  const auto dir = makeFixture();
  ProjectedSource src(dir);
  const auto role =
      names(src.engine, src.core.arena(), src.root, "$root/d.contains/d.role");
  ASSERT_EQ(role.size(), 1u);
  EXPECT_EQ(role[0], "file");
  // The root itself is a directory.
  const auto rootRole =
      names(src.engine, src.core.arena(), src.root, "$root/d.role");
  ASSERT_EQ(rootRole.size(), 1u);
  EXPECT_EQ(rootRole[0], "dir");
  fs::remove_all(dir);
}

// The point of the whole exercise: the IDENTICAL query string, run against a
// projected source and a hand-authored native slice of the same shape, yields
// the same result. A projected source is usable from VQL as easily as a native
// slice.
TEST(LensVqlGate, SameQueryTraversesProjectionAndNativeAlike) {
  const auto dir = makeFixture();
  ProjectedSource proj(dir);
  NativeSlice nat(dir.filename().string());

  // Several query strings; each must agree between the two sources.
  const std::vector<std::string> queries = {
      "$root",        "$root/d.contains",        "$root/d.contains/d.seq",
      "$root/d.role", "$root/d.contains/d.role",
  };
  for (const auto &q : queries) {
    const auto a = names(proj.engine, proj.core.arena(), proj.root, q);
    const auto b = names(nat.engine, nat.arena, nat.root, q);
    EXPECT_EQ(a, b) << "query diverged between projection and native: " << q;
  }
  fs::remove_all(dir);
}

} // namespace
