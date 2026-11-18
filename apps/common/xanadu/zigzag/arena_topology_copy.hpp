#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace zigzag {

/**
 * @brief Which signed edges a topology copy follows, resolved by the caller
 *        from a shape's rules so no rule walk happens at run time.
 *
 * An edge is selected when it was added after the last wildcard, or when a
 * wildcard selected everything and the edge was not removed since. The caller
 * folds the rules in order, so a later rule on one signed edge has already
 * replaced an earlier one.
 */
struct EdgeSelection {
  bool everything{false};
  std::vector<DirectedDim> add;
  std::vector<DirectedDim> remove;

  [[nodiscard]] bool selects(DirectedDim edge) const noexcept;
};

/// Why a topology copy was refused before it minted anything.
enum class TopologyCopyRefusal : std::uint8_t {
  UnknownSubject,  ///< a subject is not a cell the arena holds
  QuoteOccurrence, ///< a presentation cell, whose edges are view-induced
  HandleValue,     ///< a handle names the program, not a value to copy
  RuntimeObject,   ///< a closure or bindings record, which holds cell
                   ///< references as integers a copy would leave dangling
  AllocationLimit, ///< the copy would not fit under the arena's cell limit
};

[[nodiscard]] constexpr const char *toString(TopologyCopyRefusal why) noexcept {
  switch (why) {
  case TopologyCopyRefusal::UnknownSubject:
    return "subject is not a cell of this arena";
  case TopologyCopyRefusal::QuoteOccurrence:
    return "quote occurrence cell";
  case TopologyCopyRefusal::HandleValue:
    return "handle-valued cell";
  case TopologyCopyRefusal::RuntimeObject:
    return "runtime object, which names cells by reference";
  case TopologyCopyRefusal::AllocationLimit:
    return "copy exceeds the arena's cell allocation limit";
  }
  return "unknown";
}

struct TopologyCopyRefused {
  TopologyCopyRefusal reason{TopologyCopyRefusal::UnknownSubject};
  CellRef cell{noCell};
};

[[nodiscard]] std::string toString(const TopologyCopyRefused &refused);

/**
 * @brief Copy the sub-graph reachable from @p subjects along the selected
 *        edges into fresh ephemeral cells, answering the copy of each subject.
 *
 * Content spans and quote bindings are shared, not re-spelled: a cell that
 * quotes store bytes copies to a quote of the same spans in the same space, so
 * the copy still transcludes them, and scratch spans are copied by value. A
 * cell's content and group master follow @p cloneDim, as a clone reads. The
 * copy links only to copies -- never to a source cell, which would overwrite
 * that cell's reciprocal edge -- and no source cell is written. Shared cells
 * and cycles stay shared and finite, memoised by source cell.
 *
 * The walk writes no existing cell and finishes before the first copy is
 * minted, so a refusal leaves no copy and no source touched. It is not
 * byte-for-byte as it was: a foreign neighbour is read with linkedMinting,
 * which mints the proxy standing for it, and that proxy stays. Each cell and
 * each edge probed consumes work, which bounds a copy by the arena's execution
 * limits.
 */
[[nodiscard]] std::expected<std::vector<CellRef>, TopologyCopyRefused>
copyTopology(ArenaManifold &arena, std::span<const CellRef> subjects,
             const EdgeSelection &selection, DimRef cloneDim,
             std::optional<DimRef> runtimeObjectClaim = std::nullopt);

} // namespace zigzag
