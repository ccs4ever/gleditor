#include "common/xanadu/view/view_manifold.hpp"

namespace xanadu::view {

ViewManifold::ViewManifold(const zigzag::Manifold &base)
    : base_(&base), baseArena_(nullptr), binding_(&base, nullptr),
      derived_(&base, nullptr) {
  emptyDerivedMark_ = derived_.mark();
}

ViewManifold::ViewManifold(const zigzag::ArenaManifold &baseArena)
    : base_(baseArena.base()), baseArena_(&baseArena),
      binding_(baseArena.base(), nullptr), derived_(baseArena.base(), nullptr) {
  emptyDerivedMark_ = derived_.mark();
}

std::optional<zigzag::CellRef>
ViewManifold::baseLinked(zigzag::CellRef cell, zigzag::DimRef dim,
                         zigzag::DimVector dir) const noexcept {
  if (cell == zigzag::noCell || dim == zigzag::noCell) {
    return std::nullopt;
  }
  if (baseArena_) {
    const auto res = baseArena_->linked(cell, dim, dir);
    if (!res) {
      return std::nullopt;
    }
    return *res;
  }
  if (base_) {
    const auto res = base_->linked(cell, dim, dir);
    if (!res) {
      return std::nullopt;
    }
    return *res;
  }
  return std::nullopt;
}

std::optional<std::int64_t>
ViewManifold::baseAsInt64(zigzag::CellRef cell) const noexcept {
  if (baseArena_) {
    return baseArena_->asInt64(cell);
  }
  if (base_) {
    return base_->asInt64(cell);
  }
  return std::nullopt;
}

ViewManifold *ViewManifold::toss() noexcept {
  derived_.release(emptyDerivedMark_);
  emptyDerivedMark_ = derived_.mark();
  ++epoch_;
  return this;
}

ViewCellRef ViewManifold::mint(Layer layer, std::string_view name) {
  auto &arena    = (layer == Layer::Binding) ? binding_ : derived_;
  const auto ref = arena.makeCell(name);
  return ViewCellRef{.ref   = ref,
                     .epoch = (layer == Layer::Binding ? 0U : epoch_)};
}

ViewCellRef ViewManifold::mintOccurrence(Layer layer, zigzag::CellRef target) {
  auto &arena    = (layer == Layer::Binding) ? binding_ : derived_;
  const auto ref = arena.makeCell();
  (void)arena.setValueBits(ref, xanadu::ValueKind::OpHandle,
                           static_cast<std::uint64_t>(target));
  return ViewCellRef{.ref   = ref,
                     .epoch = (layer == Layer::Binding ? 0U : epoch_)};
}

std::optional<zigzag::CellRef>
ViewManifold::resolveReal(ViewCellRef vref) const noexcept {
  return resolveReal(vref.ref);
}

std::optional<zigzag::CellRef>
ViewManifold::resolveReal(std::optional<zigzag::CellRef> ref) const noexcept {
  if (!ref.has_value()) {
    return std::nullopt;
  }
  return resolveReal(*ref);
}

std::optional<zigzag::CellRef>
ViewManifold::resolveReal(zigzag::CellRef ref) const noexcept {
  if (ref == zigzag::noCell) {
    return std::nullopt;
  }
  // If not ephemeral, it is already a real base cell.
  if ((ref & zigzag::ephemeralBit) == 0) {
    return ref;
  }

  // Check derived arena first, then binding arena.
  if (derived_.holdsOwn(ref)) {
    const auto target = derived_.handleTarget(ref);
    if (target.has_value()) {
      return *target;
    }
    return ref;
  }
  if (binding_.holdsOwn(ref)) {
    const auto target = binding_.handleTarget(ref);
    if (target.has_value()) {
      return *target;
    }
    return ref;
  }
  return ref;
}

ViewResult ViewManifold::link(Layer layer, ViewCellRef from, ViewCellRef dim,
                              zigzag::DimVector dir,
                              std::optional<ViewCellRef> to) noexcept {
  if (layer == Layer::Derived) {
    if (from.epoch != epoch_ || dim.epoch != epoch_ ||
        (to.has_value() && to->epoch != epoch_)) {
      return std::unexpected(ViewError::StaleEpoch);
    }
  }

  if (!from.valid() || !dim.valid() || (to.has_value() && !to->valid())) {
    return std::unexpected(ViewError::CellNotFound);
  }

  // Invariant I3: No real cell in view link. Both ends and dimension must be
  // view cells.
  if ((*from.ref & zigzag::ephemeralBit) == 0 ||
      (*dim.ref & zigzag::ephemeralBit) == 0 ||
      (to.has_value() && (*to->ref & zigzag::ephemeralBit) == 0)) {
    return std::unexpected(ViewError::RealCellInViewLink);
  }

  auto &arena = (layer == Layer::Binding) ? binding_ : derived_;

  // Invariant I1: Refuse if direction occupied.
  const auto existing = arena.linked(*from.ref, *dim.ref, dir);
  if (existing.has_value() && to.has_value()) {
    return std::unexpected(ViewError::OccupiedDirection);
  }

  if (to.has_value()) {
    const auto oppExisting = arena.linked(*to->ref, *dim.ref, -dir);
    if (oppExisting.has_value()) {
      return std::unexpected(ViewError::OccupiedDirection);
    }
    (void)arena.link(*from.ref, *dim.ref, dir, *to->ref);
  } else {
    (void)arena.unlink(*from.ref, *dim.ref, dir);
  }

  return this;
}

std::optional<ViewCellRef>
ViewManifold::linked(Layer layer, ViewCellRef cell, ViewCellRef dim,
                     zigzag::DimVector dir) const noexcept {
  if (layer == Layer::Derived &&
      (cell.epoch != epoch_ || dim.epoch != epoch_)) {
    return std::nullopt;
  }
  if (!cell.valid() || !dim.valid()) {
    return std::nullopt;
  }
  const auto &arena = (layer == Layer::Binding) ? binding_ : derived_;
  const auto n      = arena.linked(*cell.ref, *dim.ref, dir);
  if (!n.has_value()) {
    return std::nullopt;
  }
  return ViewCellRef{.ref   = *n,
                     .epoch = (layer == Layer::Binding ? 0U : epoch_)};
}

std::size_t ViewManifold::shadowCount(Layer layer) const noexcept {
  const auto &arena = (layer == Layer::Binding) ? binding_ : derived_;
  return arena.shadowCount();
}

bool ViewManifold::verifyViewSpace() const noexcept {
  // Invariant I3: Real cells must never be shadowed.
  if (binding_.shadowCount() != 0 || derived_.shadowCount() != 0) {
    return false;
  }
  return true;
}

} // namespace xanadu::view
