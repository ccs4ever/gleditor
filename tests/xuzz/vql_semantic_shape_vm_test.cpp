#include <gtest/gtest.h>

#include <string>

#include "common/xanadu/vql/vql_engine.hpp"

namespace {

using xanadu::vql::VQLEngine;
using zigzag::DimVector;

class VQLSemanticShapeVmTest : public ::testing::Test {
protected:
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  VQLEngine engine{core};

  zigzag::CellRef subject = arena.makeCell("subject");
  zigzag::CellRef kept    = arena.makeCell("kept");
  zigzag::CellRef omitted = arena.makeCell("omitted");
  zigzag::DimRef keep     = engine.resolveDimension("d.keep");
  zigzag::DimRef omit     = engine.resolveDimension("d.omit");
  zigzag::DimRef edge     = engine.resolveDimension("d.edge");

  void SetUp() override {
    ASSERT_TRUE(arena.link(subject, keep, DimVector::POS, kept));
    ASSERT_TRUE(arena.link(subject, omit, DimVector::POS, omitted));
    engine.setVariable("s", subject);
  }

  void expectUntouched() {
    EXPECT_EQ(arena.linked(subject, keep, DimVector::POS), kept);
    EXPECT_EQ(arena.linked(kept, keep, DimVector::NEG), subject);
    EXPECT_EQ(arena.linked(subject, omit, DimVector::POS), omitted);
    EXPECT_EQ(arena.linked(omitted, omit, DimVector::NEG), subject);
  }

  std::string refusalOf(const std::string &query) {
    try {
      engine.execute(query);
    } catch (const std::runtime_error &error) {
      return error.what();
    }
    return {};
  }
};

TEST_F(VQLSemanticShapeVmTest, ApplyShapeMasksInPlaceAndReturnsSubject) {
  EXPECT_EQ(engine.execute("apply_shape($s, {+d.keep})"),
            std::vector<zigzag::CellRef>{subject});
  EXPECT_EQ(arena.linked(subject, keep, DimVector::POS), kept);
  EXPECT_FALSE(arena.linked(subject, omit, DimVector::POS));
  EXPECT_FALSE(arena.linked(omitted, omit, DimVector::NEG));
}

TEST_F(VQLSemanticShapeVmTest, ApplyShapeWildcardKeepsRuleOrder) {
  engine.execute("apply_shape($s, {*, !d.omit})");
  EXPECT_EQ(arena.linked(subject, keep, DimVector::POS), kept);
  EXPECT_FALSE(arena.linked(subject, omit, DimVector::POS));
}

TEST_F(VQLSemanticShapeVmTest, ApplyShapeAssignsReciprocally) {
  const auto target = arena.makeCell("target");
  engine.setVariable("target", target);
  EXPECT_EQ(engine.execute("apply_shape($s, {+d.edge: $target})"),
            std::vector<zigzag::CellRef>{subject});
  EXPECT_EQ(arena.linked(subject, edge, DimVector::POS), target);
  EXPECT_EQ(arena.linked(target, edge, DimVector::NEG), subject);
  EXPECT_FALSE(arena.linked(subject, keep, DimVector::POS));
  EXPECT_FALSE(arena.linked(subject, omit, DimVector::POS));
}

TEST_F(VQLSemanticShapeVmTest, TargetsAreResolvedBeforeTheMaskSevers) {
  engine.execute("apply_shape($s, {+d.edge: $s/+d.omit})");
  EXPECT_EQ(arena.linked(subject, edge, DimVector::POS), omitted);
  EXPECT_FALSE(arena.linked(subject, omit, DimVector::POS));
}

TEST_F(VQLSemanticShapeVmTest, ApplyShapeMasksEveryStreamSubject) {
  const auto other = arena.makeCell("other");
  const auto leaf  = arena.makeCell("leaf");
  ASSERT_TRUE(arena.link(other, omit, DimVector::POS, leaf));
  engine.setVariable("both", std::vector<zigzag::CellRef>{subject, other});
  const auto result = engine.execute("apply_shape($both, {+d.keep})");
  EXPECT_EQ(result, (std::vector<zigzag::CellRef>{subject, other}));
  EXPECT_FALSE(arena.linked(subject, omit, DimVector::POS));
  EXPECT_FALSE(arena.linked(other, omit, DimVector::POS));
  EXPECT_EQ(arena.linked(subject, keep, DimVector::POS), kept);
}

TEST_F(VQLSemanticShapeVmTest, AssignmentsOverSeveralSubjectsRefuseByName) {
  const auto other  = arena.makeCell("other");
  const auto target = arena.makeCell("target");
  engine.setVariable("both", std::vector<zigzag::CellRef>{subject, other});
  engine.setVariable("target", target);
  const auto message = refusalOf("apply_shape($both, {+d.edge: $target})");
  EXPECT_NE(message.find("cell-identity"), std::string::npos) << message;
  expectUntouched();
  EXPECT_FALSE(arena.linked(target, edge, DimVector::NEG));
}

TEST_F(VQLSemanticShapeVmTest, UnsupportedRulesRefuseBeforeAnyEffect) {
  const auto message =
      refusalOf("apply_shape($s, {+d.keep, d.keep <=> d.omit})");
  EXPECT_NE(message.find("apply_shape"), std::string::npos) << message;
  expectUntouched();
}

TEST_F(VQLSemanticShapeVmTest, NestedShapeAssignmentRefuses) {
  const auto message =
      refusalOf("apply_shape($s, {+d.edge: {+d.inner: \"expected\"}})");
  EXPECT_NE(message.find("nested shape"), std::string::npos) << message;
  expectUntouched();
}

TEST_F(VQLSemanticShapeVmTest, BidirectionalAssignmentRefuses) {
  engine.setVariable("target", arena.makeCell("target"));
  EXPECT_THROW(engine.execute("apply_shape($s, {~d.edge: $target})"),
               std::runtime_error);
  expectUntouched();
}

TEST_F(VQLSemanticShapeVmTest, ShapeHeldInAVariableRefusesByName) {
  const auto message = refusalOf("apply_shape($s, $shape)");
  EXPECT_NE(message.find("literal shape"), std::string::npos) << message;
  expectUntouched();
}

TEST_F(VQLSemanticShapeVmTest, ProjectRefusesByNameWithoutEffects) {
  const auto message = refusalOf("project($s, {+d.keep})");
  EXPECT_NE(message.find("project"), std::string::npos) << message;
  EXPECT_NE(message.find("content spans"), std::string::npos) << message;
  expectUntouched();
}

} // namespace
