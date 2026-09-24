/**
 * @file cell_views.hpp
 * @brief Lazy ranges and optional-returning steps over a manifold's cells.
 *
 * The read path of Manifold and ArenaManifold is a graph: a cell, its run of
 * DimLinks, a neighbour per side. Every traversal in the tree was spelled one
 * of two ways -- a `while (cur != noCell) { cur = m.linked(...); }` loop, or
 * walkRank() with a lambda whose bool return meant "keep going". Both hide the
 * two things a reader needs: where the walk stops, and what is kept. A rank as
 * a range puts the stopping rule in one place (RankView's iterator) and lets
 * what is kept be a pipeline -- `rank(m, head, d) | views::drop(1) |
 * views::filter(...)` -- that composes with every std::ranges algorithm.
 *
 * Nothing here allocates. A RankView is four words and a pointer; iterating
 * one is exactly the linked() calls a hand-written walk makes, in the same
 * order, with a cycle guard. That is what makes it usable on the render path.
 *
 * `noCell` stays the storage encoding (DimLink's sides are CellRefs, and a
 * 12-byte DimLink is R12's arithmetic), but it stops at this boundary: step()
 * and rankEnd() answer std::optional, so a caller composes with and_then /
 * transform / value_or instead of comparing against a sentinel it has to
 * remember exists.
 */
#ifndef ZIGZAG_CELL_VIEWS_HPP
#define ZIGZAG_CELL_VIEWS_HPP

#include <array>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>

#include "common/xanadu/ops.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include <gleditor/ranges.hpp>

namespace zigzag {

/**
 * @brief What a traversal needs from a manifold-shaped structure.
 *
 * Manifold and ArenaManifold both satisfy it, which is why every view below is
 * written once. `linked()` is required to be noexcept because the views are:
 * a view that can throw mid-iteration is not one the render path can use.
 */
template <typename M>
concept CellGraph = requires(const M &m, CellRef c, DimRef d, DimVector v) {
  { m.linked(c, d, v) } noexcept -> std::same_as<CellRef>;
  { m.dimensionsOf(c) } noexcept -> std::same_as<std::span<const DimLink>>;
  { m.cells() } noexcept -> std::same_as<std::span<const CellSlot>>;
  { m.cellCount() } noexcept -> std::convertible_to<std::size_t>;
  { m.traversalBound() } noexcept -> std::convertible_to<std::size_t>;
  { m.contains(c) } noexcept -> std::same_as<bool>;
  { m.slot(c) } noexcept -> std::same_as<SlotRef>;
};

/// A sentinel-encoded CellRef as an optional: noCell becomes nullopt.
[[nodiscard]] constexpr std::optional<CellRef> present(const CellRef ref) {
  return noCell == ref ? std::nullopt : std::optional{ref};
}

/// @p from's neighbour along @p dim, or nullopt. linked() with the sentinel
/// taken off, so a chain of hops is a chain of and_then()s.
template <CellGraph M>
[[nodiscard]] constexpr std::optional<CellRef>
step(const M &m, const CellRef from, const DimRef dim,
     const DimVector dir = DimVector::POS) noexcept {
  return present(m.linked(from, dim, dir));
}

template <CellGraph M>
[[nodiscard]] constexpr std::optional<CellRef>
step(const M &m, const CellRef from, const DirectedDim target) noexcept {
  return step(m, from, target.dim, target.dir);
}

/// A step() bound to its manifold and direction, for `and_then`:
/// `step(m, a, d1).and_then(hop(m, d2))`.
template <CellGraph M>
[[nodiscard]] constexpr auto
hop(const M &m, const DimRef dim,
    const DimVector dir = DimVector::POS) noexcept {
  return [&m, dim, dir](const CellRef from) noexcept {
    return step(m, from, dim, dir);
  };
}

/// The cell @p n hops from @p from along @p dim, or nullopt if the rank ends
/// first. Counts hops rather than cells, so on a ring it wraps: a grid
/// coordinate or a drop count means n steps, not n distinct cells.
template <CellGraph M>
[[nodiscard]] constexpr std::optional<CellRef>
hops(const M &m, const CellRef from, const DimRef dim, const std::size_t n,
     const DimVector dir = DimVector::POS) noexcept {
  auto at = present(from);
  for (std::size_t i = 0; i < n && at; ++i) {
    at = step(m, *at, dim, dir);
  }
  return at;
}

/**
 * @class RankView
 * @brief The cells of one rank, from @p start along @p dim, lazily.
 *
 * Yields @p start first, then each neighbour in turn, and ends at the first
 * of:
 * - a dead end (the next link is noCell),
 * - a self-link (the next cell is the current one),
 * - a ring closing (the next cell is @p start), or
 * - traversalBound() + 1 cells, which bounds a cycle that does not pass
 *   through @p start -- a rank that is a lasso rather than a ring.
 *
 * These are the rules the callback-driven walkRank() this replaced used, so
 * every caller that depended on them still can. A caller that wants fewer
 * cells pipes views::take().
 *
 * A start or dimension of noCell is an empty rank rather than a precondition
 * violation, so `rank(m, dimensionNamed(...), ...)` needs no guard.
 */
template <CellGraph M>
class RankView : public std::ranges::view_interface<RankView<M>> {
public:
  class iterator {
  public:
    using value_type       = CellRef;
    using difference_type  = std::ptrdiff_t;
    using iterator_concept = std::forward_iterator_tag;

    iterator() = default;
    iterator(const M *m, const CellRef start, const DimRef dim,
             const DimVector dir, const std::size_t limit) noexcept
        : m_{m}, start_{start}, cur_{start}, dim_{dim}, dir_{dir},
          limit_{limit} {
      if (noCell == dim || 0 == limit) {
        cur_ = noCell;
      }
    }

    [[nodiscard]] CellRef operator*() const noexcept { return cur_; }

    iterator &operator++() noexcept {
      if (++taken_ >= limit_) {
        cur_ = noCell;
        return *this;
      }
      const auto next = m_->linked(cur_, dim_, dir_);
      cur_            = (next == cur_ || next == start_) ? noCell : next;
      return *this;
    }

    iterator operator++(int) noexcept {
      auto before = *this;
      ++*this;
      return before;
    }

    [[nodiscard]] bool
    operator==(std::default_sentinel_t /*end*/) const noexcept {
      return noCell == cur_;
    }

    // Two iterators over one rank are equal when they are at the same place;
    // the step count distinguishes the visits of a lasso that the
    // traversalBound() guard is still bounding. Every end compares equal to
    // every other end.
    [[nodiscard]] bool operator==(const iterator &other) const noexcept {
      return (noCell == cur_ && noCell == other.cur_) ||
             (cur_ == other.cur_ && taken_ == other.taken_);
    }

  private:
    const M *m_{nullptr};
    CellRef start_{noCell};
    CellRef cur_{noCell};
    DimRef dim_{noCell};
    DimVector dir_{DimVector::POS};
    std::size_t taken_{0};
    std::size_t limit_{0};
  };

  RankView() = default;
  RankView(const M &m, const CellRef start, const DimRef dim,
           const DimVector dir) noexcept
      : m_{&m}, start_{start}, dim_{dim}, dir_{dir} {}

  [[nodiscard]] iterator begin() const noexcept {
    return iterator{m_, start_, dim_, dir_, m_->traversalBound() + 1};
  }
  [[nodiscard]] static std::default_sentinel_t end() noexcept { return {}; }

private:
  const M *m_{nullptr};
  CellRef start_{noCell};
  DimRef dim_{noCell};
  DimVector dir_{DimVector::POS};
};

/// The rank through @p start along @p dim in @p dir. See RankView.
template <CellGraph M>
[[nodiscard]] RankView<M> rank(const M &m, const CellRef start,
                               const DimRef dim,
                               const DimVector dir = DimVector::POS) noexcept {
  return RankView<M>{m, start, dim, dir};
}

template <CellGraph M>
[[nodiscard]] RankView<M> rank(const M &m, const CellRef start,
                               const DirectedDim target) noexcept {
  return RankView<M>{m, start, target.dim, target.dir};
}

/// The rank after @p start: its neighbours, not the cell itself. The ring
/// guard still stops at @p start, so a rank walked from its head -- the d.dims
/// rank from home, a group from its header cell -- ends where it began.
template <CellGraph M>
[[nodiscard]] auto rankAfter(const M &m, const CellRef start, const DimRef dim,
                             const DimVector dir = DimVector::POS) noexcept {
  return rank(m, start, dim, dir) | std::views::drop(1);
}

/// The last cell of @p start's rank in @p dir: @p start itself when it has no
/// neighbour there, the cell before @p start on a ring. Where an append onto a
/// rank links to. Total, unlike rankEnd(): it does not ask whether @p start is
/// held, because linked() on a cell it does not hold is a dead end anyway.
template <CellGraph M>
[[nodiscard]] CellRef rankTail(const M &m, const CellRef start,
                               const DimRef dim,
                               const DimVector dir = DimVector::POS) noexcept {
  return lastOf(rank(m, start, dim, dir)).value_or(start);
}

/**
 * @brief The far end of @p ref's rank along @p dim in @p dir, or nullopt when
 *        this manifold does not hold @p ref.
 *
 * A rank that loops answers @p ref itself: a ring has no end, and the cell
 * the question was asked about is the only one not chosen arbitrarily. This is
 * cloneMaster()'s rule, which is rankEnd() negward along d.clone.
 */
template <CellGraph M>
[[nodiscard]] std::optional<CellRef>
rankEnd(const M &m, const CellRef ref, const DimRef dim,
        const DimVector dir = DimVector::POS) noexcept {
  if (!m.contains(ref)) {
    return std::nullopt;
  }
  CellRef last = ref;
  for (const auto cell : rank(m, ref, dim, dir)) {
    last = cell;
  }
  return m.linked(last, dim, dir) == ref ? ref : last;
}

/// One neighbour of a cell: the dimension it lies along and the cell there.
struct Hop {
  DimRef dim{noCell};
  CellRef cell{noCell};

  bool operator==(const Hop &) const = default;
};

/// @p ref's neighbours in @p dir, one per dimension that has one. The run is
/// d.meta-dims, so this is "where can I go from here" without a lookup.
template <CellGraph M>
[[nodiscard]] auto neighbours(const M &m, const CellRef ref,
                              const DimVector dir) noexcept {
  return m.dimensionsOf(ref) |
         std::views::transform([dir](const DimLink &link) noexcept {
           return Hop{.dim = link.dim, .cell = link.neighbor(dir)};
         }) |
         std::views::filter(
             [](const Hop &hop) noexcept { return noCell != hop.cell; });
}

/// Both directions: every posward neighbour, then every negward one.
template <CellGraph M>
[[nodiscard]] auto neighbours(const M &m, const CellRef ref) noexcept {
  // A prvalue array, so the pipeline owns it (owning_view) rather than
  // referring to a local that is gone by the time the caller iterates.
  return std::array{DimVector::POS, DimVector::NEG} |
         std::views::transform([&m, ref](const DimVector dir) {
           return neighbours(m, ref, dir);
         }) |
         std::views::join;
}

/// The dimensions @p ref links on, as DimRefs.
template <CellGraph M>
[[nodiscard]] auto dimensionRefsOf(const M &m, const CellRef ref) noexcept {
  return m.dimensionsOf(ref) | std::views::transform(&DimLink::dim);
}

/// Every cell this manifold holds, as the CellRef that names it.
template <CellGraph M> [[nodiscard]] auto cellRefs(const M &m) noexcept {
  return m.cells() | std::views::transform(&CellSlot::birthOp);
}

// -- predicates, for views::filter ------------------------------------------

/// Whether a cell has a neighbour along @p dim in @p dir.
template <CellGraph M>
[[nodiscard]] auto linksAlong(const M &m, const DimRef dim,
                              const DimVector dir = DimVector::POS) noexcept {
  return [&m, dim, dir](const CellRef ref) noexcept {
    return noCell != m.linked(ref, dim, dir);
  };
}

/// Whether a cell carries a typed value of @p kind (R6).
template <CellGraph M>
[[nodiscard]] auto holdsValue(const M &m, const xanadu::ValueKind kind) {
  return [&m, kind](const CellRef ref) { return m.valueKindOf(ref) == kind; };
}

// -- ranges to optionals ------------------------------------------------------

using gleditor::firstOf;
using gleditor::lastOf;

} // namespace zigzag

#endif // ZIGZAG_CELL_VIEWS_HPP
