#include "common/xanadu/vql/relations.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace xanadu::vql {
namespace {

RelationKey present(std::string bytes) {
  return {.state          = RelationKeyState::Present,
          .canonicalBytes = std::move(bytes)};
}

RelationKey nullKey() {
  return {.state = RelationKeyState::Null, .canonicalBytes = {}};
}

RelationKey missingKey() {
  return {.state = RelationKeyState::Missing, .canonicalBytes = {}};
}

RelationJoinRequest request() {
  return {.kind          = RelationJoinKind::Left,
          .cardinality   = RelationCardinality::Many,
          .nullMatch     = RelationNullMatchPolicy::Never,
          .keyPolicy     = "case-sensitive-utf8-v1",
          .leftCoverage  = RelationInputCoverage::Complete,
          .rightCoverage = RelationInputCoverage::Complete,
          .limits        = {.maxInputRows   = 100,
                            .maxKeyBytes    = 1000,
                            .maxOutputPairs = 100,
                            .maxWork        = 1000}};
}

std::expected<RelationJoinResult, RelationJoinError>
run(const RelationJoinRequest &config, const std::vector<RelationKey> &left,
    const std::vector<RelationKey> &right) {
  const auto leftAt  = [&](const std::size_t index) { return left[index]; };
  const auto rightAt = [&](const std::size_t index) { return right[index]; };
  return joinOccurrences(config, left.size(), leftAt, right.size(), rightAt);
}

TEST(VqlRelationsTest, PreservesDuplicateOccurrencesInStableInputOrder) {
  const auto result = run(request(), {present("a"), present("a"), present("b")},
                          {present("a"), present("a"), present("b")});
  ASSERT_TRUE(result);
  EXPECT_EQ(result->pairs, (std::vector<RelationOccurrencePair>{
                               {0, 0, RelationPairState::Matched},
                               {0, 1, RelationPairState::Matched},
                               {1, 0, RelationPairState::Matched},
                               {1, 1, RelationPairState::Matched},
                               {2, 2, RelationPairState::Matched}}));
  EXPECT_FALSE(result->truncated);
  EXPECT_EQ(result->inputCoverage, RelationInputCoverage::Complete);
}

TEST(VqlRelationsTest, InnerJoinOmitsLeftOccurrencesWithoutMatches) {
  auto config = request();
  config.kind = RelationJoinKind::Inner;
  const auto result =
      run(config, {present("hit"), present("miss")}, {present("hit")});
  ASSERT_TRUE(result);
  EXPECT_EQ(result->pairs, (std::vector<RelationOccurrencePair>{
                               {0, 0, RelationPairState::Matched}}));
}

TEST(VqlRelationsTest, MissingNeverMatchesAndNullMatchingIsAnExplicitPolicy) {
  const std::vector<RelationKey> left{nullKey(), missingKey(), present("")};
  const std::vector<RelationKey> right{nullKey(), missingKey(), present("")};
  auto config = request();

  auto result = run(config, left, right);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->pairs, (std::vector<RelationOccurrencePair>{
                               {0, {}, RelationPairState::Unmatched},
                               {1, {}, RelationPairState::Unmatched},
                               {2, 2, RelationPairState::Matched}}));

  config.nullMatch = RelationNullMatchPolicy::Equal;
  result           = run(config, left, right);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->pairs[0],
            (RelationOccurrencePair{0, 0, RelationPairState::Matched}));
  EXPECT_EQ(result->pairs[1],
            (RelationOccurrencePair{1, {}, RelationPairState::Unmatched}));
}

TEST(VqlRelationsTest,
     IncompleteRightCoverageMakesLeftNoMatchUnknownRatherThanAbsent) {
  auto config          = request();
  config.rightCoverage = RelationInputCoverage::Gapped;
  const auto result = run(config, {present("not-found")}, {present("other")});
  ASSERT_TRUE(result);
  ASSERT_EQ(result->pairs.size(), 1U);
  EXPECT_EQ(result->pairs[0].state, RelationPairState::UnknownCoverage);
  EXPECT_EQ(relationPairStateName(result->pairs[0].state),
            "unknown-within-coverage");
  EXPECT_EQ(result->inputCoverage, RelationInputCoverage::Gapped);
  EXPECT_FALSE(result->truncated);

  config.rightCoverage = RelationInputCoverage::Partial;
  const auto partial = run(config, {present("not-found")}, {present("other")});
  ASSERT_TRUE(partial);
  ASSERT_EQ(partial->pairs.size(), 1U);
  EXPECT_EQ(partial->pairs[0].state, RelationPairState::UnknownCoverage);
  EXPECT_EQ(partial->inputCoverage, RelationInputCoverage::Partial);
}

TEST(VqlRelationsTest, AtMostOneRefusesAnObservedDuplicateRightKey) {
  auto config        = request();
  config.cardinality = RelationCardinality::AtMostOne;
  const auto result =
      run(config, {present("same")}, {present("same"), present("same")});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, RelationJoinErrorCode::CardinalityViolation);
  EXPECT_EQ(result.error().leftOccurrence, 0U);
  EXPECT_EQ(result.error().rightOccurrence, 0U);
}

TEST(VqlRelationsTest, AtMostOneRequiresCompleteRightCoverage) {
  auto config          = request();
  config.cardinality   = RelationCardinality::AtMostOne;
  config.rightCoverage = RelationInputCoverage::Partial;
  std::size_t rightCalls{};
  const auto leftAt  = [&](const std::size_t) { return present("key"); };
  const auto rightAt = [&](const std::size_t) {
    ++rightCalls;
    return present("key");
  };
  auto result = joinOccurrences(config, 1, leftAt, 1, rightAt);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, RelationJoinErrorCode::InsufficientCoverage);
  EXPECT_EQ(rightCalls, 0U);

  config.rightCoverage = RelationInputCoverage::Gapped;
  result               = joinOccurrences(config, 1, leftAt, 1, rightAt);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, RelationJoinErrorCode::InsufficientCoverage);
  EXPECT_EQ(rightCalls, 0U);
}

TEST(VqlRelationsTest, InputAndKeyByteLimitsReturnTypedErrors) {
  auto config                = request();
  config.limits.maxInputRows = 2;
  std::size_t leftCalls{};
  std::size_t rightCalls{};
  const auto leftAt = [&](const std::size_t index) {
    ++leftCalls;
    return present(index == 0 ? "a" : "b");
  };
  const auto rightAt = [&](const std::size_t) {
    ++rightCalls;
    return present("c");
  };
  auto result = joinOccurrences(config, 2, leftAt, 1, rightAt);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, RelationJoinErrorCode::InputLimitExceeded);
  EXPECT_EQ(leftCalls, 0U);
  EXPECT_EQ(rightCalls, 0U);

  config                    = request();
  config.limits.maxKeyBytes = 2;
  result                    = run(config, {present("ab")}, {present("c")});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, RelationJoinErrorCode::KeyByteLimitExceeded);
}

TEST(VqlRelationsTest, KeyPolicyMetadataHasItsOwnIndependentByteLimit) {
  auto config                  = request();
  config.limits.maxKeyBytes    = 2;
  config.limits.maxPolicyBytes = 3;
  config.keyPolicy             = "four";
  auto result                  = run(config, {present("a")}, {present("b")});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code,
            RelationJoinErrorCode::PolicyByteLimitExceeded);

  config.keyPolicy = "v1";
  result           = run(config, {present("a")}, {present("b")});
  EXPECT_TRUE(result);
}

TEST(VqlRelationsTest, OutputAndWorkLimitsReturnExplicitPartialPrefixes) {
  auto config                  = request();
  config.limits.maxOutputPairs = 2;
  auto result =
      run(config, {present("x")}, {present("x"), present("x"), present("x")});
  ASSERT_TRUE(result);
  ASSERT_EQ(result->pairs.size(), 2U);
  EXPECT_TRUE(result->truncated);
  EXPECT_EQ(result->gap, "output pair limit reached");
  EXPECT_EQ(result->inputCoverage, RelationInputCoverage::Complete);

  config                = request();
  config.limits.maxWork = 4;
  result = run(config, {present("x"), present("x")}, {present("x")});
  ASSERT_TRUE(result);
  ASSERT_EQ(result->pairs.size(), 1U);
  EXPECT_TRUE(result->truncated);
  EXPECT_EQ(result->gap, "work limit reached while emitting join pairs");
}

TEST(VqlRelationsTest, InvalidLimitsAndUnnamedKeyPolicyAreRejected) {
  auto config           = request();
  config.limits.maxWork = 0;
  auto result           = run(config, {}, {});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, RelationJoinErrorCode::InvalidLimits);

  config           = request();
  config.keyPolicy = "";
  result           = run(config, {}, {});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, RelationJoinErrorCode::MissingKeyPolicy);

  config      = request();
  config.kind = static_cast<RelationJoinKind>(255);
  result      = run(config, {}, {});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, RelationJoinErrorCode::InvalidPolicy);
}

} // namespace
} // namespace xanadu::vql
