/**
 * @file or_parallel.hpp
 * @brief Adaptive Oracle (Model 1) + Shadow-Delta (Model 2) OR-Parallel Search
 *        Coordinator over Vicon's d.or, d.branch, d.frontier, and d.submission
 *        dimensions.
 */
#ifndef COMMON_XANADU_VORTEX_OR_PARALLEL_HPP
#define COMMON_XANADU_VORTEX_OR_PARALLEL_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex {

inline constexpr std::uint64_t kRecomputeCycleThreshold = 10000;

enum class SparkModelKind : std::uint8_t {
  OracleRecompute   = 0,
  ShadowDeltaPrefix = 1,
};

struct OrParallelSpark {
  SparkModelKind model{SparkModelKind::OracleRecompute};
  std::vector<std::uint16_t> deweyPath{};
  std::string deweyKey{};
  std::uint64_t cyclesSinceRoot{0};
  CellRef entryOpcode{noCell};
  CellRef cursor{noCell};
  CellRef stackFrame{noCell};
  std::optional<ArenaManifold::PrefixSnapshot> prefixSnapshot{};
};

[[nodiscard]] std::string encodeOrParallelSpark(const OrParallelSpark &spark);

[[nodiscard]] std::optional<OrParallelSpark>
decodeOrParallelSpark(std::string_view bytes);

[[nodiscard]] bool isDeweySubtreeOf(std::string_view candidateDeweyKey,
                                    std::string_view prunedPrefix) noexcept;

struct OrParallelSolution {
  std::vector<std::uint16_t> deweyPath{};
  std::string deweyKey{};
  std::string value{};
  CellRef submissionCell{noCell};
};

class OrParallelCoordinator {
public:
  explicit OrParallelCoordinator(
      const Manifold *base         = nullptr,
      std::uint64_t cycleThreshold = kRecomputeCycleThreshold);

  [[nodiscard]] ArenaManifold &coordinatorArena() noexcept { return arena_; }
  [[nodiscard]] const ArenaManifold &coordinatorArena() const noexcept {
    return arena_;
  }
  [[nodiscard]] VortexCore &coordinatorCore() noexcept { return core_; }
  [[nodiscard]] const VortexCore &coordinatorCore() const noexcept {
    return core_;
  }
  [[nodiscard]] CellRef goalRootCell() const noexcept { return goalRoot_; }

  OrParallelCoordinator *
  setCycleThreshold(std::uint64_t cycleThreshold) noexcept {
    cycleThreshold_ = cycleThreshold;
    return this;
  }

  OrParallelCoordinator *
  setCutBroadcastCallback(std::function<void(std::string_view)> cb) {
    std::scoped_lock lock(mu_);
    cutBroadcastCallback_ = std::move(cb);
    return this;
  }

  /// Steal the oldest (nearest-to-root) choice point from @p victimVm,
  /// selecting Model 1 (OracleRecompute) when cyclesSinceRoot < threshold and
  /// Model 2 (ShadowDeltaPrefix) when cyclesSinceRoot >= threshold.
  std::optional<OrParallelSpark> exportSparkFromVictim(VortexVM &victimVm,
                                                       CellRef rootEntryOpcode);

  /// Enqueue a spark onto the coordinator's +d.frontier and +d.or/+d.branch
  /// topology.
  OrParallelCoordinator *enqueueSpark(OrParallelSpark spark);

  /// Steal the oldest non-pruned spark from the +d.frontier queue and register
  /// a cooperative std::stop_token for it.
  std::optional<std::pair<OrParallelSpark, std::stop_token>> acquireNextSpark();

  /// Register an externally acquired spark for cooperative #Cut cancellation.
  std::stop_token registerActiveSpark(std::string_view deweyKey);

  /// Prune all queued and running sparks whose Dewey path falls under
  /// @p deweyPrefix, firing their std::stop_source tokens and broadcasting
  /// #Cut.
  std::size_t prunePrefix(std::string_view deweyPrefix);

  [[nodiscard]] bool isPruned(std::string_view deweyKey) const;

  /// Record a solution and rebuild the +d.submission rank in deterministic
  /// lexicographical Dewey order. Returns false if the branch was pruned by
  /// #Cut.
  bool submitSolution(std::span<const std::uint16_t> deweyPath,
                      std::string_view value);

  /// Hydrate and execute @p spark on an isolated worker ArenaManifold over
  /// @p base.
  static ExecutionResult executeSparkOnWorker(
      const OrParallelSpark &spark, const Manifold *base,
      const std::function<void(VortexCore &, VortexVM &)> &bindProgramOpcodes,
      const std::function<std::optional<std::string>(VortexCore &, VortexVM &)>
          &extractSolution,
      OrParallelCoordinator &coordinator, std::stop_token stopToken = {},
      std::size_t maxCycles = 100000);

  [[nodiscard]] std::vector<OrParallelSolution> orderedSolutions() const;
  [[nodiscard]] std::vector<CellRef> submissionRankCells() const;
  [[nodiscard]] std::vector<CellRef> frontierRankCells() const;

private:
  void rebuildFrontierRankLocked();
  void rebuildSubmissionRankLocked();

  std::uint64_t cycleThreshold_{kRecomputeCycleThreshold};
  ArenaManifold arena_;
  VortexCore core_;
  DimRef dimOr_{noCell};
  DimRef dimBranch_{noCell};
  DimRef dimFrontier_{noCell};
  DimRef dimSubmission_{noCell};
  CellRef goalRoot_{noCell};

  mutable std::mutex mu_;
  std::vector<OrParallelSpark> frontierQueue_;
  std::unordered_map<std::string, std::stop_source> activeStopSources_;
  std::vector<std::string> prunedPrefixes_;
  std::map<std::vector<std::uint16_t>, OrParallelSolution> solutions_;
  std::function<void(std::string_view)> cutBroadcastCallback_;
};

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_OR_PARALLEL_HPP
