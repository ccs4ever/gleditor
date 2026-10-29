/**
 * @file vql_contract_test.cpp
 * @brief Unit coverage for shared VQL structural contract matching.
 */
#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

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
  second.fields.push_back(DestructureField{.isContext = true});
  second.fields.push_back(DestructureField{.varName = "$z"});
  nested.unionType.members.push_back(
      PrimaryType{.kind = ShapeType{.pattern = std::move(first)}});
  nested.unionType.members.push_back(
      PrimaryType{.kind = ShapeType{.pattern = std::move(second)}});

  EXPECT_EQ(ContractMatcher::contractCaptureNames(nested),
            (std::vector<std::string>{"$a", "$z", "."}));
}

TEST_F(ContractMatcherTest, ContextFieldChecksTypeBeforeBindingCaptures) {
  const CellRef context = arena.makeCell("context");
  DestructurePattern pattern;
  pattern.fields.push_back(DestructureField{
      .isContext = true,
      .varName   = "$self",
      .type = std::make_shared<TypeExpr>(basicType(BasicType::Kind::String))});
  ContractBindings bindings;
  const auto evaluateDefault = [this](const ValueExpr &expr,
                                      std::optional<CellRef> cell) {
    return noDefault(expr, cell);
  };

  ASSERT_TRUE(matcher->matchDestructure(pattern, context, bindings,
                                        ContractMatcher::MissingField::Refuse,
                                        evaluateDefault));
  EXPECT_EQ(bindings.at("."), (std::vector<CellRef>{context}));
  EXPECT_EQ(bindings.at("$self"), (std::vector<CellRef>{context}));

  const CellRef number = arena.makeScalarCell(std::int64_t{7});
  bindings.clear();
  EXPECT_FALSE(matcher->matchDestructure(pattern, number, bindings,
                                         ContractMatcher::MissingField::Refuse,
                                         evaluateDefault));
  EXPECT_TRUE(bindings.empty());
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

TEST_F(ContractMatcherTest,
       PreboundStreamChecksConsumeFuelPerMemberAndRecoverAfterReset) {
  TypeExpr cellType = basicType(BasicType::Kind::Cell);
  TypeExpr streamType;
  streamType.unionType.members.push_back(PrimaryType{
      .kind = GenericType{
          .name = "Stream",
          .args = {std::make_shared<TypeExpr>(std::move(cellType))}}});

  std::vector<CellRef> prebound;
  prebound.reserve(128);
  for (std::size_t index = 0; index < 128; ++index)
    prebound.push_back(arena.makeCell());

  const auto evaluateDefault = [this](const ValueExpr &expr,
                                      std::optional<CellRef> context) {
    return noDefault(expr, context);
  };
  arena.setExecutionLimits(100'000, 8, 100'000);
  EXPECT_THROW((void)matcher->satisfiesType(streamType, prebound, nullptr,
                                            evaluateDefault),
               std::runtime_error);

  arena.setExecutionLimits(100'000, 512, 100'000);
  EXPECT_TRUE(
      matcher->satisfiesType(streamType, prebound, nullptr, evaluateDefault));
}

TEST_F(ContractMatcherTest,
       DestructureFuelPrecedesDefaultCallbackAndCellMinting) {
  const CellRef root = arena.makeCell();
  DestructurePattern pattern;
  pattern.fields.push_back(DestructureField{
      .dimName      = "d.defaulted",
      .varName      = "$value",
      .defaultValue = std::make_shared<ValueExpr>(
          ValueExpr{.kind = ScalarLiteral{.value = std::string{"fallback"}}})});
  ContractBindings bindings;
  std::size_t defaultCalls   = 0;
  const auto evaluateDefault = [&](const ValueExpr &,
                                   const std::optional<CellRef>) {
    ++defaultCalls;
    return std::vector<CellRef>{arena.makeCell("fallback")};
  };

  const auto cellsBefore = arena.cellCount();
  arena.setExecutionLimits(100'000, 0, 100'000);
  EXPECT_THROW((void)matcher->matchDestructure(
                   pattern, root, bindings,
                   ContractMatcher::MissingField::Refuse, evaluateDefault),
               std::runtime_error);
  EXPECT_EQ(defaultCalls, 0u);
  EXPECT_EQ(arena.cellCount(), cellsBefore);
  EXPECT_TRUE(bindings.empty());

  arena.setExecutionLimits(100'000, 16, 100'000);
  ASSERT_TRUE(matcher->matchDestructure(pattern, root, bindings,
                                        ContractMatcher::MissingField::Refuse,
                                        evaluateDefault));
  EXPECT_EQ(defaultCalls, 1u);
  EXPECT_GT(arena.cellCount(), cellsBefore);
  ASSERT_TRUE(bindings.contains("$value"));
  ASSERT_EQ(bindings.at("$value").size(), 1u);
  EXPECT_EQ(arena.textOf(bindings.at("$value").front()), "fallback");
}

TEST_F(ContractMatcherTest,
       FuelFailureAfterShapeCaptureDoesNotLeakAndCanRecover) {
  const CellRef root = arena.makeCell();
  const CellRef text = arena.makeCell("present");
  const auto present = coordinator->resolveDimension("d.present");
  ASSERT_TRUE(core->setLink(root, present, DimVector::POS, text));
  const CellRef fallback = arena.makeCell("fallback");

  DestructurePattern pattern;
  pattern.fields.push_back(
      DestructureField{.dimName = "d.present", .varName = "$captured"});
  pattern.fields.push_back(DestructureField{
      .dimName      = "d.optional",
      .varName      = "$defaulted",
      .defaultValue = std::make_shared<ValueExpr>(
          ValueExpr{.kind = ScalarLiteral{.value = std::string{"fallback"}}})});
  TypeExpr shape;
  shape.unionType.members.push_back(
      PrimaryType{.kind = ShapeType{.pattern = std::move(pattern)}});
  ContractBindings captures;
  const auto evaluateDefault = [fallback](const ValueExpr &,
                                          const std::optional<CellRef>) {
    return std::vector<CellRef>{fallback};
  };
  const std::span<const CellRef> subject{&root, 1};

  arena.setExecutionLimits(100'000, 2, 100'000);
  EXPECT_THROW(
      (void)matcher->satisfiesType(shape, subject, &captures, evaluateDefault),
      std::runtime_error);
  EXPECT_TRUE(captures.empty());

  arena.setExecutionLimits(100'000, 16, 100'000);
  ASSERT_TRUE(
      matcher->satisfiesType(shape, subject, &captures, evaluateDefault));
  ASSERT_TRUE(captures.contains("$captured"));
  EXPECT_EQ(captures.at("$captured"), (std::vector<CellRef>{text}));
  ASSERT_TRUE(captures.contains("$defaulted"));
  EXPECT_EQ(captures.at("$defaulted"), (std::vector<CellRef>{fallback}));
}

} // namespace
} // namespace xanadu::vql
