/**
 * @file view_manifold.hpp
 * @brief Two-layered sibling arena structure (binding and derived) over a base
 * ZigZag manifold.
 *
 * Implements the isolated ViewManifold architecture defined in
 * design/view-system.md:
 * - binding_: persistent layout topology (axes, occurrences, dimension groups).
 * - derived_: ephemeral scratchpad (packs, filtered/sorted ranks) tossed in
 * O(1).
 * - Guarantees zero base cell shadowing and strict epoch validation.
 */
#ifndef XANADU_VIEW_VIEW_MANIFOLD_HPP
#define XANADU_VIEW_VIEW_MANIFOLD_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

#include "common/xanadu/ops.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu::view {

enum class Layer { Binding, Derived };

enum class ViewError {
  StaleEpoch,
  RealCellInViewLink,
  OccupiedDirection,
  CellNotFound,
  InvalidDimension
};

struct ViewCellRef {
  std::optional<zigzag::CellRef> ref{std::nullopt};
  std::uint32_t epoch{0};

  constexpr bool operator==(const ViewCellRef &) const = default;
  [[nodiscard]] constexpr bool valid() const noexcept {
    return ref.has_value();
  }
};

class ViewManifold;
using ViewResult = std::expected<ViewManifold *, ViewError>;

/**
 * @class ViewManifold
 * @brief Two sibling arenas over one read-only base manifold.
 *
 * Implements Section 6 of design/view-system.md:
 * - binding_: Holds axes, occurrences, dimension groups, ring order.
 * - derived_: Holds ephemeral derived cells (packs, filtered/sorted ranks).
 *             Tossed in constant time via toss().
 * - Real cells are NEVER shadowed in either arena.
 */
class ViewManifold {
public:
  explicit ViewManifold(const zigzag::Manifold &base);
  explicit ViewManifold(const zigzag::ArenaManifold &baseArena);

  [[nodiscard]] const zigzag::Manifold *baseManifold() const noexcept {
    return base_;
  }
  [[nodiscard]] const zigzag::ArenaManifold *baseArena() const noexcept {
    return baseArena_;
  }

  [[nodiscard]] std::optional<zigzag::CellRef>
  baseLinked(zigzag::CellRef cell, zigzag::DimRef dim,
             zigzag::DimVector dir) const noexcept;

  [[nodiscard]] std::optional<std::int64_t>
  baseAsInt64(zigzag::CellRef cell) const noexcept;
  [[nodiscard]] zigzag::ArenaManifold &bindingArena() noexcept {
    return binding_;
  }
  [[nodiscard]] const zigzag::ArenaManifold &bindingArena() const noexcept {
    return binding_;
  }
  [[nodiscard]] zigzag::ArenaManifold &derivedArena() noexcept {
    return derived_;
  }
  [[nodiscard]] const zigzag::ArenaManifold &derivedArena() const noexcept {
    return derived_;
  }

  [[nodiscard]] std::uint32_t epoch() const noexcept { return epoch_; }

  /// Discards the derived arena in O(1) time without touching the base or
  /// binding arena.
  ViewManifold *toss() noexcept;

  /// Mint a bare view cell in the given layer.
  [[nodiscard]] ViewCellRef mint(Layer layer, std::string_view name = "");

  /// Mint an occurrence cell standing for real cell @p target.
  [[nodiscard]] ViewCellRef mintOccurrence(Layer layer, zigzag::CellRef target);

  /// Resolves an occurrence or view cell to the real base cell it represents.
  [[nodiscard]] std::optional<zigzag::CellRef>
  resolveReal(ViewCellRef vref) const noexcept;
  [[nodiscard]] std::optional<zigzag::CellRef>
  resolveReal(std::optional<zigzag::CellRef> ref) const noexcept;
  [[nodiscard]] std::optional<zigzag::CellRef>
  resolveReal(zigzag::CellRef ref) const noexcept;

  /// Link two view cells along a view dimension in the given layer.
  [[nodiscard]] ViewResult link(Layer layer, ViewCellRef from, ViewCellRef dim,
                                zigzag::DimVector dir,
                                std::optional<ViewCellRef> to) noexcept;

  /// Query neighbour along view dimension in given layer.
  [[nodiscard]] std::optional<ViewCellRef>
  linked(Layer layer, ViewCellRef cell, ViewCellRef dim,
         zigzag::DimVector dir) const noexcept;

  /// Number of base cells shadowed (must remain 0).
  [[nodiscard]] std::size_t shadowCount(Layer layer) const noexcept;

  /// Invariant verifier.
  [[nodiscard]] bool verifyViewSpace() const noexcept;

private:
  const zigzag::Manifold *base_{nullptr};
  const zigzag::ArenaManifold *baseArena_{nullptr};
  zigzag::ArenaManifold binding_;
  zigzag::ArenaManifold derived_;
  zigzag::Mark emptyDerivedMark_{};
  std::uint32_t epoch_{1};
};

} // namespace xanadu::view

#endif // XANADU_VIEW_VIEW_MANIFOLD_HPP
