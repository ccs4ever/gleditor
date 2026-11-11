#include "common/xanadu/vql/windows.hpp"

#include <algorithm>
#include <utility>

namespace xanadu::vql {
namespace {

WindowError failure(const WindowErrorCode code, std::string message,
                    const std::optional<std::size_t> occurrence = {}) {
  return {
      .code = code, .message = std::move(message), .occurrence = occurrence};
}

} // namespace

std::expected<WindowResult, WindowError>
windowOccurrences(const WindowRequest &request,
                  const std::span<const WindowOccurrence> occurrences) {
  if (request.startInclusive >= request.endExclusive)
    return std::unexpected(failure(WindowErrorCode::InvalidBounds,
                                   "window bounds must be finite and ordered"));
  if (request.maxInputOccurrences == 0 || request.maxKeyBytes == 0 ||
      request.maxOutputOccurrences == 0 || request.maxWork == 0)
    return std::unexpected(failure(WindowErrorCode::InvalidLimits,
                                   "window budgets must be positive"));
  if (request.maxSourceGaps == 0 || request.maxSourceGapBytes == 0)
    return std::unexpected(
        failure(WindowErrorCode::InvalidLimits,
                "window source-gap budgets must be positive"));
  if (request.sourceGaps.size() > request.maxSourceGaps)
    return std::unexpected(failure(WindowErrorCode::SourceGapLimitExceeded,
                                   "window source-gap count limit exceeded"));
  std::size_t sourceGapBytes{};
  for (const auto &gap : request.sourceGaps) {
    if (gap.size() > request.maxSourceGapBytes - sourceGapBytes)
      return std::unexpected(
          failure(WindowErrorCode::SourceGapByteLimitExceeded,
                  "window source-gap byte limit exceeded"));
    sourceGapBytes += gap.size();
  }
  if (occurrences.size() > request.maxInputOccurrences)
    return std::unexpected(failure(WindowErrorCode::InputLimitExceeded,
                                   "window input occurrence limit exceeded"));

  std::size_t keyBytes{};
  for (std::size_t index = 0; index < occurrences.size(); ++index) {
    const auto &key = occurrences[index].key;
    if (key.state != RelationKeyState::Present) continue;
    if (key.canonicalBytes.size() > request.maxKeyBytes - keyBytes)
      return std::unexpected(failure(WindowErrorCode::KeyByteLimitExceeded,
                                     "window key byte limit exceeded", index));
    keyBytes += key.canonicalBytes.size();
  }

  WindowResult result{.inputCoverage = request.inputCoverage,
                      .sourceGaps    = request.sourceGaps,
                      .finalized = request.watermark >= request.endExclusive};
  std::size_t unknownLateness{};
  result.outcomes.reserve(
      std::min(occurrences.size(), request.maxOutputOccurrences));
  for (std::size_t index = 0; index < occurrences.size(); ++index) {
    if (result.work == request.maxWork) {
      result.truncated      = true;
      result.computationGap = "window work budget exhausted";
      break;
    }
    if (result.outcomes.size() == request.maxOutputOccurrences) {
      result.truncated      = true;
      result.computationGap = "window output occurrence budget exhausted";
      break;
    }
    ++result.work;
    const auto &timestamp = occurrences[index].timestamp;
    std::optional<WindowExclusion> exclusion;
    auto lateState = WindowLateState::Unknown;
    if (timestamp.state == WindowTimestampState::Missing) {
      exclusion = WindowExclusion::MissingTimestamp;
    } else if (timestamp.state == WindowTimestampState::Invalid) {
      exclusion = WindowExclusion::InvalidTimestamp;
    } else if (timestamp.milliseconds < request.startInclusive ||
               timestamp.milliseconds >= request.endExclusive) {
      exclusion = WindowExclusion::OutsideBounds;
    } else {
      if (timestamp.watermarkAtArrival) {
        lateState = timestamp.milliseconds < *timestamp.watermarkAtArrival
                        ? WindowLateState::Late
                        : WindowLateState::NotLate;
      } else {
        ++unknownLateness;
      }
      if (lateState == WindowLateState::Late &&
          request.latePolicy == WindowLatePolicy::Reject)
        return std::unexpected(failure(WindowErrorCode::LateDataRejected,
                                       "window rejected late occurrence",
                                       index));
      if (lateState == WindowLateState::Late &&
          request.latePolicy == WindowLatePolicy::Exclude)
        exclusion = WindowExclusion::LateData;
    }
    result.outcomes.push_back({.inputOccurrence = index,
                               .exclusion       = exclusion,
                               .lateState       = lateState});
  }
  if (unknownLateness > 0)
    result.policyGap = "arrival watermark unavailable for " +
                       std::to_string(unknownLateness) + " occurrence(s)";
  return result;
}

std::string_view windowTimeBasisName(const WindowTimeBasis basis) noexcept {
  return basis == WindowTimeBasis::EventTime ? "event-time"
                                             : "observation-time";
}

std::string_view windowLatePolicyName(const WindowLatePolicy policy) noexcept {
  switch (policy) {
  case WindowLatePolicy::Include:
    return "include";
  case WindowLatePolicy::Exclude:
    return "exclude";
  case WindowLatePolicy::Reject:
    return "reject";
  }
  return "invalid";
}

std::string_view windowExclusionName(const WindowExclusion exclusion) noexcept {
  switch (exclusion) {
  case WindowExclusion::OutsideBounds:
    return "outside-bounds";
  case WindowExclusion::MissingTimestamp:
    return "missing-timestamp";
  case WindowExclusion::InvalidTimestamp:
    return "invalid-timestamp";
  case WindowExclusion::LateData:
    return "late-data";
  }
  return "invalid";
}

} // namespace xanadu::vql
