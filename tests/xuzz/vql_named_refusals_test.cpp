// A form the compiler does not lower refuses with a message that names the
// form, never as a misspelt function or an unbound variable that is bound.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using xanadu::vql::VQLEngine;

struct VQLNamedRefusalTest : testing::Test {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  VQLEngine engine{core};

  std::string refusalOf(const std::string &query) {
    try {
      (void)engine.execute(query);
    } catch (const std::runtime_error &error) {
      return error.what();
    }
    ADD_FAILURE() << "expected a refusal: " << query;
    return {};
  }
};

TEST_F(VQLNamedRefusalTest, ShapeValuedVariableNamesTheForm) {
  // `##{ $s }` lowers now (Q-SHAPEVAR); a shape variable as a predicate is the
  // form that still refuses, and it names itself.
  const auto message =
      refusalOf("let $s := { +d.a } return ##/d.items[{ $s }]");
  EXPECT_THAT(message, testing::HasSubstr("shape"));
  EXPECT_THAT(message, testing::Not(testing::HasSubstr("graph lowering")));
}

// lens(get:, put:) lowers since Q-LENS; what still refuses is a host lens
// provider, and it names itself as one.
TEST_F(VQLNamedRefusalTest, LensNamesItselfAndIsNotATypo) {
  const auto message =
      refusalOf("let $provider := lens(\"xanadoc\", \"a\") return ##");
  EXPECT_THAT(message, testing::HasSubstr("lens(\"xanadoc\", ...)"));
  EXPECT_THAT(message, testing::HasSubstr("host lens provider"));
  EXPECT_THAT(message, testing::Not(testing::HasSubstr("unknown")));
}

TEST_F(VQLNamedRefusalTest, PivotAndComputedDimensionNameTheirForms) {
  const auto source = arena.makeCell("task");
  engine.setVariable("task", source);
  // A pivot lowers to a dynamic view now (vql_dynamic_view_lowering_test.cpp);
  // it still refuses inside project() and apply_shape().
  EXPECT_THAT(refusalOf("project($task, { d.1 <=> d.priority })"),
              testing::HasSubstr("pivot"));
  EXPECT_THAT(refusalOf("$task{ +d.label := ./value() }"),
              testing::HasSubstr("computed dimension"));
}

TEST_F(VQLNamedRefusalTest, FieldBindingInAShapeNamesTheForm) {
  EXPECT_THAT(refusalOf("##/+d.items{ +d.title: $t }"),
              testing::HasSubstr("field binding in a shape"));
}

TEST_F(VQLNamedRefusalTest, UnboundShapeVariableIsATypeContractError) {
  EXPECT_THROW(engine.execute("##{ $missing_shape }"),
               xanadu::vql::TypeContractError);
}

} // namespace
