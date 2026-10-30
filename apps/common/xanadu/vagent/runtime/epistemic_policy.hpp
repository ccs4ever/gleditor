/**
 * @file epistemic_policy.hpp
 * @brief The typed `arbitrated by` policy of a predicate (P19.4, I3-1a).
 *
 * The checker turns an `ArbitrationClause` into an `ArbitrationPolicy` through
 * one closed key table, so every key has one literal type and one domain. The
 * policy holds checked constants only; nothing here evaluates a model.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_POLICY_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_POLICY_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/runtime/ast.hpp"

namespace xanadu::vagent::runtime {

enum class ArbitrationStrategy : std::uint8_t { Truest, Ranked, Consensus };

/// The acquisition time basis of a predicate's observations.
enum class ObservationClock : std::uint8_t {
  Wall,
  /// Seconds on the executor's steady timeline, mapped by the host.
  ActorMapped,
};

/// The only model I3-1 admits; others arrive with their own review.
inline constexpr std::string_view kBetaReputationModel = "beta_reputation";

// Bounds that keep I4's reciprocal and precision sums finite for any admitted
// variance, with headroom for inflation (P19 section 8.1).
inline constexpr double kMinAdmissibleVariance = 1e-150;
inline constexpr double kMaxAdmissibleVariance = 1e150;
/// P19 section 5.3's W-space indifference default; a policy that omits
/// `score_tolerance` resolves to it, so the hash records the value in force.
inline constexpr double kDefaultScoreTolerance = 1e-9;
// Beta pseudo-counts beyond this would let I4's count sums lose integer
// precision long before any evidence could move them.
inline constexpr double kMaxPriorCount = 1e12;
// About 31 years: far beyond any freshness horizon, and small enough that
// I4's age arithmetic in milliseconds cannot overflow Int64.
inline constexpr std::int64_t kMaxPolicyDurationMs = 1'000'000'000'000;

struct ArbitrationPolicy {
  ArbitrationStrategy strategy{ArbitrationStrategy::Truest};
  std::string model;
  double minConfidence{};
  double priorAlpha{};
  double priorBeta{};
  ObservationClock clock{ObservationClock::Wall};
  std::int64_t maxAgeMs{};
  std::int64_t clockSkewMs{};
  /// `max_uncertainty`, resolved to max_age when absent, so the hash records
  /// the bound in force.
  std::int64_t maxUncertaintyMs{};
  std::optional<std::int64_t> decayMs;
  std::optional<std::int64_t> alignmentMs;
  double scoreTolerance{kDefaultScoreTolerance};
  /// Whether truest/ranked may expose UNTESTED cold-start hypotheses (P19
  /// section 5.3); consensus never may.
  bool allowUntested{false};
  /// A non-key Float parameter makes the predicate continuous; the fields
  /// below are then required (the first five) or optional.
  bool continuous{false};
  std::optional<double> clusterTolerance;
  std::optional<std::string> frame;
  std::optional<std::string> units;
  std::optional<double> minVariance;
  std::optional<double> maxVariance;
  std::optional<double> maxClusterDiameter;
  std::optional<double> maxSlew;
  std::optional<double> innovation;
  bool operator==(const ArbitrationPolicy &) const = default;
};

enum class PolicyIssueCode : std::uint8_t {
  InvalidEpistemicPolicy,
  MissingClusterTolerance,
};

struct PolicyIssue {
  PolicyIssueCode code{PolicyIssueCode::InvalidEpistemicPolicy};
  std::string message;
  SourceLocation loc{};
};

/// Check @p clause for a predicate that is @p continuous. Every problem is
/// reported, not only the first.
[[nodiscard]] std::expected<ArbitrationPolicy, std::vector<PolicyIssue>>
checkArbitrationPolicy(const ArbitrationClause &clause, bool continuous);

[[nodiscard]] std::string_view strategyName(ArbitrationStrategy s) noexcept;
[[nodiscard]] std::string_view clockName(ObservationClock c) noexcept;

/// Canonical, versioned encoding of every field; equal policies encode
/// equally and any changed field changes it.
[[nodiscard]] std::string canonicalPolicy(const ArbitrationPolicy &policy);
/// SHA-256 hex of `canonicalPolicy`, the identity upgrade and restore compare.
[[nodiscard]] std::string policyHash(const ArbitrationPolicy &policy);

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_POLICY_HPP
