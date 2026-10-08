#include <gleditor/ui/pane_tree.hpp>

#include <gleditor/ui/metrics.hpp>

#include <algorithm>
#include <cmath>

namespace gleditor::ui {
namespace {

std::optional<float> clampedShare(const float share) {
  if (!std::isfinite(share)) {
    return std::nullopt;
  }
  return std::clamp(share, 0.0F, 1.0F);
}

} // namespace

PaneTree::PaneTree() : nodes_{Node{.id = 0}}, order_{0}, nextPane_{1} {}

bool PaneTree::contains(const PaneId pane) const noexcept {
  return std::ranges::find(order_, pane) != order_.end();
}

std::optional<PaneTree::NodeIndex>
PaneTree::findLeaf(const PaneId pane) const noexcept {
  for (NodeIndex index = 0; index < nodes_.size(); ++index) {
    const auto &node = nodes_[index];
    if (node.live && node.leaf && node.id == pane) {
      return index;
    }
  }
  return std::nullopt;
}

std::optional<PaneTree::NodeIndex>
PaneTree::findDivider(const DividerId divider) const noexcept {
  for (NodeIndex index = 0; index < nodes_.size(); ++index) {
    const auto &node = nodes_[index];
    if (node.live && !node.leaf && node.id == divider) {
      return index;
    }
  }
  return std::nullopt;
}

PaneTree::NodeIndex PaneTree::allocate(const Node &node) {
  if (free_.empty()) {
    nodes_.push_back(node);
    return nodes_.size() - 1;
  }
  const NodeIndex index = free_.back();
  free_.pop_back();
  nodes_[index] = node;
  return index;
}

void PaneTree::release(const NodeIndex index) {
  nodes_[index].live = false;
  free_.push_back(index);
}

void PaneTree::replaceChild(const std::optional<NodeIndex> parent,
                            const NodeIndex from, const NodeIndex to) {
  nodes_[to].parent = parent;
  if (!parent) {
    root_ = to;
    return;
  }
  auto &children = nodes_[*parent].children;
  std::ranges::replace(children, from, to);
}

std::expected<PaneId, PaneError>
PaneTree::split(const PaneId pane, const Axis axis, const float firstShare) {
  const auto leaf = findLeaf(pane);
  if (!leaf) {
    return std::unexpected(PaneError::UnknownPane);
  }
  const auto share = clampedShare(firstShare);
  if (!share) {
    return std::unexpected(PaneError::InvalidShare);
  }
  const PaneId added      = nextPane_++;
  const NodeIndex sibling = allocate(Node{.id = added});
  const NodeIndex divider = allocate(Node{.id         = nextDivider_++,
                                          .leaf       = false,
                                          .axis       = axis,
                                          .firstShare = *share,
                                          .children   = {*leaf, sibling}});
  replaceChild(nodes_[*leaf].parent, *leaf, divider);
  nodes_[*leaf].parent   = divider;
  nodes_[sibling].parent = divider;
  order_.insert(std::ranges::find(order_, pane) + 1, added);
  return added;
}

std::expected<PaneTree *, PaneError> PaneTree::close(const PaneId pane) {
  const auto leaf = findLeaf(pane);
  if (!leaf) {
    return std::unexpected(PaneError::UnknownPane);
  }
  const auto divider = nodes_[*leaf].parent;
  if (!divider) {
    return std::unexpected(PaneError::LastPane);
  }
  const auto &children    = nodes_[*divider].children;
  const NodeIndex sibling = children[0] == *leaf ? children[1] : children[0];
  replaceChild(nodes_[*divider].parent, *divider, sibling);
  release(*leaf);
  release(*divider);
  std::erase(order_, pane);
  return this;
}

std::expected<PaneTree *, PaneError> PaneTree::resize(const PaneId pane,
                                                      const float share) {
  const auto leaf = findLeaf(pane);
  if (!leaf) {
    return std::unexpected(PaneError::UnknownPane);
  }
  const auto divider = nodes_[*leaf].parent;
  if (!divider) {
    return std::unexpected(PaneError::NoDivider);
  }
  const auto clamped = clampedShare(share);
  if (!clamped) {
    return std::unexpected(PaneError::InvalidShare);
  }
  auto &node      = nodes_[*divider];
  node.firstShare = node.children[0] == *leaf ? *clamped : 1.0F - *clamped;
  return this;
}

std::expected<PaneTree *, PaneError>
PaneTree::resizeDivider(const DividerId divider, const float firstShare) {
  const auto index = findDivider(divider);
  if (!index) {
    return std::unexpected(PaneError::UnknownDivider);
  }
  const auto clamped = clampedShare(firstShare);
  if (!clamped) {
    return std::unexpected(PaneError::InvalidShare);
  }
  nodes_[*index].firstShare = *clamped;
  return this;
}

void PaneTree::walk(
    const NodeIndex index, const Rect bounds,
    const cpp26::function_ref<void(PaneId, Rect)> pane,
    const cpp26::function_ref<void(const Divider &)> divider) const {
  const auto &node = nodes_[index];
  if (node.leaf) {
    pane(node.id, bounds);
    return;
  }
  const auto halves =
      ui::split(bounds, LayoutItem{.id = 0}, LayoutItem{.id = 1},
                SplitOptions{.axis = node.axis, .firstShare = node.firstShare});
  const Rect first  = halves.boxes[0].rect;
  const Rect second = halves.boxes[1].rect;
  const Rect edge   = node.axis == Axis::Horizontal
                          ? Rect{second.left, bounds.bottom, 0.0F, bounds.height}
                          : Rect{bounds.left, first.bottom, bounds.width, 0.0F};
  divider(
      Divider{.id = node.id, .axis = node.axis, .span = bounds, .edge = edge});
  walk(node.children[0], first, pane, divider);
  walk(node.children[1], second, pane, divider);
}

void PaneTree::rects(
    const Rect bounds,
    const cpp26::function_ref<void(PaneId, Rect)> visit) const {
  walk(root_, UiMetrics{}.rounded(bounds), visit, [](const Divider &) {});
}

void PaneTree::dividers(
    const Rect bounds,
    const cpp26::function_ref<void(const Divider &)> visit) const {
  walk(root_, UiMetrics{}.rounded(bounds), [](PaneId, Rect) {}, visit);
}

} // namespace gleditor::ui
