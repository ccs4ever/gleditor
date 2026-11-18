/**
 * @file logic_relations_errors_test.cpp
 * @brief The stdlib, store and arena relations: errors for ill-typed
 * arguments, the cells an enumeration may see, and the settings model cache.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <set>
#include <string>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vortex/logic_compile.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vprolog/compiler.hpp"
#include "common/xanadu/vprolog/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex::logic {
// Defined in logic_relations.cpp: how many times a provider has built the
// settings model from a bound store. A test seam, like ownedGoalSignatures().
std::size_t settingsModelBuilds() noexcept;
} // namespace zigzag::vortex::logic

namespace {

using namespace zigzag;
using namespace zigzag::vortex;

struct RelationHarness {
  ArenaManifold arena;
  VortexCore core{arena};
  xanadu::vprolog::Compiler compiler{core};

  std::vector<std::string> all(const std::string &query, const std::string &var,
                               std::size_t max = 100000) {
    const auto compiled = compiler.compileQuery(query);
    std::vector<std::string> out;
    for (const auto &sol : compiler.solve(compiled, max)) {
      const auto it = sol.formatted.find(var);
      out.push_back(it == sol.formatted.end() ? "?" : it->second);
    }
    return out;
  }

  // The message of the exception the query raises, or "" when it ran.
  std::string errorOf(const std::string &query) {
    try {
      (void)all(query, "X", 1);
    } catch (const std::exception &err) {
      return err.what();
    }
    return {};
  }
};

// Every relation, one argument position at a time, bound to a value of the
// wrong type. Each is an ISO type error naming the relation, not a quiet
// `false.`, the way arithmetic on a non-number is.
TEST(LogicRelationErrorsTest, WrongTypeArgumentsRaiseNamedTypeErrors) {
  const std::vector<std::pair<std::string, std::string>> cases{
      {"cell_value(foo, V).", "cell_value/2"},
      {"cell_value(1.5, V).", "cell_value/2"},
      {"cell_value(f(x), V).", "cell_value/2"},
      {"cell_link(foo, D, R, T).", "cell_link/4"},
      {"cell_link(1, 2, R, T).", "cell_link/4"},
      {"cell_link(1, bar, 3, T).", "cell_link/4"},
      {"cell_link(1, bar, pos, baz).", "cell_link/4"},
      {"cell_span(foo, S).", "cell_span/2"},
      {"cell_span(1, 2).", "cell_span/2"},
      {"bridge_edge(foo, D).", "bridge_edge/2"},
      {"bridge_edge(1, foo).", "bridge_edge/2"},
      {"bridge_edge(foo, d, V).", "bridge_edge/3"},
      {"bridge_edge(1, 2, V).", "bridge_edge/3"},
      {"bridge_edge(1, d, foo).", "bridge_edge/3"},
      {"vortex_module(1).", "vortex_module/1"},
      {"stdlib_module(f(x)).", "stdlib_module/1"},
      {"vortex_function(1).", "vortex_function/1"},
      {"vortex_function(a, 2).", "vortex_function/2"},
      {"stdlib_function(1, f).", "stdlib_function/2"},
      {"vortex_function(a, b, 3).", "vortex_function/3"},
      {"vortex_instruction(1, S, L).", "vortex_instruction/3"},
      {"vortex_instruction(p, one, L).", "vortex_instruction/3"},
      {"vortex_contract(p, 1, L).", "vortex_contract/3"},
      {"vortex_param(p, w, slot, T).", "vortex_param/4"},
      {"vortex_param(p, w, 1, target).", "vortex_param/4"},
      {"setting(1, V).", "setting/2"},
      {"setting_shape(1, V).", "setting_shape/2"},
      {"setting_default(f(x), V).", "setting_default/2"},
      {"transclude(1, S, T).", "transclude/3"},
      {"xanalink(L, 2, R).", "xanalink/3"},
  };
  for (const auto &[goal, name] : cases) {
    RelationHarness h;
    const std::string err = h.errorOf(goal);
    EXPECT_NE(err.find("type error"), std::string::npos) << goal << ": " << err;
    EXPECT_NE(err.find(name), std::string::npos) << goal << ": " << err;
  }
}

// An unbound variable is the generative mode, and a well-typed bound argument
// that matches nothing is an ordinary failure: neither is an error.
TEST(LogicRelationErrorsTest, WellTypedArgumentsAreNotErrors) {
  for (const std::string goal :
       {"cell_value(C, V).", "cell_value(999999999, V).",
        "cell_link(C, D, R, T).", "cell_link(1, bar, pos, T).",
        "cell_span(C, S).", "bridge_edge(U, D).", "bridge_edge(U, d, V).",
        "vortex_module(nosuch).", "vortex_function(F).",
        "vortex_instruction(p, 0, L).", "vortex_param(p, w, 1, T).",
        "setting(nosuch, V).", "transclude(a, b, c).", "xanalink(l, t, r)."}) {
    RelationHarness h;
    EXPECT_EQ(h.errorOf(goal), "") << goal;
  }
}

// A known relation called at an arity it does not have names the arities it
// does; an unknown name still fails without an error (the recorded ruling).
TEST(LogicRelationErrorsTest, RelationAtTheWrongArityRefusesByName) {
  RelationHarness h;
  const std::string err = h.errorOf("cell_link(foo, 2, 3).");
  EXPECT_NE(err.find("cell_link/3"), std::string::npos) << err;
  EXPECT_NE(err.find("cell_link/4"), std::string::npos) << err;
  EXPECT_EQ(h.errorOf("nosuch_relation(1, 2, 3)."), "");
}

// The rule for a scan with an unbound cell argument: it visits the cells that
// existed when the solve started and were minted after the engine's own
// bootstrap, each once. Cells the query mints while it runs (terms, compiled
// ops, activation frames) and the bootstrap machinery (dimensions, std
// modules, seeded clauses) are never answers.
TEST(LogicRelationScanTest, ScanSeesOnlyUserCellsExistingAtSolveStart) {
  RelationHarness h;
  auto &arena         = h.core.arena();
  const CellRef alpha = arena.makeCell("alpha");
  const CellRef seven = arena.makeScalarCell(std::int64_t{7});
  const CellRef dim   = arena.makeCell("d.test");
  ASSERT_TRUE(arena.link(alpha, dim, DimVector::POS, seven));

  const auto compiled =
      h.compiler.compileQuery("cell_value(C, V), T = f(C, V, g(C)).");
  const std::size_t first = h.compiler.stdlib().bootstrapCellCount();
  const std::size_t last  = arena.cellCount();
  ASSERT_GT(last, first);

  const auto idsOf = [&] {
    std::vector<std::uint64_t> ids;
    for (const auto &sol : h.compiler.solve(compiled, 100000))
      ids.push_back(std::stoull(sol.formatted.at("C")));
    return ids;
  };
  const auto firstRun = idsOf();
  // The run minted terms and frames above the snapshot; none is visible, and a
  // second run of the same query sees exactly the same cells.
  EXPECT_EQ(arena.cellCount(), last);
  EXPECT_EQ(idsOf(), firstRun);

  std::set<std::uint64_t> distinct(firstRun.begin(), firstRun.end());
  EXPECT_EQ(distinct.size(), firstRun.size()) << "a cell was visited twice";
  EXPECT_EQ(firstRun.size(), last - std::max<std::size_t>(first, 1));
  EXPECT_EQ(*distinct.begin(), ArenaManifold::refOf(static_cast<std::uint32_t>(
                                   std::max<std::size_t>(first, 1))));
  EXPECT_EQ(*distinct.rbegin(),
            ArenaManifold::refOf(static_cast<std::uint32_t>(last - 1)));
  EXPECT_TRUE(distinct.contains(alpha));
  EXPECT_TRUE(distinct.contains(seven));

  const auto values = h.all("cell_value(C, V).", "V");
  EXPECT_NE(std::find(values.begin(), values.end(), "alpha"), values.end());
  EXPECT_EQ(std::find(values.begin(), values.end(), "d.dims"), values.end())
      << "an engine dimension cell was enumerated";
}

TEST(LogicRelationScanTest, LinkAndSpanScansUseTheSameRange) {
  RelationHarness h;
  auto &arena         = h.core.arena();
  const CellRef alpha = arena.makeCell("alpha");
  const CellRef beta  = arena.makeCell("beta");
  const CellRef dim   = arena.makeCell("d.test");
  ASSERT_TRUE(arena.link(alpha, dim, DimVector::POS, beta));

  const auto from = h.all("cell_link(F, 'd.test', pos, T).", "F");
  ASSERT_EQ(from.size(), 1U);
  EXPECT_EQ(from.front(), std::to_string(alpha));
  // No engine link (a dimension rank, a clause chain) appears for an unbound
  // dimension either: every answer starts at a cell minted after bootstrap.
  const std::uint64_t floor = ArenaManifold::refOf(static_cast<std::uint32_t>(
      std::max<std::size_t>(h.compiler.stdlib().bootstrapCellCount(), 1)));
  for (const auto &id : h.all("cell_link(F, D, R, T).", "F"))
    EXPECT_GE(std::stoull(id), floor) << id;
  for (const auto &id : h.all("cell_span(C, S).", "C"))
    EXPECT_GE(std::stoull(id), floor) << id;
  for (const auto &id : h.all("bridge_edge(U, D, V).", "U"))
    EXPECT_GE(std::stoull(id), floor) << id;
}

// Enumerating the settings of a bound store used to rebuild the whole model on
// every answer, quadratic in the settings. One solve builds it once, and the
// next solve builds a fresh one (the store may have changed between them).
TEST(LogicRelationSettingsTest, BoundStoreModelIsBuiltOncePerSolve) {
  RelationHarness h;
  xanadu::Store store;
  store.setSystem(true);
  xanadu::initializeSystemStore(store, xanadu::SystemDocKind::Settings);
  h.compiler.stdlib().setBoundStore(&store);

  const std::size_t before = logic::settingsModelBuilds();
  const auto names         = h.all("setting(K, V).", "K");
  ASSERT_GE(names.size(), 5U);
  EXPECT_EQ(logic::settingsModelBuilds() - before, 1U);

  // The three relations of one solve share the model.
  (void)h.all("setting(K, V), setting_default(K, D), setting_shape(K, S).",
              "K");
  EXPECT_EQ(logic::settingsModelBuilds() - before, 2U);
}

} // namespace
