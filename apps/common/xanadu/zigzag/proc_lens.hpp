/**
 * @file proc_lens.hpp
 * @brief ProcLens: Linux Processes Projected as Multidimensional ZigZag Space.
 *
 * Replaces the Plan 9 /proc file hierarchy with dimensions and cells.
 *
 * Invariants:
 * - Topological Invariant A2: Process trees use d.child (firstborn child) and
 *   d.sibling (birth order) to strictly preserve deg^+(c, d) <= 1 and deg^-(c,
 * d) <= 1.
 * - Thread ranks link along d.threads.
 * - Scalar metrics (PID, state, CPU%, RSS memory) are carried in cell
 * attributes.
 * - Supports host /proc inspection and deterministic synthetic mock process
 * trees.
 */
#ifndef ZIGZAG_PROC_LENS_HPP
#define ZIGZAG_PROC_LENS_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/zigzag/projection_lens.hpp"

namespace zigzag {

struct ProcessSnapshot {
  std::uint32_t pid{0};
  std::uint32_t ppid{0};
  std::string comm;
  char state{'R'};
  double cpuPercent{0.0};
  std::uint64_t rssBytes{0};
  std::vector<std::uint32_t> threadTids;
};

/**
 * @class ProcLens
 * @brief ProjectionLens turning OS process hierarchies into canonical ZigZag
 * cells.
 */
class ProcLens : public ProjectionLens {
public:
  ProcLens() = default;

  /**
   * @brief Create a ProcLens with a synthetic static process tree (for tests
   * and mocking).
   */
  explicit ProcLens(std::vector<ProcessSnapshot> syntheticTree);

  [[nodiscard]] std::vector<std::pair<std::string, std::optional<GenericDim>>>
  dimensions() const override;

  void forEachNode(gleditor::cpp26::function_ref<void(const SourceNode &)> emit)
      const override;

  void forEachEdge(gleditor::cpp26::function_ref<void(const SourceEdge &)> emit)
      const override;

  /**
   * @brief Inspect the live host Linux /proc directory.
   */
  void refreshFromHost();

  /**
   * @brief Update with a new synthetic process snapshot.
   */
  void setProcessTree(std::vector<ProcessSnapshot> tree);

  /**
   * @brief Dispatch a control action (e.g. "kill -9", "pause", "resume") for @p
   * pid.
   */
  [[nodiscard]] std::expected<void, std::string>
  dispatchCommand(std::uint32_t pid, std::string_view command);

private:
  std::vector<ProcessSnapshot> processes_;
};

} // namespace zigzag

#endif // ZIGZAG_PROC_LENS_HPP
