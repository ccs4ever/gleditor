/**
 * @file view.hpp
 * @brief Multidimensional array view representation for VPL.
 *
 * Implements the VPL View: a set of cells indexed along an ordered list of
 * directed dimensions (zigzag::DirectedDim). Supports $O(1)$ transposition
 * (⍉/|:), $O(1)$ reverse walks (⌽/|.), extent determination, and
 * hyperstructural cell traversal.
 */
#ifndef COMMON_XANADU_VPL_VIEW_HPP
#define COMMON_XANADU_VPL_VIEW_HPP

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu::vpl {

/**
 * Which axes of a view are read in reverse (card V4b), parallel to the axis
 * list; the vector is empty when none is. `⌽`/`⊖` toggle a flag and touch
 * nothing else: the origin stays the operand's, and the axis keeps its
 * natural walking direction, which is a separate fact (an axis may run
 * negward by nature). A reversed axis is read by finding its far end from the
 * origin and walking back to the origin, so the view stays live: cells
 * appended to the rank later are in it, and it always holds exactly the cells
 * the unreversed view holds. The cost is that a reversed read walks the rank
 * twice.
 */
using AxisReversed = std::vector<bool>;

/**
 * @brief Represents a VPL multidimensional view or scalar value.
 *
 * A VPL value is either a scalar cell (valence 0, empty axes) or a view
 * comprising an origin cell and an ordered list of directed dimensions.
 */
class VplView {
public:
  VplView() = default;

  /// Construct a view with an origin cell and a list of directed dimensions.
  VplView(std::optional<zigzag::CellRef> origin,
          std::vector<zigzag::DirectedDim> axes)
      : origin_(origin), axes_(std::move(axes)) {}

  /// Construct a view with an origin cell and dimension refs (default POS
  /// direction).
  VplView(std::optional<zigzag::CellRef> origin,
          std::span<const zigzag::DimRef> dims)
      : origin_(origin) {
    axes_.reserve(dims.size());
    for (zigzag::DimRef d : dims) {
      axes_.emplace_back(d, zigzag::DimVector::POS);
    }
  }

  /// Construct a scalar numeric view (valence 0).
  static VplView
  makeScalar(double val, bool isFloat = false,
             std::optional<zigzag::CellRef> cell = std::nullopt) {
    VplView v;
    v.origin_      = cell;
    v.scalarFloat_ = val;
    v.scalarInt_   = static_cast<std::int64_t>(val);
    v.isScalar_    = true;
    v.isFloat_     = isFloat;
    return v;
  }

  static VplView
  makeScalar(std::int64_t val,
             std::optional<zigzag::CellRef> cell = std::nullopt) {
    VplView v;
    v.origin_      = cell;
    v.scalarFloat_ = static_cast<double>(val);
    v.scalarInt_   = val;
    v.isScalar_    = true;
    v.isFloat_     = false;
    return v;
  }

  static VplView
  makeScalar(std::string str,
             std::optional<zigzag::CellRef> cell = std::nullopt) {
    VplView v;
    v.origin_       = cell;
    v.scalarString_ = std::move(str);
    v.isScalar_     = true;
    v.isString_     = true;
    return v;
  }

  /// Source cells used to calculate this value; literals carry none.
  [[nodiscard]] const std::vector<zigzag::CellRef> &
  dependencies() const noexcept {
    return dependencies_;
  }
  VplView *setDependencies(std::vector<zigzag::CellRef> dependencies) {
    dependencies_ = std::move(dependencies);
    return this;
  }

  [[nodiscard]] std::optional<zigzag::CellRef> origin() const noexcept {
    return origin_;
  }
  VplView *setOrigin(std::optional<zigzag::CellRef> origin) noexcept {
    origin_ = origin;
    return this;
  }

  [[nodiscard]] const std::vector<zigzag::DirectedDim> &axes() const noexcept {
    return axes_;
  }
  std::vector<zigzag::DirectedDim> &axes() noexcept { return axes_; }

  [[nodiscard]] const std::vector<std::size_t> &extents() const noexcept {
    return extents_;
  }
  VplView *setExtents(std::vector<std::size_t> extents) noexcept {
    extents_ = std::move(extents);
    return this;
  }

  /// The reversed flag of each axis (see AxisReversed); empty when none is.
  [[nodiscard]] const AxisReversed &reversed() const noexcept {
    return reversed_;
  }
  VplView *setReversed(AxisReversed reversed) {
    if (!reversed.empty() && reversed.size() != axes_.size()) {
      throw std::invalid_argument(
          "VPL view reversed flags must be parallel to its axes");
    }
    reversed_ = std::move(reversed);
    if (std::ranges::none_of(reversed_, [](const bool flag) { return flag; })) {
      reversed_.clear();
    }
    return this;
  }
  [[nodiscard]] bool isReversed(std::size_t index) const noexcept {
    return index < reversed_.size() && reversed_[index];
  }

  /// Valence: number of dimensions indexed along (APL rank).
  [[nodiscard]] std::size_t valence() const noexcept { return axes_.size(); }

  [[nodiscard]] bool isScalar() const noexcept { return isScalar_; }
  [[nodiscard]] bool isFloat() const noexcept { return isFloat_; }
  [[nodiscard]] bool isString() const noexcept { return isString_; }
  [[nodiscard]] bool isRagged() const noexcept { return isRagged_; }
  VplView *setRagged(bool ragged) noexcept {
    isRagged_ = ragged;
    return this;
  }

  [[nodiscard]] bool isEnclosed() const noexcept { return isEnclosed_; }
  [[nodiscard]] const std::shared_ptr<VplView> &enclosedView() const noexcept {
    return enclosedView_;
  }
  VplView *enclose(std::shared_ptr<VplView> inner) noexcept {
    isEnclosed_   = true;
    enclosedView_ = std::move(inner);
    return this;
  }

  [[nodiscard]] double scalarFloat() const noexcept { return scalarFloat_; }
  [[nodiscard]] std::int64_t scalarInt() const noexcept { return scalarInt_; }
  VplView *setScalarPayload(std::int64_t val) noexcept {
    scalarInt_   = val;
    scalarFloat_ = static_cast<double>(val);
    return this;
  }
  VplView *setScalarPayload(double val) noexcept {
    scalarFloat_ = val;
    scalarInt_   = static_cast<std::int64_t>(val);
    return this;
  }
  [[nodiscard]] const std::string &scalarString() const noexcept {
    return scalarString_;
  }

  /**
   * @brief Transpose (⍉ / |:): permutes the axis list. O(1), no cell
   * allocations. Monadic transpose reverses the axis list.
   */
  void transpose() {
    std::ranges::reverse(axes_);
    std::ranges::reverse(reversed_);
    if (!extents_.empty()) {
      std::ranges::reverse(extents_);
    }
  }

  /**
   * @brief Dyadic transpose: reorders axes according to given permutation
   * vector.
   */
  bool transpose(std::span<const std::size_t> permutation) {
    if (permutation.size() != axes_.size()) {
      return false;
    }
    std::vector<zigzag::DirectedDim> newAxes;
    newAxes.reserve(axes_.size());
    std::vector<std::size_t> newExtents;
    AxisReversed newReversed;
    if (!extents_.empty() && extents_.size() == axes_.size()) {
      newExtents.reserve(extents_.size());
    }
    for (std::size_t p : permutation) {
      if (p >= axes_.size()) return false;
      newAxes.push_back(axes_[p]);
      if (!reversed_.empty()) newReversed.push_back(reversed_[p]);
      if (!newExtents.empty()) {
        newExtents.push_back(extents_[p]);
      }
    }
    axes_     = std::move(newAxes);
    reversed_ = std::move(newReversed);
    if (!newExtents.empty()) {
      extents_ = std::move(newExtents);
    }
    return true;
  }

  /**
   * @brief Reverse first axis (⌽ / |.): toggles its flag. O(1); the origin and
   * the cells read are unchanged, only their order.
   */
  void reverseFirst() { toggleReversed(0); }

  /**
   * @brief Reverse last axis (⊖ / |..): toggles its flag. O(1).
   */
  void reverseLast() {
    if (!axes_.empty()) toggleReversed(axes_.size() - 1);
  }

  /**
   * @brief Computes shape extents along each axis by walking the manifold.
   */
  [[nodiscard]] std::vector<std::size_t>
  shape(const zigzag::ArenaManifold &arena) const {
    requireKnownOrigin(arena);
    if (isScalar_) {
      return {};
    }
    if (!extents_.empty()) {
      return extents_;
    }
    if (axes_.empty() || !origin_) {
      return {0};
    }

    std::vector<std::size_t> dims;
    dims.reserve(axes_.size());

    // Walk primary axis
    std::size_t count0                 = 0;
    std::optional<zigzag::CellRef> cur = origin_;
    std::size_t limit                  = arena.cellCount() + 1;
    while (cur && limit-- > 0) {
      count0++;
      cur = arena.linked(*cur, axes_[0].dim, axes_[0].dir);
    }
    dims.push_back(count0);

    // If higher valence, measure secondary extents along subsequent axes
    for (std::size_t a = 1; a < axes_.size(); ++a) {
      std::size_t firstExtent = 0;
      bool extentSet          = false;
      bool ragged             = false;

      // Sample along previous axis positions
      std::optional<zigzag::CellRef> rowCur = origin_;
      std::size_t rowLimit                  = arena.cellCount() + 1;
      while (rowCur && rowLimit-- > 0) {
        std::size_t extent               = 0;
        std::optional<zigzag::CellRef> c = rowCur;
        std::size_t l                    = arena.cellCount() + 1;
        while (c && l-- > 0) {
          extent++;
          c = arena.linked(*c, axes_[a].dim, axes_[a].dir);
        }
        if (!extentSet) {
          firstExtent = extent;
          extentSet   = true;
        } else if (extent != firstExtent) {
          ragged = true;
        }
        rowCur = arena.linked(*rowCur, axes_[0].dim, axes_[0].dir);
      }
      dims.push_back(firstExtent);
      if (ragged) {
        const_cast<VplView *>(this)->isRagged_ = true;
      }
    }

    return dims;
  }

  /**
   * @brief Walks coordinates to fetch a cell.
   */
  [[nodiscard]] std::optional<zigzag::CellRef>
  cellAt(const zigzag::ArenaManifold &arena,
         std::span<const std::int64_t> coords) const {
    requireKnownOrigin(arena);
    if (!origin_) return std::nullopt;
    if (coords.empty()) return origin_;

    // A reversed axis counts its positions from the far end of the first
    // walk, so logical position p is natural position extent - 1 - p.
    std::vector<std::int64_t> natural(coords.begin(), coords.end());
    if (!reversed_.empty()) {
      const auto sh = shape(arena);
      for (std::size_t i = 0; i < natural.size() && i < sh.size(); ++i) {
        if (isReversed(i)) {
          natural[i] = static_cast<std::int64_t>(sh[i]) - 1 - natural[i];
        }
      }
    }
    return cellAtNatural(arena, natural);
  }

  /**
   * @brief Collects all cell references in standard walk order.
   */
  [[nodiscard]] std::vector<zigzag::CellRef>
  collectCells(const zigzag::ArenaManifold &arena) const {
    requireKnownOrigin(arena);
    std::vector<zigzag::CellRef> cells;
    if (!origin_) {
      return cells;
    }
    if (axes_.empty()) {
      cells.push_back(*origin_);
      return cells;
    }

    if (axes_.size() == 1) {
      auto sh                            = shape(arena);
      std::optional<zigzag::CellRef> cur = origin_;
      std::size_t limit                  = sh.empty() ? arena.cellCount() + 1
                                                      : std::min(sh[0], arena.cellCount() + 1);
      while (cur && limit-- > 0) {
        cells.push_back(*cur);
        cur = arena.linked(*cur, axes_[0].dim, axes_[0].dir);
      }
      if (isReversed(0)) std::ranges::reverse(cells);
      return cells;
    }

    // Multi-dimensional traversal
    auto sh = shape(arena);
    if (sh.empty() || sh[0] == 0) return cells;

    std::vector<std::int64_t> coords(axes_.size(), 0);
    std::function<void(std::size_t)> traverse = [&](std::size_t dimIdx) {
      if (dimIdx == axes_.size()) {
        const auto c = cellAtNatural(arena, coords);
        if (c) {
          cells.push_back(*c);
        }
        return;
      }
      for (std::size_t step = 0; step < sh[dimIdx]; ++step) {
        coords[dimIdx] = static_cast<std::int64_t>(
            isReversed(dimIdx) ? sh[dimIdx] - 1 - step : step);
        traverse(dimIdx + 1);
      }
    };
    traverse(0);
    return cells;
  }

  /**
   * @brief The same cells in the same order as a view nothing is reversed in
   * (origin at the far end, axes flipped), or nullopt when no such view reads
   * them. For the wire, which carries an origin, directions and extents and
   * no flag. Exact for any reversed rank-1 view and for rectangular arrays;
   * a ragged array may not survive the move of the origin, which is checked
   * by reading both and comparing, never assumed.
   */
  [[nodiscard]] std::optional<VplView>
  walkable(const zigzag::ArenaManifold &arena) const {
    if (reversed_.empty()) return *this;
    const auto cells = collectCells(arena);
    VplView out{cells.empty() ? std::nullopt
                              : std::optional<zigzag::CellRef>{cells.front()},
                axes_};
    for (std::size_t i = 0; i < axes_.size(); ++i) {
      if (isReversed(i)) out.axes_[i].dir = -out.axes_[i].dir;
    }
    out.extents_ = shape(arena);
    out.setDependencies(dependencies_);
    if (out.collectCells(arena) != cells) return std::nullopt;
    return out;
  }

private:
  void toggleReversed(std::size_t index) {
    if (axes_.empty() || index >= axes_.size()) return;
    reversed_.resize(axes_.size(), false);
    reversed_[index] = !reversed_[index];
    if (std::ranges::none_of(reversed_, [](const bool flag) { return flag; })) {
      reversed_.clear();
    }
  }

  [[nodiscard]] std::optional<zigzag::CellRef>
  cellAtNatural(const zigzag::ArenaManifold &arena,
                std::span<const std::int64_t> coords) const {
    std::optional<zigzag::CellRef> cur = origin_;
    for (std::size_t i = 0; i < coords.size() && i < axes_.size(); ++i) {
      std::int64_t steps    = coords[i];
      zigzag::DimVector dir = axes_[i].dir;
      if (steps < 0) {
        dir   = -dir;
        steps = -steps;
      }
      while (steps > 0 && cur) {
        cur = arena.linked(*cur, axes_[i].dim, dir);
        steps--;
      }
      if (!cur) break;
    }
    return cur;
  }

  void requireKnownOrigin(const zigzag::ArenaManifold &arena) const {
    if (origin_ && !arena.contains(*origin_)) {
      throw std::invalid_argument(
          "VPL view origin does not exist in this arena");
    }
  }
  std::optional<zigzag::CellRef> origin_;
  std::vector<zigzag::DirectedDim> axes_;
  AxisReversed reversed_;
  std::vector<std::size_t> extents_;
  std::vector<zigzag::CellRef> dependencies_;
  bool isRagged_{false};

  bool isEnclosed_{false};
  std::shared_ptr<VplView> enclosedView_{nullptr};

  // Scalar payload (for valence 0 numbers / strings)
  double scalarFloat_{0.0};
  std::int64_t scalarInt_{0};
  std::string scalarString_;
  bool isScalar_{false};
  bool isFloat_{false};
  bool isString_{false};
};

} // namespace xanadu::vpl

#endif // COMMON_XANADU_VPL_VIEW_HPP
