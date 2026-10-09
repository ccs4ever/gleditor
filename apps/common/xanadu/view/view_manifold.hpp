/**
 * @file view_manifold.hpp
 * @brief A slice placement's view space: the real manifold, read-only, and
 *        two arenas of view cells over it (design/view-system.md §6, §8.2).
 *
 * The binding arena holds what the reader set and lives as long as the
 * placement; the derived arena holds what a view mints to show where the
 * cursor is, and every toss empties it. They are siblings rather than two
 * layers of one stack so that a binding edited after derived cells exist is
 * never minted above them, and emptying the derived cells never has to be
 * ordered against the rebind (§6.1).
 *
 * Both arenas number their cells from the same ephemeral base, so the first
 * cell of each has the same CellRef. Nothing here therefore takes a bare
 * CellRef for a view cell: a ViewCellRef says which arena it belongs to and
 * which generation, every call checks both, and a write names its layer
 * explicitly and is refused unless every ref it is given agrees (plan §2.2
 * G1, spike S4).
 *
 * No view cell is ever linked to a real one, in either direction, and no real
 * cell is ever written: an arena would shadow it, which makes a stale copy,
 * makes the toss proportional to the number of shadows and leaves view links
 * on a real cell (§6.2). A view cell stands for a real one by a handle value,
 * never a link (§6.3).
 */
#ifndef COMMON_XANADU_VIEW_VIEW_MANIFOLD_HPP
#define COMMON_XANADU_VIEW_VIEW_MANIFOLD_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/view/view_error.hpp"
#include "common/xanadu/view/view_ids.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu::view {

/// Which of a placement's two arenas a view cell lives in.
enum class Layer : std::uint8_t { Binding, Derived };

/// The epoch every binding cell carries: the binding arena is never tossed, so
/// its cells stay valid for the placement's life.
inline constexpr ViewEpoch bindingEpoch = 0;

/**
 * @brief A view cell and the generation it belongs to.
 *
 * A derived cell's ref is valid only in its own epoch, because a toss
 * truncates the derived arena and the next mint reuses the same dense index:
 * a bare CellRef kept across a toss would silently name a different cell.
 */
struct ViewCellRef {
  zigzag::CellRef ref{};
  ViewEpoch epoch{};
  Layer layer{Layer::Derived};

  bool operator==(const ViewCellRef &) const = default;
};

/// A view-owned dimension is a bare view cell used as a link key.
using ViewDim = ViewCellRef;

class ViewManifold;
class ViewAxisSet;
using ViewResult = std::expected<ViewManifold *, ViewError>;

/// One thing verifyViewSpace() found wrong. An arena-wide violation, such as
/// a trail on the derived arena, names a layer but no cell.
struct ViewSpaceViolation {
  std::optional<ViewCellRef> cell;
  Layer layer{Layer::Derived};
  std::string_view rule; ///< "I3", "I4", "two-sided", "rank-shape", ...
};

/**
 * @class ViewManifold
 * @brief The real manifold plus a placement's binding and derived arenas.
 *
 * Strategies and views are handed a ViewManifold & and have no other way to
 * write a view cell: mint(), mintOccurrence(), link() and unlink() are the
 * whole write path, and each refuses rather than displaces (§6.6).
 */
class ViewManifold {
public:
  /// @p base must outlive this and must not change while this reads it.
  explicit ViewManifold(const zigzag::Manifold &base);

  ViewManifold(const ViewManifold &)            = delete;
  ViewManifold &operator=(const ViewManifold &) = delete;
  /// The axis set keeps a pointer back to its space, re-seated here.
  ViewManifold(ViewManifold &&other) noexcept;
  ViewManifold &operator=(ViewManifold &&) = delete;
  ~ViewManifold();

  [[nodiscard]] const zigzag::Manifold &base() const noexcept { return base_; }
  [[nodiscard]] ViewEpoch epoch() const noexcept { return epoch_; }

  /// The placement's bindings (§7, §8.3), which live in the binding arena.
  [[nodiscard]] ViewAxisSet &axes() noexcept { return *axes_; }
  [[nodiscard]] const ViewAxisSet &axes() const noexcept { return *axes_; }

  /// Whether @p cell belongs to the current generation of its own layer.
  [[nodiscard]] bool isCurrent(ViewCellRef cell) const noexcept;

  // -- mint: every write of a view cell goes through these five ------------

  /// A bare cell.
  [[nodiscard]] std::expected<ViewCellRef, ViewError> mint(Layer layer);
  /// A cell holding @p text, as a view-owned dimension's name does.
  [[nodiscard]] std::expected<ViewCellRef, ViewError>
  mint(Layer layer, std::string_view text);
  /**
   * @brief A cell that stands for @p target by handle, never by link.
   *
   * @p target is a real cell of the base, kept as the cell's own ref so that
   * two occurrences of one cell hold the same value; or, for the binding
   * layer only, a live group of axes(). A derived occurrence of a group is
   * refused: the two arenas' refs can be numerically equal, so the handle
   * would not say which it meant.
   */
  [[nodiscard]] std::expected<ViewCellRef, ViewError>
  mintOccurrence(Layer layer, zigzag::CellRef target);

  /**
   * @brief Link @p from's @p dir-ward neighbour on @p dim to @p to.
   *
   * Refuses, changing nothing, when a ref or the dimension is stale
   * (StaleEpoch); when any of the three is not a view cell of @p layer's arena
   * (RealCellInViewLink); or when @p from already has another neighbour on
   * @p dim in @p dir, or @p to another in the opposite direction
   * (OccupiedDirection). The arena would evict in that last case, which is
   * right for unification and, for a view, hides a derivation bug. Linking a
   * pair that is already linked is a success that writes nothing.
   */
  [[nodiscard]] ViewResult link(Layer layer, ViewCellRef from, ViewDim dim,
                                zigzag::DimVector dir, ViewCellRef to) noexcept;
  /// Clear @p from's @p dir-ward neighbour on @p dim, and the far end's
  /// reciprocal. Nothing there is a success that writes nothing.
  [[nodiscard]] ViewResult unlink(Layer layer, ViewCellRef from, ViewDim dim,
                                  zigzag::DimVector dir) noexcept;
  /// Restate a view cell's text, as renaming a group does. The text lives in
  /// the arena's own scratch buffer, never in a permascroll.
  [[nodiscard]] ViewResult setText(Layer layer, ViewCellRef cell,
                                   std::string_view text);

  // -- read ----------------------------------------------------------------

  /// @p from's neighbour on @p dim, or nothing: also nothing when either is
  /// stale or they are not cells of the same layer, so a ref of one arena can
  /// never be answered from the other.
  [[nodiscard]] std::optional<ViewCellRef>
  linked(ViewCellRef from, ViewDim dim, zigzag::DimVector dir) const noexcept;
  /// A view cell's text, or nothing for a stale or foreign ref.
  [[nodiscard]] std::optional<std::string> text(ViewCellRef cell) const;
  /// What an occurrence stands for, or nothing for any other cell.
  [[nodiscard]] std::optional<zigzag::CellRef>
  target(ViewCellRef occurrence) const noexcept;
  /**
   * @brief I5: the real cell a view cell stands for.
   *
   * An occurrence answers its target in one read. A pack container answers
   * its first constituent that resolves, at most one step per nesting level:
   * d.pack from the container to its first constituent, then d.packing along
   * the constituents. A group, and an occurrence of one, answers as a pack
   * does: its first member that resolves, depth first, so an empty group is
   * UnknownTarget. Anything else is UnknownTarget.
   */
  [[nodiscard]] std::expected<zigzag::CellRef, ViewError>
  resolveReal(ViewCellRef cell) const noexcept;

  // -- the derived generation ----------------------------------------------

  /// View-owned dimensions of the current epoch, minted on first use.
  [[nodiscard]] ViewDim packDim();    ///< d.pack: container, then first
  [[nodiscard]] ViewDim packingDim(); ///< d.packing: constituents in order
  [[nodiscard]] ViewDim axisStepDim(ViewAxisId axis); ///< packs along an axis

  /// The same dimensions as read by a caller that must not mint, such as
  /// cellAt(): nothing until a prepare() of this epoch has minted them.
  [[nodiscard]] std::optional<ViewDim> findPackDim() const noexcept;
  [[nodiscard]] std::optional<ViewDim> findPackingDim() const noexcept;
  [[nodiscard]] std::optional<ViewDim>
  findAxisStepDim(ViewAxisId axis) const noexcept;
  /**
   * @brief An occurrence of @p target in @p dim's layer that is on @p dim.
   *
   * The way back from a real cell into derived structure, as the origin of a
   * rank of packs is found from the cursor's real origin (§9.3.3). A scan of
   * the arena, which derivation keeps to what is on screen (§6.7).
   */
  [[nodiscard]] std::optional<ViewCellRef>
  findOccurrence(ViewDim dim, zigzag::CellRef target) const noexcept;

  [[nodiscard]] std::size_t derivedCellCount() const noexcept {
    return derived_.cellCount();
  }
  [[nodiscard]] std::size_t cellCount(Layer layer) const noexcept {
    return arena(layer).cellCount();
  }

  /**
   * @brief Empty the derived arena and start a new generation (§6.5).
   *
   * release() to the mark taken on the empty arena: with no shadows (I3), no
   * trail (every derived cell is minted under that mark) and no attached
   * spaces, that is a fixed number of truncations whatever was minted. The
   * storage is kept for the next generation, not returned.
   */
  ViewManifold *toss() noexcept;

  // -- what verifyViewSpace and the toss test read (plan §2.2 G3) ----------

  [[nodiscard]] std::size_t shadowCount(Layer layer) const noexcept {
    return arena(layer).shadowCount();
  }
  [[nodiscard]] std::size_t trailSize(Layer layer) const noexcept {
    return arena(layer).trailSize();
  }
  [[nodiscard]] std::size_t spaceCount(Layer layer) const noexcept {
    return arena(layer).spaceCount();
  }

  /// Direct write access to one arena, bypassing every check above. Only for
  /// a test proving that verifyViewSpace() finds the damage it is there for.
  [[nodiscard]] zigzag::ArenaManifold &arenaForTesting(Layer layer) noexcept {
    return Layer::Binding == layer ? bindings_ : derived_;
  }

private:
  friend class ViewAxisSet;
  friend std::size_t verifyViewSpace(
      const ViewManifold &space,
      gleditor::cpp26::function_ref<void(const ViewSpaceViolation &)> report);

  [[nodiscard]] const zigzag::ArenaManifold &arena(Layer layer) const noexcept {
    return Layer::Binding == layer ? bindings_ : derived_;
  }
  [[nodiscard]] zigzag::ArenaManifold &arena(Layer layer) noexcept {
    return Layer::Binding == layer ? bindings_ : derived_;
  }
  [[nodiscard]] ViewEpoch epochOf(Layer layer) const noexcept {
    return Layer::Binding == layer ? bindingEpoch : epoch_;
  }
  [[nodiscard]] ViewCellRef wrap(Layer layer,
                                 zigzag::CellRef ref) const noexcept {
    return ViewCellRef{.ref = ref, .epoch = epochOf(layer), .layer = layer};
  }

  /// Whether @p ref is a cell @p layer's arena minted itself.
  [[nodiscard]] bool isViewCell(Layer layer,
                                zigzag::CellRef ref) const noexcept;
  /// Current, of @p layer, and a view cell of it; or why not.
  [[nodiscard]] std::expected<void, ViewError>
  check(Layer layer, ViewCellRef cell) const noexcept;
  /// @p target as the base's own name for that cell, if it is a real cell.
  [[nodiscard]] std::optional<zigzag::CellRef>
  realCell(zigzag::CellRef target) const noexcept;
  [[nodiscard]] std::expected<zigzag::CellRef, ViewError>
  resolveIn(Layer layer, zigzag::CellRef cell,
            std::size_t &budget) const noexcept;
  [[nodiscard]] ViewDim derivedDim(std::optional<zigzag::CellRef> &slot,
                                   std::string_view name);

  const zigzag::Manifold &base_;
  zigzag::ArenaManifold bindings_;
  zigzag::ArenaManifold derived_;
  zigzag::Mark empty_; ///< taken on the empty derived arena
  ViewEpoch epoch_{bindingEpoch + 1};

  // The current generation's dimension cells; cleared by toss().
  std::optional<zigzag::CellRef> packDim_;
  std::optional<zigzag::CellRef> packingDim_;
  std::vector<std::optional<zigzag::CellRef>> axisStepDims_;

  std::unique_ptr<ViewAxisSet> axes_;
};

/**
 * @brief Report every violation of the view-space invariants @p space holds.
 *
 * Checks, in both arenas: I3 (a shadowed real cell); a link key or far end
 * that is not a view cell of the same arena; links that are not two-sided; an
 * occurrence whose target is not a real cell of the base or, in the binding
 * arena, a live group; a d.pack or d.packing rank that loops. On the derived
 * arena it also reports a trail or an attached space, either of which would
 * make the toss cost more than a fixed number of steps (I4). On the binding
 * arena it reports every rank of the binding model with the wrong shape and
 * a group reachable from itself (ViewAxisSet).
 *
 * An occurrence on no rank at all is garbage an undo or a deleted group left
 * behind, unreachable from anything; it may name a group that is gone.
 *
 * @return how many violations @p report was called with.
 */
std::size_t verifyViewSpace(
    const ViewManifold &space,
    gleditor::cpp26::function_ref<void(const ViewSpaceViolation &)> report);

} // namespace xanadu::view

#endif // COMMON_XANADU_VIEW_VIEW_MANIFOLD_HPP
