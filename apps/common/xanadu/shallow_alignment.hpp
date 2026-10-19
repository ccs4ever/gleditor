#ifndef XANADU_SHALLOW_ALIGNMENT_HPP
#define XANADU_SHALLOW_ALIGNMENT_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace xanadu {

/// An exact visible page fragment, not an authored left/right pairing. Keys are
/// scoped to the snapshot: the caller retains durable navigation identities.
struct SlopeFragment {
  std::uint64_t link{};
  std::uint64_t occurrence{};
  std::uint64_t fragment{};
  bool right{};
  bool translated{};
  bool visible{true};
  double x{};
  double y{};
};

/// Projected-pixel limits, supplied by the host after pinning reading focus and
/// intersecting document/chrome visibility bounds. X, scale and camera never
/// change. This device-free solver cannot establish those host preconditions.
struct ShallowAlignmentParams {
  double minimumDelta{};
  double maximumDelta{};
  double minimumRun{};
  double slopeSaturation{};
  double movementScale{};
  double movementWeight{};
  double worstSlopeWeight{};
  double minimumImprovement{};
  double gatheringShare{};
  std::size_t candidates{};
  std::size_t maximumFragments{};
};

enum class ShallowAlignmentError {
  InvalidParameters,
  InvalidFragment,
  ConflictingDuplicate,
  WorkLimit,
  NumericalOverflow
};

struct ShallowAlignmentResult {
  double delta{};
  double originalCost{};
  double cost{};
  std::size_t fragments{};
  std::size_t occurrences{};
  std::size_t evaluations{};
};

/// Evaluate a fixed budget of bounded destinations once. Duplicate scaffold
/// references count once; fragments of one occurrence share its vote. Missing
/// opposite sides are omitted, never paired with another link. An unchanged
/// result is an honest refusal. The caller applies no motion until it has
/// rechecked that this settled snapshot and its reading focus are still
/// current.
[[nodiscard]] std::expected<ShallowAlignmentResult, ShallowAlignmentError>
preferShallowAlignment(std::span<const SlopeFragment> fragments,
                       const ShallowAlignmentParams &params);

} // namespace xanadu

#endif
