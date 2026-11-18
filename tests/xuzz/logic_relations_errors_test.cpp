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

#include "common/xanadu/vortex/logic_compile.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vprolog/compiler.hpp"
#include "common/xanadu/vprolog/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

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

} // namespace
