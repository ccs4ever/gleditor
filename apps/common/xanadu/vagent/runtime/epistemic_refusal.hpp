/**
 * @file epistemic_refusal.hpp
 * @brief The one typed refusal that resume and upgrade give back (P19 T1a).
 *
 * `ActorError` is a closed enum without a payload, so a caller could not tell
 * a changed policy from a populated profile from an active intention. This
 * type names the cause, the road, the subject and the offending texts and
 * numbers, and is produced before any state changes.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_REFUSAL_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_REFUSAL_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "common/xanadu/vortex/mesh_actor.hpp"

namespace xanadu::vagent::runtime {

enum class TransferRoute : std::uint8_t {
  Resume,
  UpgradeSchema,
  UpgradeReconciler
};

/// Each slice declares the codes it can produce and appends the rest.
enum class TransferCode : std::uint8_t {
  Runtime,
  NotQuiescent,
  ActiveIntention,
  PolicyChanged,
  PredicateIncompatible,
  PredicateRemoved,
  PeakCopyBudget,
  WorkBudget,
  ProgramReferenceReused,
  RouteCarriesNoEpistemicState,
  UnknownShapeVersion,    ///< T1b1
  PolicyEncodingChanged,  ///< T1b1
};

/// Texts are cut here so a hostile program cannot make a refusal large. Only
/// predicate names, principal tokens, hashes, field names and reason names
/// are ever put in one, never document text or key material.
inline constexpr std::size_t kRefusalTextLimit = 128;

struct RefusalDetail {
  /// Predicate, principal token, reason or dimension.
  std::optional<std::string> subject;
  /// The texts found in the old state and in the new program.
  std::optional<std::string> saved, current;
  std::optional<std::uint64_t> savedCount, currentCount, limit;
};

class TransferRefusal {
public:
  /// The only constructor, so `actor` is always a decided value.
  [[nodiscard]] static TransferRefusal
  make(TransferCode code, TransferRoute route,
       zigzag::vortex::mesh::ActorError actor, RefusalDetail detail = {});

  TransferCode code;
  TransferRoute route;
  /// What callers compared before this type existed.
  zigzag::vortex::mesh::ActorError actor;
  RefusalDetail detail;

private:
  TransferRefusal() = default;
};

[[nodiscard]] std::string_view toString(TransferCode code) noexcept;
[[nodiscard]] std::string_view toString(TransferRoute route) noexcept;
[[nodiscard]] std::string toString(const TransferRefusal &refusal);

} // namespace xanadu::vagent::runtime

#endif
