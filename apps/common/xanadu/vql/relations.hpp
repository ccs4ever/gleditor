#ifndef COMMON_XANADU_VQL_RELATIONS_HPP
#define COMMON_XANADU_VQL_RELATIONS_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gleditor/cpp26.hpp>

namespace xanadu::vql {

enum class RelationJoinKind : std::uint8_t { Inner, Left };
// AtMostOne means each left occurrence has at most one matching right row.
enum class RelationCardinality : std::uint8_t { Many, AtMostOne };
enum class RelationNullMatchPolicy : std::uint8_t { Never, Equal };
enum class RelationKeyState : std::uint8_t { Missing, Null, Present };
enum class RelationInputCoverage : std::uint8_t { Complete, Partial, Gapped };
enum class RelationPairState : std::uint8_t {
  Matched,
  Unmatched,
  UnknownCoverage
};

struct RelationKey {
  RelationKeyState state{RelationKeyState::Missing};
  // The caller supplies canonical bytes under the named key policy.
  std::string canonicalBytes;
};

struct RelationJoinLimits {
  std::size_t maxInputRows{};
  std::size_t maxKeyBytes{};
  std::size_t maxOutputPairs{};
  std::size_t maxWork{};
  std::size_t maxPolicyBytes{4096};
};

struct RelationJoinRequest {
  RelationJoinKind kind{RelationJoinKind::Inner};
  RelationCardinality cardinality{RelationCardinality::Many};
  RelationNullMatchPolicy nullMatch{RelationNullMatchPolicy::Never};
  std::string keyPolicy;
  RelationInputCoverage leftCoverage{RelationInputCoverage::Complete};
  RelationInputCoverage rightCoverage{RelationInputCoverage::Complete};
  RelationJoinLimits limits;
};

struct RelationOccurrencePair {
  std::size_t leftOccurrence{};
  std::optional<std::size_t> rightOccurrence;
  RelationPairState state{RelationPairState::Matched};
  bool operator==(const RelationOccurrencePair &) const = default;
};

struct RelationJoinResult {
  std::vector<RelationOccurrencePair> pairs;
  RelationInputCoverage leftCoverage{RelationInputCoverage::Complete};
  RelationInputCoverage rightCoverage{RelationInputCoverage::Complete};
  // Combined source coverage, independent of local computation truncation.
  RelationInputCoverage inputCoverage{RelationInputCoverage::Complete};
  std::string keyPolicy;
  RelationJoinKind kind{RelationJoinKind::Inner};
  RelationCardinality cardinality{RelationCardinality::Many};
  RelationNullMatchPolicy nullMatch{RelationNullMatchPolicy::Never};
  // True only when this result was cut short by its output or work budget.
  // Input coverage is reported independently by inputCoverage and pair states.
  bool truncated{};
  std::optional<std::string> gap;
  std::size_t work{};
};

enum class RelationJoinErrorCode : std::uint8_t {
  InvalidLimits,
  MissingKeyPolicy,
  PolicyByteLimitExceeded,
  InputLimitExceeded,
  KeyByteLimitExceeded,
  InvalidKey,
  CardinalityViolation,
  InvalidPolicy,
  InsufficientCoverage
};

struct RelationJoinError {
  RelationJoinErrorCode code{RelationJoinErrorCode::InvalidLimits};
  std::string message;
  std::optional<std::size_t> leftOccurrence;
  std::optional<std::size_t> rightOccurrence;
};

using RelationKeyAt = gleditor::cpp26::function_ref<RelationKey(std::size_t)>;

// Joins finite input occurrences. Callbacks are borrowed, pure key extractors;
// one key is requested for each input occurrence and callbacks must be bounded.
// Work/output exhaustion returns a retained prefix with truncated=true and a
// gap; source coverage remains independently reported. Malformed inputs,
// input/key budgets, and observed cardinality violations return an error.
[[nodiscard]] std::expected<RelationJoinResult, RelationJoinError>
joinOccurrences(const RelationJoinRequest &request, std::size_t leftSize,
                RelationKeyAt leftKey, std::size_t rightSize,
                RelationKeyAt rightKey);

[[nodiscard]] std::string_view
relationCoverageName(RelationInputCoverage coverage) noexcept;
[[nodiscard]] std::string_view
relationPairStateName(RelationPairState state) noexcept;

} // namespace xanadu::vql
#endif
