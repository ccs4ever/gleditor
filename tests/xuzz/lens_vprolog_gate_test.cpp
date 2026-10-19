/**
 * @file lens_vprolog_gate_test.cpp
 * @brief Gate the vprolog / Vlog logic frontend directly against a source
 *        projected through the anything-to-ZigZag lens
 *        (ProjectionLens/ProjectionSession/FilesystemLens).
 *
 * The point of this gate is to prove a projected source is queryable as
 * logical facts over the SAME arena, exactly as any native ZigZag structure
 * would be. A FilesystemLens projects a directory tree into a VortexCore's
 * ArenaManifold; the std:logic fact builtins (cell_link/4, cell_value/2) read
 * that same arena, so the real Vlog solver -- VortexStdLib::solveQuery, the
 * engine vprolog itself runs -- enumerates the projected cells with no extra
 * wiring beyond sharing the arena.
 *
 * These tests drive the REAL solver (solveQuery over makeTerm goals, with
 * variable backtracking and multi-goal conjunction), not a hand-rolled arena
 * walk. The only thing stitched together is the arena the projection writes to
 * and the core the stdlib reads from -- which is precisely the
 * project-into-bound-arena contract in design/anything-to-zigzag-lens.md
 * §9A.2.
 */
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/filesystem_lens.hpp"
#include "common/xanadu/zigzag/projection_lens.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"

namespace {

using zigzag::CellRef;
using zigzag::FilesystemLens;
using zigzag::GenericDim;
using zigzag::ProjectionSession;
using namespace zigzag::vortex;

// Build a deterministic fixture directory (2 files + a subdir with 1 file):
//   root/
//     a.txt         "alpha beta"
//     b.txt         "beta gamma"
//     sub/
//       c.txt       "gamma"
std::filesystem::path makeFixture() {
  auto base = std::filesystem::temp_directory_path() /
              ("vlens_vprolog_" + std::to_string(::getpid()));
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

// A projected source: one arena, one core, one VM, one stdlib (the real Vlog
// frontend), and the session that projected the filesystem lens INTO that
// core's arena. Everything the solver reads is in `core.arena()`.
struct ProjectedLogic {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  VortexStdLib stdlib{core, vm};
  std::unique_ptr<ProjectionSession> session;
};

std::unique_ptr<ProjectedLogic> project(const std::filesystem::path &dir) {
  auto p     = std::make_unique<ProjectedLogic>();
  p->session = std::make_unique<ProjectionSession>(
      std::make_unique<FilesystemLens>(dir), p->core);
  return p;
}

// The root cell as an arena scalar id, ready to bind as a cell_link/cell_value
// argument (the fact builtins deref an int64 id back to the CellRef).
CellRef rootIdCell(ProjectedLogic &p) {
  const CellRef root = p.session->roots().front();
  return p.core.arena().makeScalarCell(static_cast<std::int64_t>(root));
}

} // namespace

// A projected root answers cell_link(root, Dim, Dir, To): the real solver
// enumerates the root's outgoing links over the projected arena. The root is a
// directory, so it must have at least one d.contains edge to a child, plus its
// d.role attribute edge.
TEST(LensVprologGate, RootLinksAreEnumerableAsLogicFacts) {
  auto dir = makeFixture();
  auto p   = project(dir);

  CellRef dimVar = p->stdlib.makeVar("Dim");
  CellRef dirVar = p->stdlib.makeVar("Dir");
  CellRef toVar  = p->stdlib.makeVar("To");
  CellRef goal =
      p->stdlib.makeTerm("cell_link", {rootIdCell(*p), dimVar, dirVar, toVar});

  auto sols = p->stdlib.solveQuery(goal);
  ASSERT_FALSE(sols.empty())
      << "the real Vlog solver found no links out of the projected root";

  // Collect the dimensions the solver reported on the root.
  std::vector<std::string> dims;
  for (const auto &s : sols) {
    auto it = s.formatted.find("Dim");
    if (it != s.formatted.end()) dims.push_back(it->second);
    // Every reported edge is posward in this projection.
    auto dit = s.formatted.find("Dir");
    ASSERT_NE(dit, s.formatted.end());
    EXPECT_EQ(dit->second, "pos");
  }
  // The containment edge and the role attribute edge are both present.
  EXPECT_NE(std::find(dims.begin(), dims.end(), "d.contains"), dims.end())
      << "projected root exposes no d.contains child as a logic fact";
  EXPECT_NE(std::find(dims.begin(), dims.end(), "d.role"), dims.end())
      << "projected root exposes no d.role attribute as a logic fact";

  std::filesystem::remove_all(dir);
}

// cell_link(root, 'd.contains', 'pos', To) with a BOUND dimension and
// direction narrows the solver to the containment child -- the same selective
// query a native slice would answer. The child's text (via cell_value) is the
// first sorted entry, "a.txt".
TEST(LensVprologGate, ContainmentChildIsSelectableByBoundDimension) {
  auto dir = makeFixture();
  auto p   = project(dir);

  CellRef dContains = p->core.arena().makeCell(
      std::string(zigzag::genericDimName(GenericDim::Containment)));
  CellRef posDir = p->core.arena().makeCell("pos");
  CellRef toVar  = p->stdlib.makeVar("To");

  // Conjunction: cell_link(root,'d.contains','pos',To), cell_value(To,Text).
  CellRef textVar  = p->stdlib.makeVar("Text");
  CellRef linkGoal = p->stdlib.makeTerm(
      "cell_link", {rootIdCell(*p), dContains, posDir, toVar});
  CellRef valueGoal = p->stdlib.makeTerm("cell_value", {toVar, textVar});
  std::vector<CellRef> goals{linkGoal, valueGoal};

  auto sols = p->stdlib.solveQuery(goals);
  ASSERT_FALSE(sols.empty())
      << "no d.contains child reachable from the projected root via Vlog";
  // The root's single posward containment neighbour is the first sorted entry.
  EXPECT_EQ(sols.front().formatted.at("Text"), "a.txt");

  std::filesystem::remove_all(dir);
}

// role(C,'dir') shaped as a two-goal conjunction over the fact surface:
// cell_link(root,'d.role','pos',R), cell_value(R,Role). The root directory's
// role attribute reads "dir" -- a projected attribute answered as a logic fact.
TEST(LensVprologGate, RoleAttributeIsAnswerableAsAFact) {
  auto dir = makeFixture();
  auto p   = project(dir);

  CellRef dRole       = p->core.arena().makeCell("d.role");
  CellRef posDir      = p->core.arena().makeCell("pos");
  CellRef roleCellVar = p->stdlib.makeVar("R");
  CellRef roleVar     = p->stdlib.makeVar("Role");

  CellRef linkGoal = p->stdlib.makeTerm(
      "cell_link", {rootIdCell(*p), dRole, posDir, roleCellVar});
  CellRef valueGoal = p->stdlib.makeTerm("cell_value", {roleCellVar, roleVar});
  std::vector<CellRef> goals{linkGoal, valueGoal};

  auto sols = p->stdlib.solveQuery(goals);
  ASSERT_FALSE(sols.empty())
      << "projected root has no d.role attribute reachable through Vlog";
  EXPECT_EQ(sols.front().formatted.at("Role"), "dir")
      << "the root directory's role fact should read 'dir'";

  std::filesystem::remove_all(dir);
}

// The file child's role fact reads "file": navigate root --d.contains--> child,
// then that child --d.role--> role cell, all as solver goals. This proves the
// fact surface composes across projected hops, not just off the root.
TEST(LensVprologGate, FileChildRoleFactReadsFile) {
  auto dir = makeFixture();
  auto p   = project(dir);

  CellRef dContains = p->core.arena().makeCell(
      std::string(zigzag::genericDimName(GenericDim::Containment)));
  CellRef dRole  = p->core.arena().makeCell("d.role");
  CellRef posDir = p->core.arena().makeCell("pos");

  CellRef childVar    = p->stdlib.makeVar("Child");
  CellRef roleCellVar = p->stdlib.makeVar("R");
  CellRef roleVar     = p->stdlib.makeVar("Role");

  // root --d.contains--> Child, Child --d.role--> R, cell_value(R, Role).
  CellRef g1 = p->stdlib.makeTerm(
      "cell_link", {rootIdCell(*p), dContains, posDir, childVar});
  CellRef g2 =
      p->stdlib.makeTerm("cell_link", {childVar, dRole, posDir, roleCellVar});
  CellRef g3 = p->stdlib.makeTerm("cell_value", {roleCellVar, roleVar});
  std::vector<CellRef> goals{g1, g2, g3};

  auto sols = p->stdlib.solveQuery(goals);
  ASSERT_FALSE(sols.empty())
      << "could not reach a file child's role through chained Vlog goals";
  EXPECT_EQ(sols.front().formatted.at("Role"), "file")
      << "the first contained child is a regular file";

  std::filesystem::remove_all(dir);
}

// A fully-unbound cell_value(C, V) over the projected arena enumerates every
// projected cell's value -- the generative mode of the real solver, bounded by
// the solver's maxSolutions cap. The FilesystemLens projects each entry's NAME
// as the cell's text (node.content = e.name), so the entry names must appear as
// enumerated facts, proving the projected text landed in the arena as queryable
// facts a logic program can scan without any bound starting cell.
TEST(LensVprologGate, UnboundCellValueEnumeratesProjectedEntryNames) {
  auto dir = makeFixture();
  auto p   = project(dir);

  CellRef cVar = p->stdlib.makeVar("C");
  CellRef vVar = p->stdlib.makeVar("V");
  CellRef goal = p->stdlib.makeTerm("cell_value", {cVar, vVar});

  // Raise the solution cap well past the bootstrap machinery cells (dimensions,
  // the std:* modules, VM opcodes) the core mints before any projection, so the
  // enumeration reaches the projected filesystem cells minted after them.
  auto sols = p->stdlib.solveQuery(goal, /*customPredicates=*/{},
                                   /*maxSolutions=*/100000);
  ASSERT_FALSE(sols.empty())
      << "the solver enumerated no cell values over the projected arena";

  std::vector<std::string> values;
  values.reserve(sols.size());
  for (const auto &s : sols) {
    auto it = s.formatted.find("V");
    if (it != s.formatted.end()) values.push_back(it->second);
  }
  // Every projected directory entry's name is present as an enumerated fact.
  EXPECT_NE(std::find(values.begin(), values.end(), "a.txt"), values.end());
  EXPECT_NE(std::find(values.begin(), values.end(), "b.txt"), values.end());
  EXPECT_NE(std::find(values.begin(), values.end(), "sub"), values.end());
  EXPECT_NE(std::find(values.begin(), values.end(), "c.txt"), values.end());
  // The root directory's own name (the fixture dir) is projected too.
  EXPECT_NE(std::find(values.begin(), values.end(), dir.filename().string()),
            values.end());

  std::filesystem::remove_all(dir);
}
