/**
 * @file view_binding.hpp
 * @brief The binding model: binding points and their roles, what each shows,
 *        dimension groups, the ring order and the pouch, with undo and replay
 *        by name (design/view-system.md §7, §8.3; plan §2.2 G2, G9, G11).
 *
 * Bindings are cells in a placement's binding arena, on view-owned
 * dimensions (§7.1):
 *
 *   d.axes        the axis head, then one slot per binding point, in order
 *   d.axis-role   a slot, then a cell naming its role
 *   d.binds       a slot, then the occurrence of what it shows
 *   d.view-groups the group head, then every live group, in order
 *   d.dim-group   a group, then one occurrence per member, in order
 *   d.ring-order  the ring head, then one occurrence per dimension
 *   d.dim-pouch   the pouch head, then one occurrence per dimension kept
 *
 * Every use of a dimension or a group is its own occurrence, so a dimension
 * can be on any number of axes and in any number of groups and nothing is
 * spent twice. The real dimension cell is never written. d.axis-role and
 * d.view-groups are not in the spec's table: a role is configuration the
 * placement must remember per point, and a group with no members yet is on no
 * other rank, so without its own list there would be no way to tell a group
 * from any other bare cell.
 *
 * Nothing is minted until the first edit, so a view space that never binds
 * anything costs nothing and its arenas' first cells are what its own caller
 * minted.
 */
#ifndef COMMON_XANADU_VIEW_VIEW_BINDING_HPP
#define COMMON_XANADU_VIEW_VIEW_BINDING_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/view/view_error.hpp"
#include "common/xanadu/view/view_ids.hpp"
#include "common/xanadu/view/view_manifold.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu {
class SpanReader;
}

namespace xanadu::view {

// -- roles (G2) -------------------------------------------------------------

/**
 * @brief What binding at a point means (§7.3).
 *
 * The first implementation has three roles, in the engine, and no registry:
 * one waits for a fourth role to exist. The subspace and hypertime roles are
 * named so that a configured point can say what it is, but their cursor rules
 * are package E16 and the owner has deferred the `u` and `t` points, so no
 * default configuration uses them.
 */
class AxisRole {
public:
  AxisRole()                            = default;
  AxisRole(const AxisRole &)            = delete;
  AxisRole &operator=(const AxisRole &) = delete;
  AxisRole(AxisRole &&)                 = delete;
  AxisRole &operator=(AxisRole &&)      = delete;
  virtual ~AxisRole()                   = default;

  /// The word layout.bindingPoints names it by.
  [[nodiscard]] virtual std::string_view id() const noexcept = 0;
  /// Whether a view gives a point with this role a direction of its space.
  [[nodiscard]] virtual bool spatial() const noexcept = 0;
};

[[nodiscard]] const AxisRole &spatialRole() noexcept;
[[nodiscard]] const AxisRole &subspaceRole() noexcept;
[[nodiscard]] const AxisRole &hypertimeRole() noexcept;
/// The role @p id names, if any.
[[nodiscard]] gleditor::cpp26::optional<const AxisRole &>
roleNamed(std::string_view id) noexcept;

/// One configured binding point: its name, which is its compass label and
/// its key in the selector, and its role.
struct BindingPoint {
  std::string name;
  std::reference_wrapper<const AxisRole> role;
};

/**
 * @brief The points layout.bindingPoints lists, in order.
 *
 * The setting is a flat list of name and role pairs, one cell each, so a
 * point is added by adding two cells rather than by editing a string that
 * would need parsing. A pair whose role is unknown, or a name left without
 * a role, is skipped and logged.
 */
[[nodiscard]] std::vector<BindingPoint>
bindingPointsFrom(std::span<const CellValue> setting);

// -- the axis set -----------------------------------------------------------

/// A real dimension cell, or a group cell of this placement's binding arena.
using BindTarget = zigzag::CellRef;

class ViewAxisSet;
using AxisResult = std::expected<ViewAxisSet *, ViewError>;

/**
 * @class ViewAxisSet
 * @brief Every edit of a placement's bindings (§8.3).
 *
 * Each mutator writes the binding arena only through ViewManifold's mint,
 * link, unlink and setText, refuses rather than half-applies, and pushes its
 * inverse for undo(). The caller tosses afterwards (§7.4): nothing here
 * touches the derived arena. Undo is local to the placement and never
 * hypertime; it appends nothing to any store (§7.5).
 *
 * A ViewAxisId is a place on the d.axes rank, so removing an axis renumbers
 * the ones after it.
 */
class ViewAxisSet {
public:
  explicit ViewAxisSet(ViewManifold &space) noexcept : space_(&space) {}

  ViewAxisSet(const ViewAxisSet &)            = delete;
  ViewAxisSet &operator=(const ViewAxisSet &) = delete;
  ViewAxisSet(ViewAxisSet &&)                 = delete;
  ViewAxisSet &operator=(ViewAxisSet &&)      = delete;
  ~ViewAxisSet()                              = default;

  // -- axes ----------------------------------------------------------------

  [[nodiscard]] std::size_t axisCount() const noexcept; ///< not a cap
  /// A new binding point at the end of the axes, showing nothing.
  [[nodiscard]] std::expected<ViewAxisId, ViewError>
  addAxis(std::string_view name, const AxisRole &role);
  /// Add each of @p points not already present by name, in order, and forget
  /// the history: configuring the placement is not an edit to undo.
  AxisResult configure(std::span<const BindingPoint> points);
  AxisResult removeAxis(ViewAxisId axis);
  /// Replace what the axis shows. The target's other axes are left alone.
  AxisResult bind(ViewAxisId axis, BindTarget target);
  AxisResult unbind(ViewAxisId axis);
  AxisResult swap(ViewAxisId first, ViewAxisId second);
  [[nodiscard]] std::optional<BindTarget> shown(ViewAxisId axis) const noexcept;
  [[nodiscard]] std::optional<ViewAxisId>
  axisNamed(std::string_view name) const;
  [[nodiscard]] std::optional<std::string> pointName(ViewAxisId axis) const;
  [[nodiscard]] gleditor::cpp26::optional<const AxisRole &>
  role(ViewAxisId axis) const;
  [[nodiscard]] bool isGroup(BindTarget target) const noexcept;

  // -- groups --------------------------------------------------------------

  /// A group of @p members, in order; any member that is neither a real
  /// dimension nor a live group refuses the whole call (UnknownTarget).
  [[nodiscard]] std::expected<BindTarget, ViewError>
  createGroup(std::string_view name, std::span<const BindTarget> members);
  AxisResult renameGroup(BindTarget group, std::string_view name);
  /// GroupCycle when @p member is @p group or contains it.
  AxisResult insertMember(BindTarget group, std::size_t position,
                          BindTarget member);
  AxisResult removeMember(BindTarget group, std::size_t position);
  AxisResult moveMember(BindTarget group, std::size_t from, std::size_t to);
  /// Removes every occurrence of @p group, from axes and parent groups; call
  /// forEachAxisShowing() and forEachGroupContaining() first to say which.
  AxisResult deleteGroup(BindTarget group);
  [[nodiscard]] std::size_t memberCount(BindTarget group) const noexcept;
  [[nodiscard]] std::optional<BindTarget>
  member(BindTarget group, std::size_t position) const noexcept;
  [[nodiscard]] std::optional<std::string> groupName(BindTarget group) const;
  /// Every live group, in the order they were made.
  void
  forEachGroup(gleditor::cpp26::function_ref<void(BindTarget)> visit) const;
  /// The real dimensions under @p target, depth first in member order: the
  /// dimension itself, or a group's leaves. A dimension reached by two
  /// routes is visited twice, because each route is a lane (§9.3.2).
  void
  forEachLeaf(BindTarget target,
              gleditor::cpp26::function_ref<void(zigzag::DimRef)> visit) const;

  // -- ring order (§9.2; G11) ----------------------------------------------

  /**
   * @brief The dimension's place in the ring order, appending it if new.
   *
   * The order begins as the slice's own (its d.dims rank) and is the
   * reader's from then on (G11). The one mutator a view calls for itself,
   * from prepare(); it pushes no undo entry.
   */
  [[nodiscard]] std::expected<std::size_t, ViewError>
  ringPlace(zigzag::DimRef dimension);
  [[nodiscard]] std::optional<std::size_t>
  ringPlaceIfKnown(zigzag::DimRef dimension) const;
  AxisResult moveInRing(zigzag::DimRef dimension, std::size_t place);
  void forEachInRing(
      gleditor::cpp26::function_ref<void(zigzag::DimRef)> visit) const;

  // -- the pouch (§7.7) ----------------------------------------------------

  AxisResult addToPouch(zigzag::DimRef dimension);
  AxisResult removeFromPouch(zigzag::DimRef dimension);
  [[nodiscard]] bool inPouch(zigzag::DimRef dimension) const noexcept;
  void forEachInPouch(
      gleditor::cpp26::function_ref<void(zigzag::DimRef)> visit) const;

  // -- who uses what: scans of a handful of cells --------------------------

  void forEachAxisShowing(
      BindTarget target,
      gleditor::cpp26::function_ref<void(ViewAxisId)> visit) const;
  void forEachGroupContaining(
      BindTarget target,
      gleditor::cpp26::function_ref<void(BindTarget)> visit) const;

  // -- undo: local to the placement, never hypertime -----------------------

  bool undo();
  bool redo();
  [[nodiscard]] bool canUndo() const noexcept { return !undo_.empty(); }
  [[nodiscard]] bool canRedo() const noexcept { return !redo_.empty(); }
  /// Forget every undo and redo entry, as configuring or replaying does.
  ViewAxisSet *forgetHistory() noexcept;

private:
  friend class ViewManifold;
  friend std::size_t verifyViewSpace(
      const ViewManifold &space,
      gleditor::cpp26::function_ref<void(const ViewSpaceViolation &)> report);

  /// The view-owned dimensions and heads, minted together on first edit.
  struct Frame {
    zigzag::CellRef axes, roles, binds, groups, members, ring, pouch;
    zigzag::CellRef axesHead, groupsHead, ringHead, pouchHead;
  };

  // One reversible change. An entry is the changes one edit made, applied
  // forward by redo() and backward, in reverse, by undo().
  struct LinkStep {
    zigzag::CellRef from, dim;
    zigzag::DimVector dir;
    zigzag::CellRef to;
  };
  struct UnlinkStep {
    zigzag::CellRef from, dim;
    zigzag::DimVector dir;
    zigzag::CellRef to; ///< what was there, so undo can put it back
  };
  struct TextStep {
    zigzag::CellRef cell;
    std::string before, after;
  };
  /// A ring move is kept as places rather than links: ringPlace() appends
  /// at the tail between edits, which would make a recorded link stale.
  struct RingStep {
    zigzag::DimRef dimension;
    std::size_t from, to;
  };
  using Step  = std::variant<LinkStep, UnlinkStep, TextStep, RingStep>;
  using Entry = std::vector<Step>;
  class Edit;

  /// Where a rank holds an occurrence of a target, and the occurrence.
  struct Found {
    zigzag::CellRef cell;
    std::size_t place;
  };

  [[nodiscard]] std::expected<const Frame *, ViewError> ensureFrame();
  [[nodiscard]] static ViewCellRef cell(zigzag::CellRef ref) noexcept;
  [[nodiscard]] std::optional<zigzag::CellRef>
  next(zigzag::CellRef from, zigzag::CellRef dim,
       zigzag::DimVector dir = zigzag::DimVector::POS) const noexcept;
  [[nodiscard]] std::optional<zigzag::CellRef>
  targetOf(zigzag::CellRef occurrence) const noexcept;
  [[nodiscard]] std::optional<std::string> textOf(zigzag::CellRef ref) const;
  /// Visit the cells after @p head on @p dim until @p visit answers false.
  /// Bounded by the arena's size, so a rank damaged into a loop still ends.
  void walk(zigzag::CellRef head, zigzag::CellRef dim,
            gleditor::cpp26::function_ref<bool(zigzag::CellRef)> visit) const;
  [[nodiscard]] std::optional<zigzag::CellRef>
  rankAt(zigzag::CellRef head, zigzag::CellRef dim, std::size_t position) const;
  [[nodiscard]] std::size_t rankSize(zigzag::CellRef head,
                                     zigzag::CellRef dim) const;
  [[nodiscard]] zigzag::CellRef tailOf(zigzag::CellRef head,
                                       zigzag::CellRef dim) const;
  [[nodiscard]] std::optional<Found>
  findOccurrence(zigzag::CellRef head, zigzag::CellRef dim,
                 zigzag::CellRef target) const;
  [[nodiscard]] std::optional<zigzag::CellRef> slotAt(ViewAxisId axis) const;
  /// @p target as the base's own name for a dimension cell, if it is one.
  [[nodiscard]] std::optional<zigzag::DimRef>
  realDimension(zigzag::CellRef target) const;
  /// @p target as something a slot or a group may show: a real dimension
  /// or a live group.
  [[nodiscard]] std::optional<BindTarget> bindable(BindTarget target) const;
  /// Whether @p inner is reachable from @p outer through group members.
  [[nodiscard]] bool reaches(BindTarget outer, BindTarget inner) const;
  [[nodiscard]] bool hasLeaf(BindTarget target) const;
  [[nodiscard]] std::size_t groupCount() const;
  /// Put the ring's occurrence of @p dimension at @p place, recording
  /// nothing: the caller records the move as places.
  [[nodiscard]] std::expected<void, ViewError>
  relinkRing(zigzag::DimRef dimension, std::size_t place);
  AxisResult finish(Edit &edit, std::expected<void, ViewError> outcome);
  [[nodiscard]] std::expected<void, ViewError> apply(const Step &step,
                                                     bool forward);

  /// The checks verifyViewSpace() makes of the binding model's ranks.
  std::size_t
  verify(gleditor::cpp26::function_ref<void(const ViewSpaceViolation &)> report)
      const;

  ViewManifold *space_;
  std::optional<Frame> frame_;
  std::vector<Entry> undo_;
  std::vector<Entry> redo_;
};

// -- persistence by name (§7.5; G9) -----------------------------------------

/// A dimension, by its name: refs do not survive a session.
struct SavedDimension {
  std::string name;
  bool operator==(const SavedDimension &) const = default;
};
/// A group, by its place in SavedBindings::groups: two groups may share a
/// name, and a place cannot be confused with a dimension's name.
struct SavedGroupRef {
  std::size_t index{};
  bool operator==(const SavedGroupRef &) const = default;
};
using SavedTarget = std::variant<SavedDimension, SavedGroupRef>;

struct SavedAxis {
  std::string point;
  std::optional<SavedTarget> shows;
  bool operator==(const SavedAxis &) const = default;
};
struct SavedGroup {
  std::string name;
  std::vector<SavedTarget> members;
  bool operator==(const SavedGroup &) const = default;
};

/// What layout.slice.<sliceId>.axes, .groups and .ringOrder, and the slice's
/// pouch in system://pouches, hold; the host writes and reads those keys.
struct SavedBindings {
  std::vector<SavedAxis> axes;
  std::vector<SavedGroup> groups;
  std::vector<std::string> ringOrder;
  std::vector<std::string> pouch;
  bool operator==(const SavedBindings &) const = default;
};

/// Dimension names are read from the base through @p reader; group names
/// from the binding arena.
[[nodiscard]] SavedBindings saveBindings(const ViewManifold &space,
                                         const xanadu::SpanReader &reader);

/// One name replay could not resolve: @p message's {name} is @p name, and
/// its {axis}, {group} or {where} is @p where.
struct ReplayNotice {
  ViewMessage message{ViewMessage::SavedNameGone};
  std::string name;
  std::string where;
  bool operator==(const ReplayNotice &) const = default;
};

/// G9: one summary, plus one notice per name that no longer resolves.
struct ReplayReport {
  ViewMessage summary{ViewMessage::BindingsReplayed};
  std::size_t restored{}; ///< axes bound and groups made
  std::vector<ReplayNotice> unresolved;
};

/**
 * @brief Replay @p saved through @p space's axes, resolving names in its
 *        base.
 *
 * Groups are made leaves first, so a parent saved before its child still
 * finds it; a member that would close a cycle is skipped and reported, as is
 * every name that no longer resolves. Then axes, the ring order and the
 * pouch. The history is forgotten afterwards: restoring is not an edit.
 */
ReplayReport replayBindings(ViewManifold &space, const SavedBindings &saved,
                            const xanadu::SpanReader &reader);

} // namespace xanadu::view

#endif // COMMON_XANADU_VIEW_VIEW_BINDING_HPP
