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
  EXPECT_THAT(refusalOf("let $s := { +d.a } return ##{ $s }"),
              testing::HasSubstr("shape-valued"));
  EXPECT_THAT(refusalOf("let $s := { +d.a } return ##{ $s }"),
              testing::Not(testing::HasSubstr("graph lowering")));
}

TEST_F(VQLNamedRefusalTest, LensNamesItselfAndIsNotATypo) {
  const auto message =
      refusalOf("let $editable := lens(get: \\(.) ./value(), "
                "put: \\(cell, val) $cell/value($val)) return ##");
  EXPECT_THAT(message, testing::HasSubstr("lens(...)"));
  EXPECT_THAT(message, testing::HasSubstr("is not compiled to Vortex"));
  EXPECT_THAT(message, testing::Not(testing::HasSubstr("unknown")));
}

TEST_F(VQLNamedRefusalTest, PivotAndComputedDimensionNameTheirForms) {
  const auto source = arena.makeCell("task");
  engine.setVariable("task", source);
  EXPECT_THAT(refusalOf("$task{ d.1 <=> d.priority }"),
              testing::HasSubstr("dimension pivot"));
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
