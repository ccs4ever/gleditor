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

TEST_F(ContractMatcherTest,
       UnionVisitsFieldsAndArmsInOrderWithoutLeakingFailedCaptures) {
  const CellRef root    = arena.makeCell();
  const CellRef present = arena.makeCell("present");
  const auto presentDim = coordinator->resolveDimension("d.present");
  ASSERT_TRUE(core->setLink(root, presentDim, DimVector::POS, present));

  const auto defaultExpr = [](std::string value) {
    return std::make_shared<ValueExpr>(ValueExpr{.kind = std::move(value)});
  };
  DestructurePattern failedArm;
  failedArm.fields.push_back(
      DestructureField{.dimName = "d.present", .varName = "$discard"});
  failedArm.fields.push_back(DestructureField{.dimName = "d.required"});
  failedArm.fields.push_back(
      DestructureField{.dimName      = "d.too_late",
                       .varName      = "$too_late",
                       .defaultValue = defaultExpr("failed-arm-late")});

  DestructurePattern successfulArm;
  successfulArm.fields.push_back(
      DestructureField{.dimName      = "d.defaulted",
                       .varName      = "$defaulted",
                       .defaultValue = defaultExpr("successful-arm")});
  successfulArm.fields.push_back(
      DestructureField{.dimName = "d.present", .varName = "$kept"});

  DestructurePattern unvisitedArm;
  unvisitedArm.fields.push_back(
      DestructureField{.dimName      = "d.unvisited",
                       .varName      = "$unvisited",
                       .defaultValue = defaultExpr("unvisited-arm")});

  TypeExpr type;
  type.unionType.members.push_back(
      PrimaryType{.kind = ShapeType{.pattern = std::move(failedArm)}});
  type.unionType.members.push_back(
      PrimaryType{.kind = ShapeType{.pattern = std::move(successfulArm)}});
  type.unionType.members.push_back(
      PrimaryType{.kind = ShapeType{.pattern = std::move(unvisitedArm)}});

  const CellRef prior = arena.makeCell("prior");
  ContractBindings bindings{{"$prior", {prior}}};
  std::vector<std::string> defaults;
  const auto evaluateDefault = [&](const ValueExpr &expr,
                                   const std::optional<CellRef> context) {
    EXPECT_EQ(context, root);
    const auto name = std::get<std::string>(expr.kind);
    defaults.push_back(name);
    return std::vector<CellRef>{arena.makeCell(name)};
  };

  ASSERT_TRUE(matcher->satisfiesType(type, std::span{&root, 1}, &bindings,
                                     evaluateDefault));
  EXPECT_EQ(defaults, (std::vector<std::string>{"successful-arm"}));
  EXPECT_EQ(bindings.at("$prior"), (std::vector<CellRef>{prior}));
  EXPECT_FALSE(bindings.contains("$discard"));
  EXPECT_FALSE(bindings.contains("$too_late"));
  EXPECT_FALSE(bindings.contains("$unvisited"));
  EXPECT_EQ(bindings.at("$kept"), (std::vector<CellRef>{present}));
  ASSERT_EQ(bindings.at("$defaulted").size(), 1U);
  EXPECT_EQ(arena.textOf(bindings.at("$defaulted").front()), "successful-arm");
}

TEST_F(ContractMatcherTest,
       DefaultsUseEntryLexicalBindingsNotEarlierPatternCaptures) {
  const CellRef root    = arena.makeCell();
  const CellRef inner   = arena.makeCell("inner");
  const CellRef outer   = arena.makeCell("outer");
  const auto captureDim = coordinator->resolveDimension("d.capture");
  ASSERT_TRUE(core->setLink(root, captureDim, DimVector::POS, inner));

  DestructurePattern pattern;
  pattern.fields.push_back(
      DestructureField{.dimName = "d.capture", .varName = "$alias"});
  pattern.fields.push_back(
      DestructureField{.dimName      = "d.defaulted",
                       .varName      = "$defaulted",
                       .defaultValue = std::make_shared<ValueExpr>(
                           ValueExpr{.kind = std::string{"$alias"}})});
  ContractBindings lexical{{"$alias", {outer}}};
  ContractBindings captures;
  const auto evaluateDefault = [&](const ValueExpr &expr,
                                   const std::optional<CellRef> context) {
    EXPECT_EQ(context, root);
    return lexical.at(std::get<std::string>(expr.kind));
  };

  ASSERT_TRUE(matcher->matchDestructure(pattern, root, captures,
                                        ContractMatcher::MissingField::Refuse,
                                        evaluateDefault));
  EXPECT_EQ(captures.at("$alias"), (std::vector<CellRef>{inner}));
  EXPECT_EQ(captures.at("$defaulted"), (std::vector<CellRef>{outer}));
}

TEST_F(ContractMatcherTest,
       BidirectionalFieldPrefersForwardThenReverseAndOnlyThenDefaults) {
  const auto dimension  = coordinator->resolveDimension("d.choice");
  const CellRef root    = arena.makeCell();
  const CellRef forward = arena.makeCell("forward");
  const CellRef reverse = arena.makeCell("reverse");
  ASSERT_TRUE(core->setLink(root, dimension, DimVector::POS, forward));
  ASSERT_TRUE(core->setLink(root, dimension, DimVector::NEG, reverse));

  DestructurePattern pattern;
  pattern.fields.push_back(
      DestructureField{.dimName      = "d.choice",
                       .direction    = DimVector::POS,
                       .isBidir      = true,
                       .varName      = "$choice",
                       .defaultValue = std::make_shared<ValueExpr>(
                           ValueExpr{.kind = std::string{"fallback"}})});
  std::size_t defaultCalls   = 0;
  const auto evaluateDefault = [&](const ValueExpr &,
                                   const std::optional<CellRef>) {
    ++defaultCalls;
    return std::vector<CellRef>{arena.makeCell("fallback")};
  };
  ContractBindings captures;
  ASSERT_TRUE(matcher->matchDestructure(pattern, root, captures,
                                        ContractMatcher::MissingField::Refuse,
                                        evaluateDefault));
  EXPECT_EQ(captures.at("$choice"), (std::vector<CellRef>{forward}));
  EXPECT_EQ(defaultCalls, 0U);

  const CellRef reverseRoot = arena.makeCell();
  ASSERT_TRUE(core->setLink(reverseRoot, dimension, DimVector::NEG, reverse));
  captures.clear();
  ASSERT_TRUE(matcher->matchDestructure(pattern, reverseRoot, captures,
                                        ContractMatcher::MissingField::Refuse,
                                        evaluateDefault));
  EXPECT_EQ(captures.at("$choice"), (std::vector<CellRef>{reverse}));
  EXPECT_EQ(defaultCalls, 0U);

  const CellRef absentRoot = arena.makeCell();
  captures.clear();
  ASSERT_TRUE(matcher->matchDestructure(pattern, absentRoot, captures,
                                        ContractMatcher::MissingField::Refuse,
                                        evaluateDefault));
  EXPECT_EQ(defaultCalls, 1U);
  EXPECT_EQ(arena.textOf(captures.at("$choice").front()), "fallback");

  // Presence selects the forward field even if its type then fails.
  const CellRef typedRoot    = arena.makeCell();
  const CellRef wrongForward = arena.makeCell("not an integer");
  const CellRef validReverse = arena.makeScalarCell(std::int64_t{9});
  ASSERT_TRUE(
      core->setLink(typedRoot, dimension, DimVector::POS, wrongForward));
  ASSERT_TRUE(
      core->setLink(typedRoot, dimension, DimVector::NEG, validReverse));
  pattern.fields.front().type =
      std::make_shared<TypeExpr>(basicType(BasicType::Kind::Int));
  const auto prior = captures;
  EXPECT_FALSE(matcher->matchDestructure(pattern, typedRoot, captures,
                                         ContractMatcher::MissingField::Refuse,
                                         evaluateDefault));
  EXPECT_EQ(captures, prior);
  EXPECT_EQ(defaultCalls, 1U);
}

TEST_F(ContractMatcherTest,
       GenericElementAndRefinementCapturesAreIgnoredAndSuccessRollsBack) {
  const auto elementDim     = coordinator->resolveDimension("d.element");
  const CellRef first       = arena.makeCell("first");
  const CellRef second      = arena.makeCell("second");
  const CellRef firstValue  = arena.makeCell("one");
  const CellRef secondValue = arena.makeCell("two");
  ASSERT_TRUE(core->setLink(first, elementDim, DimVector::POS, firstValue));
  ASSERT_TRUE(core->setLink(second, elementDim, DimVector::POS, secondValue));

  DestructurePattern elementPattern;
  elementPattern.fields.push_back(
      DestructureField{.dimName = "d.element", .varName = "$element"});
  elementPattern.fields.push_back(
      DestructureField{.dimName      = "d.element_default",
                       .varName      = "$element_default",
                       .defaultValue = std::make_shared<ValueExpr>(
                           ValueExpr{.kind = std::string{"element-default"}})});
  TypeExpr elementType;
  elementType.unionType.members.push_back(
      PrimaryType{.kind = ShapeType{.pattern = std::move(elementPattern)}});

  DestructurePattern refinement;
  refinement.fields.push_back(
      DestructureField{.dimName      = "d.refinement_default",
                       .varName      = "$refinement",
                       .defaultValue = std::make_shared<ValueExpr>(ValueExpr{
                           .kind = std::string{"refinement-default"}})});
  TypeExpr streamType;
  streamType.unionType.members.push_back(
      PrimaryType{.kind       = GenericType{.name = "Stream",
                                            .args = {std::make_shared<TypeExpr>(
                                          std::move(elementType))}},
                  .refinement = ShapeType{.pattern = std::move(refinement)}});

  const CellRef prior = arena.makeCell("prior");
  ContractBindings captures{{"$prior", {prior}}};
  std::vector<CellRef> defaultContexts;
  const auto evaluateDefault = [&](const ValueExpr &,
                                   const std::optional<CellRef> context) {
    EXPECT_TRUE(context.has_value());
    defaultContexts.push_back(*context);
    return std::vector<CellRef>{arena.makeCell("temporary default")};
  };
  const CellRef subjects[]{first, second};
  const auto cellsBefore = arena.cellCount();

  ASSERT_TRUE(
      matcher->satisfiesType(streamType, subjects, &captures, evaluateDefault));
  EXPECT_EQ(defaultContexts,
            (std::vector<CellRef>{first, first, second, second}));
  EXPECT_EQ(arena.cellCount(), cellsBefore);
  EXPECT_EQ(captures.size(), 1U);
  EXPECT_EQ(captures.at("$prior"), (std::vector<CellRef>{prior}));
}

TEST_F(ContractMatcherTest, ShapeCapturesPrecedeRefinementCaptures) {
  const CellRef root        = arena.makeCell();
  const CellRef shapeValue  = arena.makeCell("shape value");
  const CellRef refineValue = arena.makeCell("refinement value");
  const auto shapeDim       = coordinator->resolveDimension("d.shape");
  const auto refinementDim  = coordinator->resolveDimension("d.refinement");
  ASSERT_TRUE(core->setLink(root, shapeDim, DimVector::POS, shapeValue));
  ASSERT_TRUE(core->setLink(root, refinementDim, DimVector::POS, refineValue));

  DestructurePattern shape;
  shape.fields.push_back(
      DestructureField{.dimName = "d.shape", .varName = "$same"});
  DestructurePattern refinement;
  refinement.fields.push_back(
      DestructureField{.dimName = "d.refinement", .varName = "$same"});
  TypeExpr type;
  type.unionType.members.push_back(
      PrimaryType{.kind       = ShapeType{.pattern = std::move(shape)},
                  .refinement = ShapeType{.pattern = std::move(refinement)}});

  ContractBindings captures;
  const auto evaluateDefault = [this](const ValueExpr &expr,
                                      const std::optional<CellRef> context) {
    return noDefault(expr, context);
  };
  ASSERT_TRUE(matcher->satisfiesType(type, std::span{&root, 1}, &captures,
                                     evaluateDefault));
  EXPECT_EQ(captures.at("$same"), (std::vector<CellRef>{refineValue}));
}

TEST_F(ContractMatcherTest,
       DefaultFailurePublishesNoCapturesAndReleasesTemporaryCells) {
  const CellRef root    = arena.makeCell();
  const CellRef present = arena.makeCell("present");
  const CellRef prior   = arena.makeCell("prior");
  const auto presentDim = coordinator->resolveDimension("d.present");
  ASSERT_TRUE(core->setLink(root, presentDim, DimVector::POS, present));

  DestructurePattern pattern;
  pattern.fields.push_back(
      DestructureField{.dimName = "d.present", .varName = "$new"});
  pattern.fields.push_back(
      DestructureField{.dimName      = "d.failing_default",
                       .varName      = "$partial",
                       .defaultValue = std::make_shared<ValueExpr>(
                           ValueExpr{.kind = std::string{"throw"}})});
  ContractBindings captures{{"$prior", {prior}}};
  const auto cellsBefore = arena.cellCount();
  const auto evaluateDefault =
      [&](const ValueExpr &,
          const std::optional<CellRef>) -> std::vector<CellRef> {
    (void)arena.makeCell("temporary");
    throw TypeContractError("default evaluation refused");
  };

  EXPECT_THROW((void)matcher->matchDestructure(
                   pattern, root, captures,
                   ContractMatcher::MissingField::Refuse, evaluateDefault),
               TypeContractError);
  EXPECT_EQ(captures.size(), 1U);
  EXPECT_EQ(captures.at("$prior"), (std::vector<CellRef>{prior}));
  EXPECT_EQ(arena.cellCount(), cellsBefore);
  EXPECT_EQ(arena.outstandingMarks(), 0U);
}

TEST_F(ContractMatcherTest, NestedValidationRollsBackBeforeOuterRefinement) {
  const CellRef root = arena.makeCell();
  (void)coordinator->resolveDimension("d.inner_default");
  (void)coordinator->resolveDimension("d.refinement_default");
  const auto field = [](const std::string &dimension, const std::string &name) {
    return DestructureField{
        .dimName      = dimension,
        .varName      = name,
        .defaultValue = std::make_shared<ValueExpr>(ValueExpr{
            .kind = ScalarLiteral{.value = std::string{"fallback"}}})};
  };
  TypeExpr inner;
  inner.unionType.members.push_back(PrimaryType{
      .kind = ShapeType{.pattern = DestructurePattern{
                            .fields = {field("d.inner_default", "$inner")}}}});
  TypeExpr outer;
  outer.unionType.members.push_back(PrimaryType{
      .kind       = std::make_shared<TypeExpr>(inner),
      .refinement = ShapeType{
          .pattern = DestructurePattern{
              .fields = {field("d.refinement_default", "$refined")}}}});

  for (const bool capture : {false, true}) {
    ContractBindings bindings;
    std::optional<CellRef> temporary;
    std::size_t calls          = 0;
    const auto evaluateDefault = [&](const ValueExpr &,
                                     const std::optional<CellRef> owner) {
      EXPECT_EQ(owner, root);
      if (calls++ == 0) {
        temporary = arena.makeCell("inner temporary");
        return std::vector<CellRef>{*temporary};
      }
      EXPECT_EQ(arena.contains(*temporary), capture);
      return std::vector<CellRef>{arena.makeCell("refinement temporary")};
    };
    const auto before = arena.cellCount();
    ASSERT_TRUE(matcher->satisfiesType(outer, std::span{&root, 1},
                                       capture ? &bindings : nullptr,
                                       evaluateDefault));
    EXPECT_EQ(calls, 2u);
    EXPECT_EQ(arena.outstandingMarks(), 0u);
    if (capture) {
      EXPECT_EQ(bindings.at("$inner"), (std::vector<CellRef>{*temporary}));
      EXPECT_EQ(bindings.at("$refined").size(), 1u);
    } else {
      EXPECT_EQ(arena.cellCount(), before);
      EXPECT_TRUE(bindings.empty());
    }
  }
}

TEST_F(ContractMatcherTest, FlatPatternsDoNotRequirePerFieldStackFrames) {
  const auto root      = arena.makeCell();
  const auto value     = arena.makeCell("present");
  const auto dimension = coordinator->resolveDimension("d.flat");
  ASSERT_TRUE(core->setLink(root, dimension, DimVector::POS, value));
  DestructurePattern pattern;
  pattern.fields.resize(8192, DestructureField{.dimName = "d.flat"});
  ContractBindings captures;
  const auto before          = arena.cellCount();
  const auto evaluateDefault = [&](const ValueExpr &, std::optional<CellRef>) {
    ADD_FAILURE() << "present fields must skip defaults";
    return std::vector<CellRef>{};
  };
  ASSERT_TRUE(matcher->matchDestructure(pattern, root, captures,
                                        ContractMatcher::MissingField::Refuse,
                                        evaluateDefault));
  EXPECT_TRUE(captures.empty());
  EXPECT_EQ(arena.cellCount(), before);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

} // namespace
} // namespace xanadu::vql
