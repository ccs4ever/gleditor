/**
 * @file epistemic_preflight.hpp
 * @brief Read-only checks that decide a resume before a pid exists (P19 T1a).
 *
 * Everything here reads an arena and writes nothing, so the executor can ask
 * "would this checkpoint be admitted?" against the received base without a
 * `WorkerContext`, and `restoreState` can ask again against the rebuilt arena
 * with the same code.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_PREFLIGHT_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_PREFLIGHT_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "common/xanadu/vagent/runtime/epistemic_refusal.hpp"
#include "common/xanadu/vagent/runtime/upgrade_schema.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vagent::runtime {

/// What the grant a resume will be issued allows, as far as the host knows
/// before the runtime asks its authorizer.
struct ResumeLimits {
  std::uint64_t instructions{};
  std::uint32_t maxCells{};
};

/// Instruction charges a cell is expected to cost across a transfer's passes
/// (the T1 design's estimate, 14, against the 10,000,000 default grant).
/// The translation routine replaces it with a measured figure.
inline constexpr std::uint64_t kTransferWorkPerCell = 14;

/// Dimensions of a saved graph by name. A dimension the graph never links
/// along is absent, and so is everything it would have carried.
using DimTable = std::unordered_map<std::string, zigzag::DimRef>;

/// Finds the base's dimensions by name from @p root. The rebuilt base does not
/// register its dimensions, so the refs the running arena used are not the
/// refs of the cells; the names are. Only the base's cells are walked, since
/// the core hangs its standard library off the same home cell.
[[nodiscard]] DimTable discoverDimensions(const zigzag::ArenaManifold &arena,
                                          zigzag::CellRef root);

/// The dimensions that carry an agent's epistemic state, each absent when the
/// graph has none.
struct OwnedGraphDims {
  std::optional<zigzag::DimRef> vars, values, grab, step, clone, clause, source,
      belief, trust, epistemic;

  [[nodiscard]] static OwnedGraphDims byName(const DimTable &table);
};

/// Cells of the profiles, their histories and every support's history, with
/// what they hold, counted up to @p ceiling + 1 so a hostile image cannot
/// make the walk unbounded.
[[nodiscard]] std::size_t countOwnedCells(const zigzag::ArenaManifold &arena,
                                          zigzag::CellRef agent,
                                          const OwnedGraphDims &dims,
                                          std::size_t ceiling);

/// What a resume may use: the host's epistemic cell ceiling and, when the host
/// can say, the grant that will be issued.
struct ResumeAllowance {
  std::size_t epistemicCells{};
  std::optional<ResumeLimits> grant;
};

/// Policy and removal against the new program's schema.
[[nodiscard]] std::optional<TransferRefusal>
checkResumePolicy(const zigzag::ArenaManifold &arena, zigzag::CellRef root,
                  const DimTable &dims, const UpgradeSchema &into);

/// The cell and work budgets of copying the owned graph.
[[nodiscard]] std::optional<TransferRefusal>
checkResumeBudget(const zigzag::ArenaManifold &arena, zigzag::CellRef root,
                  const DimTable &dims, const ResumeAllowance &allowance);

/// An apply-time refusal travels out of `restoreState` as this, so `prepare`
/// can store it for `AgentExecutor::lastRefusal`.
class TransferRefusalError : public std::runtime_error {
public:
  explicit TransferRefusalError(TransferRefusal refusal)
      : std::runtime_error(toString(refusal)), refusal_(std::move(refusal)) {}
  [[nodiscard]] const TransferRefusal &refusal() const noexcept {
    return refusal_;
  }

private:
  TransferRefusal refusal_;
};

} // namespace xanadu::vagent::runtime

#endif
