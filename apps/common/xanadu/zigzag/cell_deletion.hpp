/**
 * @file cell_deletion.hpp
 * @brief The one ZigZag cell-deletion algorithm (P7), device-free.
 *
 * Deleting a cell is: break every connection it has, remove the cell, and then
 * either leave those connections broken or repair them by joining each
 * dimension's negward and posward neighbours. Both are valid and the caller
 * chooses (DeletionMode). This header holds the part that does not depend on
 * where the cell lives -- reading the neighbourhood, planning the link edits,
 * and choosing where a cursor on the deleted cell goes -- so an ephemeral arena
 * (ArenaManifold::deleteCell), a persistent store's Structure operations
 * (xuzz's delete command) and engine reclamation (the I/O reapers) all run the
 * same plan rather than each hand-rolling the splice.
 *
 * What "remove" means is the only thing that differs: an arena cell is freed
 * (its slot is reused); a persistent cell was minted by an operation and an
 * append-only spool never un-mints it (store-slice-convergence R7/R8), so it
 * is detached -- it keeps its place in hypertime and has no connections.
 */
#ifndef COMMON_XANADU_ZIGZAG_CELL_DELETION_HPP
#define COMMON_XANADU_ZIGZAG_CELL_DELETION_HPP

#include <optional>
#include <span>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/zigzag/dim_vector.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace zigzag {

/// What happens to the connections a deleted cell leaves behind.
enum class DeletionMode {
  LeaveBroken, ///< every neighbour simply loses its link to the cell
  Repair,      ///< each dimension's two neighbours are joined to each other
};

/// The two cells a cell sits between along one dimension, either of which may
/// be absent. A cell linked to itself (a one-cell loop) names itself; the plan
/// clears such a link and never joins across it.
struct RankNeighbours {
  DimRef dim{noCell};
  std::optional<CellRef> negward;
  std::optional<CellRef> posward;

  [[nodiscard]] constexpr std::optional<CellRef>
  toward(const DimVector dir) const noexcept {
    return dir == DimVector::POS ? posward : negward;
  }
  bool operator==(const RankNeighbours &) const = default;
};

/// The neighbourhood a deletion broke: one entry per dimension the cell was
/// linked on, in the order the cell's run lists them.
struct Neighbourhood {
  CellRef cell{noCell}; ///< the cell that was deleted
  std::vector<RankNeighbours> ranks;

  /// Read @p cell's neighbourhood off its link run (dimensionsOf()). A
  /// dimension on which the cell has no neighbour is omitted.
  [[nodiscard]] static Neighbourhood of(CellRef cell,
                                        std::span<const DimLink> run);

  /// True when the cell had no connection at all.
  [[nodiscard]] bool isolated() const noexcept { return ranks.empty(); }
  /// The entry for @p dim, if the cell was linked on it.
  [[nodiscard]] std::optional<RankNeighbours> along(DimRef dim) const noexcept;
  bool operator==(const Neighbourhood &) const = default;
};

/// One write: point @p from's @p dir-ward neighbour on @p dim at @p to, or
/// clear it. Both ends are maintained by whatever applies it, as SetLink does.
struct LinkEdit {
  CellRef from{noCell};
  DimRef dim{noCell};
  DimVector dir{DimVector::POS};
  std::optional<CellRef> to;
  bool operator==(const LinkEdit &) const = default;
};

/// The clone dimension of a caller that wants a clone master with views
/// protected. A master is the head of its clone rank; deleting it either way
/// (Repair or LeaveBroken) makes its first view the new head, which does not
/// carry the master's content. That is the caller's decision, not the plan's:
/// the default refuses, and @c promoteFirstView says the caller means it.
struct CloneGuard {
  DimRef dim{noCell};
  bool promoteFirstView{false};
};

/// True when deleting the cell @p broken describes would leave views of a clone
/// master without it and @p guard does not allow that.
[[nodiscard]] bool
orphansViews(const Neighbourhood &broken,
             const std::optional<CloneGuard> &guard) noexcept;

/**
 * @brief The fewest link writes that delete @p cell from @p ranks in @p mode.
 *
 * Repair joins neighbours with one write per dimension: a link is a single
 * edge, so pointing the negward neighbour at the posward one already displaces
 * the cell from both its sides. A dimension where the cell had both
 * neighbours and they are the same cell (a rank of exactly two) is left
 * cleared rather than joined to itself. A dimension with one neighbour needs
 * only the cell's own link cleared in either mode. LeaveBroken clears each of
 * the cell's links. Idempotent writes are never emitted: each edit changes
 * something, which matters to an append-only spool that records every write.
 *
 * Clone ranks need no special case. A clone master is the head of its rank, so
 * Repair promotes its first view to master, and LeaveBroken leaves the views
 * as a rank headed by that first view; a view is just a member. A caller that
 * must not orphan a master's views passes a CloneGuard (orphansViews), or
 * checks first as the io_cells reapers do (heldByViews).
 */
void planDeletion(CellRef cell, const Neighbourhood &ranks, DeletionMode mode,
                  gleditor::cpp26::function_ref<void(const LinkEdit &)> emit);

/// planDeletion() for one dimension: take @p cell out of a single rank and
/// leave its other connections alone.
void planRankDeletion(
    CellRef cell, const RankNeighbours &ward, DeletionMode mode,
    gleditor::cpp26::function_ref<void(const LinkEdit &)> emit);

/**
 * @brief The deterministic cell a cursor on a deleted cell moves to.
 *
 * Order, fixed and deterministic: (1) the cursor's bound dimensions in their
 * binding order, and along each the posward neighbour before the negward one
 * -- the order the xuzz view (x, y, z) already used, kept as the convention
 * (it is an inherited choice, not derived: forward is the direction a reader
 * keeps moving); (2) when no bound dimension yields a cell, the cell's other
 * connections in neighbourhood order, again posward before negward; (3) with
 * no usable connection, @p home. @p eligible refuses candidates the caller
 * cannot land on (a protected or derived cell, a metadata dimension); it sees
 * the dimension and the candidate.
 *
 * Pure: it reads only its arguments, so the engine and any UI call it alike.
 * nullopt only when there is no eligible neighbour and no home.
 */
[[nodiscard]] std::optional<CellRef> cursorMoveOff(
    const Neighbourhood &broken, std::span<const DimRef> boundDimensions,
    std::optional<CellRef> home,
    gleditor::cpp26::function_ref<bool(DimRef, CellRef)> eligible =
        [](DimRef, CellRef) { return true; });

} // namespace zigzag

#endif
