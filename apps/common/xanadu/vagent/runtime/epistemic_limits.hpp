/**
 * @file epistemic_limits.hpp
 * @brief Shared P19 resource defaults and checked host configuration.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_LIMITS_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_LIMITS_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

namespace xanadu::vagent::runtime {

inline constexpr std::int64_t kDefaultEpistemicMaxSources    = 128;
inline constexpr std::int64_t kDefaultEpistemicMaxCandidates = 1024;
// A query frame nests record, list and field levels; 32 leaves room for a
// candidate list of scored records inside a result without admitting the
// unbounded recursion a hostile descriptor could request.
inline constexpr std::int64_t kDefaultEpistemicScopeDepth = 32;
// A query may call a ranked sub-query inside its callback; eight levels bound
// the saved caller continuations an actor can hold at once.
inline constexpr std::int64_t kDefaultEpistemicNestedScopes = 8;
// Checked ingestion (I3-1 design section 9). A record costs about 100 cells,
// so these defaults hold about 18 fully active principals in the cell budget.
inline constexpr std::int64_t kDefaultEpistemicHistory               = 4;
inline constexpr std::int64_t kDefaultEpistemicDedup                 = 16;
inline constexpr std::int64_t kDefaultEpistemicKeysPerSource         = 4;
inline constexpr std::int64_t kDefaultEpistemicIngressQueue          = 64;
inline constexpr std::int64_t kDefaultEpistemicIngressQueuePerSource = 8;
inline constexpr std::int64_t kDefaultEpistemicOutcomeLog            = 256;
inline constexpr std::int64_t kDefaultEpistemicCells                 = 32768;

enum class EpistemicLimitError : std::uint8_t {
  InvalidSources,
  InvalidCandidates,
  InvalidScopeDepth,
  InvalidNestedScopes,
  InvalidIngestionBound,
};

/// Logical admission bounds; actor cell, byte and fuel grants remain
/// independent.
struct EpistemicLimits {
  std::size_t maxSources{static_cast<std::size_t>(kDefaultEpistemicMaxSources)};
  std::size_t maxCandidates{
      static_cast<std::size_t>(kDefaultEpistemicMaxCandidates)};
  /// Schema and value nesting a scoped query may admit.
  std::size_t maxScopeDepth{
      static_cast<std::size_t>(kDefaultEpistemicScopeDepth)};
  /// Scoped evaluations one actor may have open at once, nesting included.
  std::size_t maxNestedScopes{
      static_cast<std::size_t>(kDefaultEpistemicNestedScopes)};
  /// `vagent.epistemic_history`: records retained per (principal, key) support;
  /// current and pinned records are never excised.
  std::size_t history{static_cast<std::size_t>(kDefaultEpistemicHistory)};
  /// `vagent.epistemic_dedup`: (incarnation, event ID, digest) entries per
  /// source profile; the replay horizon for exact retries.
  std::size_t dedup{static_cast<std::size_t>(kDefaultEpistemicDedup)};
  /// `vagent.epistemic_keys_per_source`: keys one source principal may support
  /// on one predicate.
  std::size_t keysPerSource{
      static_cast<std::size_t>(kDefaultEpistemicKeysPerSource)};
  /// `vagent.epistemic_ingress_queue`: checked observations queued for one
  /// actor.
  std::size_t ingressQueue{
      static_cast<std::size_t>(kDefaultEpistemicIngressQueue)};
  /// `vagent.epistemic_ingress_queue_per_source`: queued observations one
  /// source principal may hold.
  std::size_t ingressQueuePerSource{
      static_cast<std::size_t>(kDefaultEpistemicIngressQueuePerSource)};
  /// `vagent.epistemic_outcome_log`: ingestion outcomes the host log keeps per
  /// actor.
  std::size_t outcomeLog{static_cast<std::size_t>(kDefaultEpistemicOutcomeLog)};
  /// `vagent.epistemic_cells`: maintained epistemic cell ceiling, kept below
  /// vagent.value_operations so a full budget still checkpoints.
  std::size_t cells{static_cast<std::size_t>(kDefaultEpistemicCells)};

  [[nodiscard]] std::expected<void, EpistemicLimitError> validate() const {
    if (maxSources == 0)
      return std::unexpected(EpistemicLimitError::InvalidSources);
    if (maxCandidates == 0)
      return std::unexpected(EpistemicLimitError::InvalidCandidates);
    if (maxScopeDepth == 0)
      return std::unexpected(EpistemicLimitError::InvalidScopeDepth);
    if (maxNestedScopes == 0)
      return std::unexpected(EpistemicLimitError::InvalidNestedScopes);
    for (const auto bound : {history, dedup, keysPerSource, ingressQueue,
                             ingressQueuePerSource, outcomeLog, cells})
      if (bound == 0)
        return std::unexpected(EpistemicLimitError::InvalidIngestionBound);
    if (ingressQueuePerSource > ingressQueue)
      return std::unexpected(EpistemicLimitError::InvalidIngestionBound);
    return {};
  }
};

/// Validate signed system-setting values before converting them to allocation
/// bounds.
[[nodiscard]] inline std::expected<EpistemicLimits, EpistemicLimitError>
checkedEpistemicLimits(std::int64_t sources, std::int64_t candidates) {
  if (sources <= 0 || !std::in_range<std::size_t>(sources))
    return std::unexpected(EpistemicLimitError::InvalidSources);
  if (candidates <= 0 || !std::in_range<std::size_t>(candidates))
    return std::unexpected(EpistemicLimitError::InvalidCandidates);
  return EpistemicLimits{static_cast<std::size_t>(sources),
                         static_cast<std::size_t>(candidates)};
}

[[nodiscard]] inline std::expected<EpistemicLimits, EpistemicLimitError>
checkedEpistemicLimits(std::int64_t sources, std::int64_t candidates,
                       std::int64_t scopeDepth, std::int64_t nestedScopes) {
  auto limits = checkedEpistemicLimits(sources, candidates);
  if (!limits) return limits;
  if (scopeDepth <= 0 || !std::in_range<std::size_t>(scopeDepth))
    return std::unexpected(EpistemicLimitError::InvalidScopeDepth);
  if (nestedScopes <= 0 || !std::in_range<std::size_t>(nestedScopes))
    return std::unexpected(EpistemicLimitError::InvalidNestedScopes);
  limits->maxScopeDepth   = static_cast<std::size_t>(scopeDepth);
  limits->maxNestedScopes = static_cast<std::size_t>(nestedScopes);
  return limits;
}

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_LIMITS_HPP
