#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "common/xanadu/vql/windows.hpp"

namespace xanadu::vql {
namespace {

WindowRequest request() {
  return {.timeBasis            = WindowTimeBasis::EventTime,
          .startInclusive       = 10,
          .endExclusive         = 20,
          .watermark            = 30,
          .latePolicy           = WindowLatePolicy::Exclude,
          .inputCoverage        = RelationInputCoverage::Partial,
          .sourceGaps           = {"page 2 unavailable"},
          .maxSourceGaps        = 4,
          .maxSourceGapBytes    = 100,
          .maxInputOccurrences  = 20,
          .maxKeyBytes          = 100,
          .maxOutputOccurrences = 20,
          .maxWork              = 100};
}

WindowOccurrence at(std::int64_t time,
                    std::optional<std::int64_t> arrivalWatermark = {}) {
  return {
      .timestamp = {.state              = WindowTimestampState::Valid,
                    .milliseconds       = time,
                    .watermarkAtArrival = arrivalWatermark},
      .key = {.state = RelationKeyState::Present, .canonicalBytes = "same"}};
}

TEST(VqlWindowsTest, KeepsDuplicateOccurrencesAndTypedExclusions) {
  auto values = std::vector<WindowOccurrence>{
      at(9, 10),
      at(10, 10),
      at(10, 10),
      at(15, 16),
      at(20, 15),
      {.timestamp = {.state = WindowTimestampState::Missing}},
      {.timestamp = {.state = WindowTimestampState::Invalid}}};
  const auto result = windowOccurrences(request(), values);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->outcomes.size(), values.size());
  EXPECT_EQ(result->outcomes[0].exclusion, WindowExclusion::OutsideBounds);
  EXPECT_EQ(result->outcomes[1],
            (WindowOutcome{1, {}, WindowLateState::NotLate}));
  EXPECT_EQ(result->outcomes[2],
            (WindowOutcome{2, {}, WindowLateState::NotLate}));
  EXPECT_EQ(result->outcomes[3].exclusion, WindowExclusion::LateData);
  EXPECT_EQ(result->outcomes[3].lateState, WindowLateState::Late);
  EXPECT_EQ(result->outcomes[4].exclusion, WindowExclusion::OutsideBounds);
  EXPECT_EQ(result->outcomes[5].exclusion, WindowExclusion::MissingTimestamp);
  EXPECT_EQ(result->outcomes[6].exclusion, WindowExclusion::InvalidTimestamp);
  EXPECT_EQ(result->inputCoverage, RelationInputCoverage::Partial);
  EXPECT_EQ(result->sourceGaps, request().sourceGaps);
  EXPECT_TRUE(result->finalized);
  EXPECT_FALSE(result->truncated);
}

TEST(VqlWindowsTest, MissingArrivalWatermarkDoesNotInventLateness) {
  auto config       = request();
  config.latePolicy = WindowLatePolicy::Reject;
  const std::vector occurrences{at(12)};
  const auto result = windowOccurrences(config, occurrences);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->outcomes.size(), 1U);
  EXPECT_FALSE(result->outcomes.front().exclusion);
  EXPECT_EQ(result->outcomes.front().lateState, WindowLateState::Unknown);
  EXPECT_EQ(result->policyGap,
            "arrival watermark unavailable for 1 occurrence(s)");
}

TEST(VqlWindowsTest, RejectsKnownLateDataAndBoundsResources) {
  auto config       = request();
  config.latePolicy = WindowLatePolicy::Reject;
  const std::vector lateOccurrence{at(12, 13)};
  auto result = windowOccurrences(config, lateOccurrence);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, WindowErrorCode::LateDataRejected);
  EXPECT_EQ(result.error().occurrence, 0U);

  config                     = request();
  config.maxInputOccurrences = 1;
  const std::vector twoOccurrences{at(12), at(13)};
  result = windowOccurrences(config, twoOccurrences);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, WindowErrorCode::InputLimitExceeded);

  config                      = request();
  config.maxOutputOccurrences = 1;
  result                      = windowOccurrences(config, twoOccurrences);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->outcomes.size(), 1U);
  EXPECT_TRUE(result->truncated);
  EXPECT_EQ(result->computationGap,
            "window output occurrence budget exhausted");
  EXPECT_EQ(result->inputCoverage, config.inputCoverage);
}

TEST(VqlWindowsTest, BoundsSourceGapCountAndBytesBeforeCopying) {
  auto config          = request();
  config.maxSourceGaps = 0;
  const std::vector<WindowOccurrence> noOccurrences;
  auto result = windowOccurrences(config, noOccurrences);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, WindowErrorCode::InvalidLimits);

  config = request();
  config.sourceGaps.push_back("another gap");
  config.maxSourceGaps = 1;
  result               = windowOccurrences(config, noOccurrences);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, WindowErrorCode::SourceGapLimitExceeded);

  config                   = request();
  config.maxSourceGapBytes = 4;
  result                   = windowOccurrences(config, noOccurrences);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, WindowErrorCode::SourceGapByteLimitExceeded);
}

} // namespace
} // namespace xanadu::vql
