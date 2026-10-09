/**
 * @file view_manifold.cpp
 * @brief The view space's one write path and its verifier
 *        (design/view-system.md §6).
 */
#include "common/xanadu/view/view_manifold.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "common/xanadu/ops.hpp"
#include "common/xanadu/view/view_binding.hpp"

namespace xanadu::view {

namespace {

constexpr std::array<zigzag::DimVector, 2> bothWays{zigzag::DimVector::POS,
                                                    zigzag::DimVector::NEG};

} // namespace

ViewManifold::ViewManifold(const zigzag::Manifold &base)
    // No store, so that no provenance cells are projected into either arena:
    // a view arena holds what a view minted and nothing else (§6.1).
    : base_(base), bindings_(&base, nullptr), derived_(&base, nullptr),
      empty_(derived_.mark()), axes_(std::make_unique<ViewAxisSet>(*this)) {}

ViewManifold::ViewManifold(ViewManifold &&other) noexcept
    : base_(other.base_), bindings_(std::move(other.bindings_)),
      derived_(std::move(other.derived_)), empty_(other.empty_),
      epoch_(other.epoch_), packDim_(other.packDim_),
      packingDim_(other.packingDim_),
      axisStepDims_(std::move(other.axisStepDims_)),
      axes_(std::move(other.axes_)) {
  axes_->space_ = this;
}

ViewManifold::~ViewManifold() = default;

bool ViewManifold::isCurrent(const ViewCellRef cell) const noexcept {
  return cell.epoch == epochOf(cell.layer);
}

bool ViewManifold::isViewCell(const Layer layer,
                              const zigzag::CellRef ref) const noexcept {
  // denseOf() alone would also answer for a shadowed real cell, which keeps
  // its base ref; the ephemeral bit is what says the arena minted it.
  return zigzag::isEphemeral(ref) && arena(layer).denseOf(ref).has_value();
}

std::expected<void, ViewError>
ViewManifold::check(const Layer layer, const ViewCellRef cell) const noexcept {
  if (!isCurrent(cell)) {
    return std::unexpected{ViewError::StaleEpoch};
  }
  if (cell.layer != layer || !isViewCell(layer, cell.ref)) {
    return std::unexpected{ViewError::RealCellInViewLink};
  }
  return {};
}

std::optional<zigzag::CellRef>
ViewManifold::realCell(const zigzag::CellRef target) const noexcept {
  if (zigzag::isEphemeral(target)) {
    return std::nullopt;
  }
  const auto slot = base_.slot(target);
  if (!slot) {
    return std::nullopt;
  }
  // A cell's micro-history chain resolves to it from any of its operations;
  // the handle keeps the cell's own name so that occurrences of one cell are
  // equal values, which is what matching occurrences compares (plan G7).
  return slot->birthOp;
}

std::expected<ViewCellRef, ViewError> ViewManifold::mint(const Layer layer) {
  try {
    return wrap(layer, arena(layer).makeCell());
  } catch (const std::runtime_error &) {
    return std::unexpected{ViewError::ArenaRefused};
  }
}

std::expected<ViewCellRef, ViewError>
ViewManifold::mint(const Layer layer, const std::string_view text) {
  try {
    return wrap(layer, arena(layer).makeCell(text));
  } catch (const std::runtime_error &) {
    return std::unexpected{ViewError::ArenaRefused};
  }
}

std::expected<ViewCellRef, ViewError>
ViewManifold::mintOccurrence(const Layer layer, const zigzag::CellRef target) {
  auto handle = realCell(target);
  if (!handle.has_value() && Layer::Binding == layer &&
      axes_->isGroup(target)) {
    handle = target;
  }
  if (!handle.has_value()) {
    return std::unexpected{ViewError::UnknownTarget};
  }
  return mint(layer).and_then(
      [&](const ViewCellRef cell) -> std::expected<ViewCellRef, ViewError> {
        // A cell minted a line above: the arena cannot refuse it, and writing
        // it shadows nothing.
        if (!arena(layer).setValueBits(cell.ref, xanadu::ValueKind::OpHandle,
                                       *handle)) {
          return std::unexpected{ViewError::ArenaRefused};
        }
        return cell;
      });
}

ViewResult ViewManifold::link(const Layer layer, const ViewCellRef from,
                              const ViewDim dim, const zigzag::DimVector dir,
                              const ViewCellRef to) noexcept {
  for (const auto &end : {from, dim, to}) {
    if (const auto ok = check(layer, end); !ok) {
      return std::unexpected{ok.error()};
    }
  }
  auto &space       = arena(layer);
  const auto mine   = space.linked(from.ref, dim.ref, dir);
  const auto theirs = space.linked(to.ref, dim.ref, -dir);
  if (mine.has_value() && *mine == to.ref) {
    return this; // already this edge: two-sided, so theirs is from
  }
  if (mine.has_value() || (theirs.has_value() && *theirs != from.ref)) {
    return std::unexpected{ViewError::OccupiedDirection};
  }
  if (!space.link(from.ref, dim.ref, dir, to.ref)) {
    return std::unexpected{ViewError::ArenaRefused};
  }
  return this;
}

ViewResult ViewManifold::unlink(const Layer layer, const ViewCellRef from,
                                const ViewDim dim,
                                const zigzag::DimVector dir) noexcept {
  for (const auto &end : {from, dim}) {
    if (const auto ok = check(layer, end); !ok) {
      return std::unexpected{ok.error()};
    }
  }
  auto &space = arena(layer);
  // Clearing an edge that is not there would still give the cell a link run
  // entry on the dimension, an empty one; nothing to clear writes nothing.
  if (!space.linked(from.ref, dim.ref, dir).has_value()) {
    return this;
  }
  if (!space.unlink(from.ref, dim.ref, dir)) {
    return std::unexpected{ViewError::ArenaRefused};
  }
  return this;
}

ViewResult ViewManifold::setText(const Layer layer, const ViewCellRef cell,
                                 const std::string_view text) {
  if (const auto ok = check(layer, cell); !ok) {
    return std::unexpected{ok.error()};
  }
  auto &space = arena(layer);
  try {
    const auto span = space.intern(text);
    if (!space.setContent(cell.ref, std::span{&span, 1})) {
      return std::unexpected{ViewError::ArenaRefused};
    }
  } catch (const std::runtime_error &) {
    return std::unexpected{ViewError::ArenaRefused};
  }
  return this;
}

std::optional<std::string> ViewManifold::text(const ViewCellRef cell) const {
  if (!check(cell.layer, cell)) {
    return std::nullopt;
  }
  return arena(cell.layer).textOf(cell.ref);
}

std::optional<ViewCellRef>
ViewManifold::linked(const ViewCellRef from, const ViewDim dim,
                     const zigzag::DimVector dir) const noexcept {
  if (!check(from.layer, from) || !check(from.layer, dim)) {
    return std::nullopt;
  }
  const auto found = arena(from.layer).linked(from.ref, dim.ref, dir);
  if (!found.has_value()) {
    return std::nullopt;
  }
  return wrap(from.layer, *found);
}

std::optional<zigzag::CellRef>
ViewManifold::target(const ViewCellRef occurrence) const noexcept {
  if (!check(occurrence.layer, occurrence)) {
    return std::nullopt;
  }
  return arena(occurrence.layer).handleTarget(occurrence.ref);
}

std::expected<zigzag::CellRef, ViewError>
ViewManifold::resolveReal(const ViewCellRef cell) const noexcept {
  if (const auto ok = check(cell.layer, cell); !ok) {
    return std::unexpected{ViewError::StaleEpoch == ok.error()
                               ? ok.error()
                               : ViewError::UnknownTarget};
  }
  // Each cell is visited at most once on an acyclic pack, so the arena's size
  // bounds the walk; a pack that loops runs out of budget instead of forever.
  std::size_t budget = arena(cell.layer).cellCount() + 1;
  return resolveIn(cell.layer, cell.ref, budget);
}

std::expected<zigzag::CellRef, ViewError>
ViewManifold::resolveIn(const Layer layer, const zigzag::CellRef cell,
                        std::size_t &budget) const noexcept {
  // A link to anything but a view cell of this arena is damage the verifier
  // reports; reading on would answer from the base.
  if (0 == budget || !isViewCell(layer, cell)) {
    return std::unexpected{ViewError::UnknownTarget};
  }
  --budget;
  const auto &space = arena(layer);
  auto group        = std::optional<zigzag::CellRef>{};
  if (const auto handle = space.handleTarget(cell); handle.has_value()) {
    if (const auto real = realCell(*handle); real.has_value()) {
      return *real;
    }
    if (Layer::Binding != layer || !axes_->isGroup(*handle)) {
      return std::unexpected{ViewError::UnknownTarget};
    }
    group = *handle;
  } else if (Layer::Binding == layer && axes_->isGroup(cell)) {
    group = cell;
  }
  if (group.has_value()) {
    // A group is a pack's rule before it is a pack (§9.3.2), so it resolves
    // as one: its first member that does.
    const auto members = axes_->frame_->members;
    for (auto member = space.linked(*group, members, zigzag::DimVector::POS);
         member.has_value() && budget > 0;
         member = space.linked(*member, members, zigzag::DimVector::POS)) {
      if (const auto real = resolveIn(layer, *member, budget);
          real.has_value()) {
        return real;
      }
    }
    return std::unexpected{ViewError::UnknownTarget};
  }
  if (Layer::Derived != layer || !packDim_.has_value() ||
      !packingDim_.has_value()) {
    return std::unexpected{ViewError::UnknownTarget};
  }
  auto constituent = space.linked(cell, *packDim_, zigzag::DimVector::POS);
  while (constituent.has_value() && budget > 0) {
    if (const auto real = resolveIn(layer, *constituent, budget);
        real.has_value()) {
      return real;
    }
    constituent =
        space.linked(*constituent, *packingDim_, zigzag::DimVector::POS);
  }
  return std::unexpected{ViewError::UnknownTarget};
}

ViewDim ViewManifold::derivedDim(std::optional<zigzag::CellRef> &slot,
                                 const std::string_view name) {
  if (!slot.has_value()) {
    slot = derived_.makeCell(name);
  }
  return wrap(Layer::Derived, *slot);
}

ViewDim ViewManifold::packDim() { return derivedDim(packDim_, "d.pack"); }

ViewDim ViewManifold::packingDim() {
  return derivedDim(packingDim_, "d.packing");
}

ViewDim ViewManifold::axisStepDim(const ViewAxisId axis) {
  if (axisStepDims_.size() <= axis) {
    axisStepDims_.resize(static_cast<std::size_t>(axis) + 1);
  }
  return derivedDim(axisStepDims_[axis], "d.axis-step");
}

std::optional<ViewDim> ViewManifold::findPackDim() const noexcept {
  return packDim_.has_value() ? std::optional{wrap(Layer::Derived, *packDim_)}
                              : std::nullopt;
}

std::optional<ViewDim> ViewManifold::findPackingDim() const noexcept {
  return packingDim_.has_value()
             ? std::optional{wrap(Layer::Derived, *packingDim_)}
             : std::nullopt;
}

std::optional<ViewDim>
ViewManifold::findAxisStepDim(const ViewAxisId axis) const noexcept {
  if (axisStepDims_.size() <= axis || !axisStepDims_[axis].has_value()) {
    return std::nullopt;
  }
  return wrap(Layer::Derived, *axisStepDims_[axis]);
}

std::optional<ViewCellRef>
ViewManifold::findOccurrence(const ViewDim dim,
                             const zigzag::CellRef target) const noexcept {
  if (!check(dim.layer, dim)) {
    return std::nullopt;
  }
  const auto own = realCell(target);
  if (!own.has_value()) {
    return std::nullopt;
  }
  const auto &space = arena(dim.layer);
  for (const auto &slot : space.cells()) {
    const auto ref = slot.birthOp;
    if (!zigzag::isEphemeral(ref) || space.handleTarget(ref) != own) {
      continue;
    }
    if (space.linked(ref, dim.ref, zigzag::DimVector::POS).has_value() ||
        space.linked(ref, dim.ref, zigzag::DimVector::NEG).has_value()) {
      return wrap(dim.layer, ref);
    }
  }
  return std::nullopt;
}

ViewManifold *ViewManifold::toss() noexcept {
  derived_.release(empty_);
  empty_ = derived_.mark();
  ++epoch_;
  packDim_.reset();
  packingDim_.reset();
  axisStepDims_.clear();
  return this;
}

namespace {

/// Walks one arena's cells for verifyViewSpace(), counting what it reports.
class ArenaVerifier {
public:
  ArenaVerifier(
      const zigzag::ArenaManifold &arena, const zigzag::Manifold &base,
      const Layer layer, const ViewEpoch epoch, const ViewAxisSet &axes,
      gleditor::cpp26::function_ref<void(const ViewSpaceViolation &)> report)
      : arena_(arena), base_(base), layer_(layer), epoch_(epoch), axes_(axes),
        report_(report) {}

  [[nodiscard]] std::size_t count() const noexcept { return count_; }

  void cells() {
    for (const auto &slot : arena_.cells()) {
      // birthOp is the slot's name: its own ephemeral ref for a cell the
      // arena minted, the base cell's ref for a shadow.
      const auto ref = slot.birthOp;
      if (!zigzag::isEphemeral(ref)) {
        flag(ref, "I3");
        continue;
      }
      for (const auto &edge : arena_.dimensionsOf(ref)) {
        if (!isViewCell(edge.dim)) {
          flag(ref, "foreign-key");
          continue;
        }
        for (const auto dir : bothWays) {
          const auto far = edge.neighbor(dir);
          if (zigzag::noCell == far) {
            continue;
          }
          if (!isViewCell(far)) {
            flag(ref, "foreign-end");
          } else if (arena_.linked(far, edge.dim, -dir) != ref) {
            flag(ref, "two-sided");
          }
        }
      }
      if (const auto handle = arena_.handleTarget(ref); handle.has_value()) {
        const bool real =
            !zigzag::isEphemeral(*handle) && base_.slot(*handle).has_value();
        const bool group = Layer::Binding == layer_ && isViewCell(*handle) &&
                           (axes_.isGroup(*handle) || !onAnyRank(ref));
        if (!real && !group) {
          flag(ref, "occurrence-target");
        }
      }
    }
  }

  /// A rank on @p dim that closes on itself. Two-sidedness gives every cell at
  /// most one negward neighbour, so a walk posward from each head reaches every
  /// cell on an open rank; a cell on the dimension that none reaches is on a
  /// loop.
  void acyclic(const std::optional<zigzag::CellRef> dim) {
    if (!dim.has_value()) {
      return;
    }
    const auto cells = arena_.cells();
    std::vector<bool> reached(cells.size(), false);
    for (const auto &slot : cells) {
      const auto head = slot.birthOp;
      if (!onDimension(head, *dim) ||
          arena_.linked(head, *dim, zigzag::DimVector::NEG).has_value()) {
        continue;
      }
      for (auto cur = std::optional<zigzag::CellRef>{head}; cur.has_value();
           cur      = arena_.linked(*cur, *dim, zigzag::DimVector::POS)) {
        const auto dense = arena_.denseOf(*cur);
        if (!dense.has_value() || reached[*dense]) {
          break;
        }
        reached[*dense] = true;
      }
    }
    for (std::size_t dense = 0; dense < cells.size(); ++dense) {
      const auto ref = cells[dense].birthOp;
      if (!reached[dense] && onDimension(ref, *dim)) {
        flag(ref, "rank-shape");
      }
    }
  }

  void arenaWide(const bool fails, const std::string_view rule) {
    if (fails) {
      report_(ViewSpaceViolation{
          .cell = std::nullopt, .layer = layer_, .rule = rule});
      ++count_;
    }
  }

private:
  [[nodiscard]] bool isViewCell(const zigzag::CellRef ref) const noexcept {
    return zigzag::isEphemeral(ref) && arena_.denseOf(ref).has_value();
  }

  [[nodiscard]] bool onAnyRank(const zigzag::CellRef ref) const noexcept {
    return std::ranges::any_of(arena_.dimensionsOf(ref), [](const auto &edge) {
      return zigzag::noCell != edge.pos || zigzag::noCell != edge.neg;
    });
  }

  [[nodiscard]] bool onDimension(const zigzag::CellRef ref,
                                 const zigzag::CellRef dim) const noexcept {
    return arena_.linked(ref, dim, zigzag::DimVector::POS).has_value() ||
           arena_.linked(ref, dim, zigzag::DimVector::NEG).has_value();
  }

  void flag(const zigzag::CellRef ref, const std::string_view rule) {
    report_(ViewSpaceViolation{
        .cell  = ViewCellRef{.ref = ref, .epoch = epoch_, .layer = layer_},
        .layer = layer_,
        .rule  = rule,
    });
    ++count_;
  }

  const zigzag::ArenaManifold &arena_;
  const zigzag::Manifold &base_;
  Layer layer_;
  ViewEpoch epoch_;
  const ViewAxisSet &axes_;
  gleditor::cpp26::function_ref<void(const ViewSpaceViolation &)> report_;
  std::size_t count_{0};
};

} // namespace

std::size_t verifyViewSpace(
    const ViewManifold &space,
    gleditor::cpp26::function_ref<void(const ViewSpaceViolation &)> report) {
  std::size_t total = 0;
  for (const auto layer : {Layer::Binding, Layer::Derived}) {
    ArenaVerifier verifier{space.arena(layer),   space.base(), layer,
                           space.epochOf(layer), *space.axes_, report};
    verifier.cells();
    if (Layer::Derived == layer) {
      verifier.acyclic(space.packDim_);
      verifier.acyclic(space.packingDim_);
      verifier.arenaWide(space.trailSize(layer) != 0, "I4");
      verifier.arenaWide(space.spaceCount(layer) != 0, "I4");
    }
    total += verifier.count();
  }
  return total + space.axes_->verify(report);
}

} // namespace xanadu::view
