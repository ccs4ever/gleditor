/**
 * @file pane_tree.hpp
 * @brief A window divided into panes by a tree of two-way splits.
 */
#ifndef GLEDITOR_UI_PANE_TREE_HPP
#define GLEDITOR_UI_PANE_TREE_HPP

#include <gleditor/cpp26.hpp>
#include <gleditor/ui/layout.hpp>
#include <gleditor/ui/rect.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace gleditor::ui {

using PaneId    = std::uint32_t;
using DividerId = std::uint32_t;

/// Why a change to a pane tree was refused.
enum class PaneError : unsigned char {
  UnknownPane,    ///< No open pane has that id; ids are never reused.
  UnknownDivider, ///< No divider has that id; ids are never reused.
  LastPane,       ///< The tree always keeps one pane.
  NoDivider,      ///< The only pane has no divider to move.
  InvalidShare,   ///< A share that is not a finite number.
};

/// One divider as laid out: the rectangle it divides and the edge itself.
struct Divider {
  DividerId id{};
  /// Horizontal puts the first side on the left, Vertical on top, as
  /// ui::split() does.
  Axis axis{Axis::Horizontal};
  /// What the divider divides. A share is measured across it from the first
  /// side, so a host turns a drag into a share with this alone.
  Rect span;
  /// The shared edge, as a rectangle of zero width or height.
  Rect edge;
};

/**
 * @brief Splits, closes and resizes rectangular panes, and keeps their order.
 *
 * Every split is in two, so a pane is a leaf and a divider an inner node. The
 * tree draws nothing and knows nothing of focus: a host registers each pane
 * with FocusManager::addPane() and turns each rectangle into whatever it
 * draws through. Rectangles come from ui::split(), so they are edge-rounded
 * and neighbours share an edge exactly.
 *
 * Panes are in reading order: a split's first side before its second, all the
 * way down. That is the focus order, and resizing never changes it.
 */
class PaneTree {
public:
  /// One pane, filling whatever bounds it is given.
  PaneTree();

  /**
   * @brief Divide @p pane in two along @p axis.
   *
   * The pane keeps the first side, @p firstShare of the room (clamped to
   * [0, 1]), and a new pane takes the second; it follows @p pane in order.
   *
   * @return The new pane.
   */
  [[nodiscard]] std::expected<PaneId, PaneError> split(PaneId pane, Axis axis,
                                                       float firstShare = 0.5F);

  /// Close @p pane; its sibling, pane or subtree, takes the room they shared.
  std::expected<PaneTree *, PaneError> close(PaneId pane);

  /**
   * @brief Give @p pane @p share (clamped to [0, 1]) of the divider that
   *        made it.
   *
   * The common case, a pane growing against its neighbour. A divider between
   * two subtrees is not next to any one pane; resizeDivider() reaches it.
   */
  std::expected<PaneTree *, PaneError> resize(PaneId pane, float share);

  /// Move a divider: its first side gets @p firstShare (clamped to [0, 1]).
  std::expected<PaneTree *, PaneError> resizeDivider(DividerId divider,
                                                     float firstShare);

  /// Each pane's rectangle within @p bounds, in order.
  void rects(Rect bounds, cpp26::function_ref<void(PaneId, Rect)> visit) const;

  /// Each divider within @p bounds, outer before inner.
  void dividers(Rect bounds,
                cpp26::function_ref<void(const Divider &)> visit) const;

  /// The panes, in focus order. Never empty.
  [[nodiscard]] std::span<const PaneId> order() const noexcept {
    return order_;
  }

  [[nodiscard]] bool contains(PaneId pane) const noexcept;

private:
  using NodeIndex = std::size_t;

  struct Node {
    /// A PaneId for a leaf, a DividerId otherwise.
    std::uint32_t id{};
    bool leaf{true};
    Axis axis{Axis::Horizontal};
    float firstShare{0.5F};
    std::array<NodeIndex, 2> children{};
    std::optional<NodeIndex> parent;
    /// False once closed, until the slot is reused.
    bool live{true};
  };

  [[nodiscard]] std::optional<NodeIndex> findLeaf(PaneId pane) const noexcept;
  [[nodiscard]] std::optional<NodeIndex>
  findDivider(DividerId divider) const noexcept;
  NodeIndex allocate(const Node &node);
  void release(NodeIndex index);
  /// Point whatever held @p from (its parent, or the root) at @p to.
  void replaceChild(std::optional<NodeIndex> parent, NodeIndex from,
                    NodeIndex to);
  void walk(NodeIndex index, Rect bounds,
            cpp26::function_ref<void(PaneId, Rect)> pane,
            cpp26::function_ref<void(const Divider &)> divider) const;

  std::vector<Node> nodes_;
  /// Slots of closed panes and their dividers, reused before the vector grows.
  std::vector<NodeIndex> free_;
  NodeIndex root_{};
  std::vector<PaneId> order_;
  PaneId nextPane_{};
  DividerId nextDivider_{};
};

} // namespace gleditor::ui

#endif // GLEDITOR_UI_PANE_TREE_HPP
