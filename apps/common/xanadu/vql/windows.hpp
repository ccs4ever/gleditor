#ifndef COMMON_XANADU_VQL_WINDOWS_HPP
#define COMMON_XANADU_VQL_WINDOWS_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vql/relations.hpp"

namespace xanadu::vql {

enum class WindowTimeBasis : std::uint8_t { EventTime, ObservationTime };
enum class WindowLatePolicy : std::uint8_t { Include, Exclude, Reject };
enum class WindowTimestampState : std::uint8_t { Valid, Missing, Invalid };
enum class WindowLateState : std::uint8_t { NotLate, Late, Unknown };
enum class WindowExclusion : std::uint8_t {
  OutsideBounds,
  MissingTimestamp,
  InvalidTimestamp,
  LateData
};

struct WindowTimestamp {
  WindowTimestampState state{WindowTimestampState::Missing};
  std::int64_t milliseconds{};
  std::optional<std::int64_t> watermarkAtArrival;
  bool operator==(const WindowTimestamp &) const = default;
};

struct WindowOccurrence {
  WindowTimestamp timestamp;
  RelationKey key;
  bool operator==(const WindowOccurrence &other) const {
    return timestamp == other.timestamp && key.state == other.key.state &&
           key.canonicalBytes == other.key.canonicalBytes;
  }
};

struct WindowRequest {
  WindowTimeBasis timeBasis{WindowTimeBasis::EventTime};
  std::int64_t startInclusive{};
  std::int64_t endExclusive{};
  std::int64_t watermark{};
  WindowLatePolicy latePolicy{WindowLatePolicy::Exclude};
  RelationInputCoverage inputCoverage{RelationInputCoverage::Complete};
  std::vector<std::string> sourceGaps;
  std::size_t maxSourceGaps{};
  std::size_t maxSourceGapBytes{};
  std::size_t maxInputOccurrences{};
  std::size_t maxKeyBytes{};
  std::size_t maxOutputOccurrences{};
  std::size_t maxWork{};
};

struct WindowOutcome {
  std::size_t inputOccurrence{};
  std::optional<WindowExclusion> exclusion;
  WindowLateState lateState{WindowLateState::Unknown};
  bool operator==(const WindowOutcome &) const = default;
};

struct WindowResult {
  std::vector<WindowOutcome> outcomes;
  RelationInputCoverage inputCoverage{RelationInputCoverage::Complete};
  std::vector<std::string> sourceGaps;
  bool finalized{};
  std::optional<std::string> policyGap;
  bool truncated{};
  std::optional<std::string> computationGap;
  std::size_t work{};
};

enum class WindowErrorCode : std::uint8_t {
  InvalidBounds,
  InvalidLimits,
  SourceGapLimitExceeded,
  SourceGapByteLimitExceeded,
  InputLimitExceeded,
  KeyByteLimitExceeded,
  LateDataRejected
};

struct WindowError {
  WindowErrorCode code{WindowErrorCode::InvalidLimits};
  std::string message;
  std::optional<std::size_t> occurrence;
};

[[nodiscard]] std::expected<WindowResult, WindowError>
windowOccurrences(const WindowRequest &request,
                  std::span<const WindowOccurrence> occurrences);

[[nodiscard]] std::string_view
windowTimeBasisName(WindowTimeBasis basis) noexcept;
[[nodiscard]] std::string_view
windowLatePolicyName(WindowLatePolicy policy) noexcept;
[[nodiscard]] std::string_view
windowExclusionName(WindowExclusion exclusion) noexcept;

} // namespace xanadu::vql
#endif
