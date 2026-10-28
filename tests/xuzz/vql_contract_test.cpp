/**
 * @file vql_contract_test.cpp
 * @brief Unit coverage for shared VQL structural contract matching.
 */
#include <gtest/gtest.h>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::VortexCore;

class ContractMatcherTest : public ::testing::Test {
protected:
  void SetUp() override {
    core        = std::make_unique<VortexCore>(arena);
    coordinator = std::make_unique<MultiStoreCoordinator>(*core);
    matcher     = std::make_unique<ContractMatcher>(*core, *coordinator);
  }

  static TypeExpr basicType(const BasicType::Kind kind) {
    TypeExpr result;
    result.unionType.members.push_back(
        PrimaryType{.kind = BasicType{.kind = kind}});
    return result;
  }

  std::vector<CellRef> noDefault(const ValueExpr &, std::optional<CellRef>) {
    return {};
  }

  zigzag::ArenaManifold arena;
  std::unique_ptr<VortexCore> core;
  std::unique_ptr<MultiStoreCoordinator> coordinator;
  std::unique_ptr<ContractMatcher> matcher;
};

TEST_F(ContractMatcherTest, UnionFailureDoesNotLeakShapeCaptures) {
  const CellRef root = arena.makeCell();
  const CellRef text = arena.makeCell("present");
  const auto dim     = coordinator->resolveDimension("d.present");
  ASSERT_TRUE(core->setLink(root, dim, DimVector::POS, text));

  DestructurePattern pattern;
  pattern.fields.push_back(
      DestructureField{.dimName = "d.present", .varName = "$captured"});
  pattern.fields.push_back(DestructureField{.dimName = "d.absent"});
  TypeExpr unionType;
  unionType.unionType.members.push_back(
      PrimaryType{.kind = ShapeType{.pattern = std::move(pattern)}});
  unionType.unionType.members.push_back(
      PrimaryType{.kind = BasicType{.kind = BasicType::Kind::Cell}});

  ContractBindings captures;
  const auto evaluateDefault = [this](const ValueExpr &expr,
                                      std::optional<CellRef> context) {
    return noDefault(expr, context);
  };
  ASSERT_TRUE(matcher->satisfiesType(unionType, std::span{&root, 1}, &captures,
                                     evaluateDefault));
  EXPECT_FALSE(captures.contains("$captured"));
}

TEST_F(ContractMatcherTest, UnknownTypesAreRejectedBeforeValueMatching) {
  TypeExpr unknown;
  unknown.unionType.members.push_back(
      PrimaryType{.kind = BasicType{.kind = BasicType::Kind::Contextual,
                                    .name = "Widget"}});

  EXPECT_THROW(matcher->validateType(unknown), TypeContractError);
  const auto evaluateDefault = [this](const ValueExpr &expr,
                                      std::optional<CellRef> context) {
    return noDefault(expr, context);
  };
  EXPECT_THROW(
      (void)matcher->satisfiesType(unknown, {}, nullptr, evaluateDefault),
      TypeContractError);
}

TEST_F(ContractMatcherTest, CaptureNamesAreUniqueAndSortedAcrossUnionArms) {
  TypeExpr nested;
  DestructurePattern first;
  first.fields.push_back(DestructureField{.varName = "$z"});
  first.fields.push_back(DestructureField{.varName = "$a"});
  DestructurePattern second;
  second.fields.push_back(DestructureField{.varName = "$z"});
  nested.unionType.members.push_back(
      PrimaryType{.kind = ShapeType{.pattern = std::move(first)}});
  nested.unionType.members.push_back(
      PrimaryType{.kind = ShapeType{.pattern = std::move(second)}});

  EXPECT_EQ(ContractMatcher::contractCaptureNames(nested),
            (std::vector<std::string>{"$a", "$z"}));
}

TEST_F(ContractMatcherTest, OptionalAndStreamCardinalityAreDistinct) {
  auto integer         = basicType(BasicType::Kind::Int);
  auto optionalInteger = integer;
  optionalInteger.unionType.members.front().isOptional = true;
  TypeExpr streamInteger;
  auto inner = std::make_shared<TypeExpr>(integer);
  streamInteger.unionType.members.push_back(PrimaryType{
      .kind = GenericType{.name = "Stream", .args = {std::move(inner)}}});

  const auto evaluateDefault = [this](const ValueExpr &expr,
                                      std::optional<CellRef> context) {
    return noDefault(expr, context);
  };
  EXPECT_FALSE(matcher->satisfiesType(integer, {}, nullptr, evaluateDefault));
  EXPECT_TRUE(
      matcher->satisfiesType(optionalInteger, {}, nullptr, evaluateDefault));

  const CellRef first  = arena.makeCell("1");
  const CellRef second = arena.makeCell("2");
  const CellRef values[]{first, second};
  EXPECT_FALSE(
      matcher->satisfiesType(integer, values, nullptr, evaluateDefault));
  EXPECT_TRUE(
      matcher->satisfiesType(streamInteger, values, nullptr, evaluateDefault));
}

TEST_F(ContractMatcherTest, NumericTextCanAlsoSatisfyString) {
  const CellRef numericText  = arena.makeCell("95");
  const auto evaluateDefault = [this](const ValueExpr &expr,
                                      std::optional<CellRef> context) {
    return noDefault(expr, context);
  };
  EXPECT_TRUE(matcher->satisfiesType(basicType(BasicType::Kind::Int),
                                     std::span{&numericText, 1}, nullptr,
                                     evaluateDefault));
  EXPECT_TRUE(matcher->satisfiesType(basicType(BasicType::Kind::String),
                                     std::span{&numericText, 1}, nullptr,
                                     evaluateDefault));
}

} // namespace
} // namespace xanadu::vql
