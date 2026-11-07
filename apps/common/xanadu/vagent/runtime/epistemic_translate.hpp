/**
 * @file epistemic_translate.hpp
 * @brief The routine resume and both upgrade roads share to carry P19 state
 *        from one graph to another (P19 T1c).
 *
 * The three copiers stay three; what they treat as foreign is everything
 * under `d.trust`, `d.epistemic` and the profile `d.clone` chains, and this
 * routine owns that half. `planTranslation` reads and refuses, so the
 * executor can ask it before a pid exists; `applyTranslation` is the only
 * writer, in four passes over one old-to-new map the caller seeded with the
 * clauses and supports the generic copier made.
 *
 * C++, not Vortex: it copies one arena into another between two contexts,
 * inside the host's atomic upgrade callback or before a pid exists, and the
 * old graph is read-only data during the swap.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_TRANSLATE_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_TRANSLATE_HPP

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vagent/runtime/epistemic_limits.hpp"
#include "common/xanadu/vagent/runtime/epistemic_policy.hpp"
#include "common/xanadu/vagent/runtime/epistemic_preflight.hpp"
#include "common/xanadu/vagent/runtime/epistemic_refusal.hpp"
#include "common/xanadu/vagent/runtime/epistemic_shape.hpp"
#include "common/xanadu/vagent/runtime/upgrade_schema.hpp"
#include "common/xanadu/vagent/runtime/wire.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vagent::runtime {

enum class Disposition : std::uint8_t { Resume, Upgrade };

/// The new graph's dimensions, one arena's refs.
struct DimBinding {
  zigzag::DimRef vars, values, grab, step, clone, clause, source, belief, trust,
      epistemic;
};

// -- Predicate keys
// ------------------------------------------------------------

/// What makes two predicates the same one for the state kept under them: the
/// canonical signature (name, arity, ordered types, key positions) and the
/// typed policy, absent when the predicate is not arbitrated.
struct PredicateKey {
  std::string signature;
  std::optional<ArbitrationPolicy> policy;
  bool operator==(const PredicateKey &) const = default;
};

/// Rebuild a policy from its master's typed cells; absent when a field is
/// missing or of the wrong kind.
[[nodiscard]] std::optional<ArbitrationPolicy>
decodePolicy(const zigzag::ArenaManifold &arena, const ValueDims &vd,
             zigzag::CellRef master);

[[nodiscard]] PredicateKey keyOf(std::string_view name,
                                 const UpgradePredicate &predicate);

/// The first 64 bits of SHA-256 over the signature. Only an index.
[[nodiscard]] std::uint64_t keyDigest(const PredicateKey &key);

/// The new program's keys, bucketed by digest and searched by full-key
/// equality, so a collision can cost a comparison and never a wrong match.
template <class Digest = std::uint64_t (*)(const PredicateKey &)>
class PredicateMatcher {
public:
  explicit PredicateMatcher(std::vector<PredicateKey> keys,
                            Digest digest = &keyDigest)
      : keys_(std::move(keys)), digest_(std::move(digest)) {
    for (std::size_t i = 0; i < keys_.size(); ++i)
      buckets_[digest_(keys_[i])].push_back(i);
  }
  /// The index of the key equal to @p key.
  [[nodiscard]] std::optional<std::size_t> find(const PredicateKey &key) const {
    const auto bucket = buckets_.find(digest_(key));
    if (bucket == buckets_.end()) return std::nullopt;
    for (const auto index : bucket->second)
      if (keys_[index] == key) return index;
    return std::nullopt;
  }

private:
  std::vector<PredicateKey> keys_;
  Digest digest_;
  std::unordered_map<std::uint64_t, std::vector<std::size_t>> buckets_;
};

/// Removal, encoding, policy and signature of every predicate the saved graph
/// holds a policy master for, against the new program's schema.
[[nodiscard]] std::optional<TransferRefusal>
checkPredicateKeys(const zigzag::ArenaManifold &arena, zigzag::CellRef agent,
                   const OwnedGraphDims &dims, const UpgradeSchema &into,
                   TransferRoute route);

// -- The routine
// -----------------------------------------------------------------

struct PlanRequest {
  Disposition disposition;
  TransferRoute route;
  const zigzag::ArenaManifold &from;
  zigzag::CellRef fromAgent;
  OwnedGraphDims fromDims;
  const UpgradeSchema &intoSchema;
  const TranslatorRegistry &registry;
  /// Absent: no cell or work budget is decided (an upgrade with no grant).
  std::optional<ResumeAllowance> allowance;
  /// Work the caller has already committed to, such as the schema comparison.
  std::uint64_t fixedWork{0};
  /// Scratch bytes the caller's copiers have already taken from the same
  /// grant: the generic copier runs first on the schema road, so the
  /// translator's texts must fit in what it left (S-3).
  std::uint64_t fixedBytes{0};
  /// The destination's `vagent.epistemic_health_reasons` and `_actions`. A
  /// profile's lists must fit them and its `health_cells` must be what they
  /// hold, or the admission reserve is computed from a forged number (S-2).
  std::int64_t healthReasons{kDefaultEpistemicHealthReasons};
  std::int64_t healthActions{kDefaultEpistemicHealthActions};
};

struct TranslationPlan {
  struct Profile {
    zigzag::CellRef cell;
    std::vector<zigzag::CellRef> views;
  };
  struct Record {
    zigzag::CellRef cell;
    /// The support or profile whose `d.epistemic` rank holds it.
    zigzag::CellRef anchor;
  };
  std::vector<Profile> profiles;
  std::vector<Record> records;
  /// Records on the agent's own `d.epistemic` rank besides the shape header,
  /// in rank order: the audience record (I3-3a2) and the clock lineage
  /// (I3-3b2).
  std::vector<zigzag::CellRef> agentRecords;
  std::uint64_t cells{0};
  std::uint64_t work{0};
};

struct TranslationReport {
  std::uint64_t cells{0};
  /// Instruction charges the passes made; the measured work per cell.
  std::uint64_t charges{0};
};

struct ApplyRequest {
  const PlanRequest &plan;
  zigzag::ArenaManifold &into;
  zigzag::CellRef intoAgent;
  DimBinding intoDims;
  /// Old to new; the caller seeds it with the clauses and supports it copied.
  std::unordered_map<zigzag::CellRef, zigzag::CellRef> &map;
  /// Remaining instruction grant, charged per step.
  std::uint64_t &work;
};

/// Read-only: refuses before anything is written.
[[nodiscard]] std::expected<TranslationPlan, TransferRefusal>
planTranslation(const PlanRequest &request);

// -- Agent-rank records (P19 I3-3a2), in epistemic_translate_ingress.cpp ----

/// The token of the agent's `EpistemicAudience` record, as text: a checkpoint
/// may type an all-digit identifier as a number, and the text is what every
/// road keeps.
[[nodiscard]] std::optional<std::string>
audienceOf(const zigzag::ArenaManifold &arena, zigzag::CellRef agent,
           zigzag::DimRef epistemic, const ValueDims &vd);

/// Whether @p record is a lineage of the one shape the routine writes (P19
/// I3-3b2, epistemic_translate_clock.cpp): a `segments` list of at most
/// `kMaxClockLineage` segments, each with an instance of 16 hex digits and
/// finite doubles, the wall anchor present as a pair or absent as a pair.
[[nodiscard]] bool clockLineageWellFormed(const zigzag::ArenaManifold &arena,
                                          const OwnedGraphDims &dims,
                                          zigzag::CellRef record);

/// The cells of the agent's `d.epistemic` rank that a transfer carries: the
/// audience record and the clock lineage, at most one of each, the first with
/// exactly its `token` field holding 64 hex and the second well formed.
/// Anything else on the rank but the shape header refuses
/// `UnregisteredOwnedStructure`; a malformed or repeated record refuses
/// `StateInconsistent`.
[[nodiscard]] std::expected<std::vector<zigzag::CellRef>, TransferRefusal>
agentRankRecords(const zigzag::ArenaManifold &arena, zigzag::CellRef agent,
                 const OwnedGraphDims &dims, TransferRoute route);

[[nodiscard]] std::expected<TranslationReport, TransferRefusal>
applyTranslation(const TranslationPlan &plan, const ApplyRequest &request);

/// What `setOwnedStateTranslator` registers.
using EpistemicTranslator =
    std::function<std::expected<TranslationReport, TransferRefusal>(
        const TranslationPlan &, const ApplyRequest &)>;

/// The translator for shape 1, registered at construction.
[[nodiscard]] EpistemicTranslator defaultEpistemicTranslator();

enum class DigestMode : std::uint8_t {
  Full,
  /// Omits `last_receive`, `last_contact` and the header, so a source, its
  /// resume and its in-place upgrade compare equal.
  ModuloClock
};

/// SHA-256 hex of a canonical stream of the profiles, their histories and the
/// supports' records. Clone occurrences go by position, doubles by their
/// stored bits, and no ref enters it.
[[nodiscard]] std::string
epistemicStateDigest(const zigzag::ArenaManifold &arena, zigzag::CellRef agent,
                     const OwnedGraphDims &dims, DigestMode mode);

} // namespace xanadu::vagent::runtime

#endif
