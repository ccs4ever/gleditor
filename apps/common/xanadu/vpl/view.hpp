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

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu::vpl {

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
  VplView(zigzag::CellRef origin, std::vector<zigzag::DirectedDim> axes)
      : origin_(origin), axes_(std::move(axes)) {}

  /// Construct a view with an origin cell and dimension refs (default POS
  /// direction).
  VplView(zigzag::CellRef origin, std::span<const zigzag::DimRef> dims)
      : origin_(origin) {
    axes_.reserve(dims.size());
    for (zigzag::DimRef d : dims) {
      axes_.push_back(zigzag::DirectedDim{d, zigzag::DimVector::POS});
    }
  }

  /// Construct a scalar numeric view (valence 0).
  static VplView makeScalar(double val, bool isFloat = false,
                            zigzag::CellRef cell = zigzag::noCell) {
    VplView v;
    v.origin_      = cell;
    v.scalarFloat_ = val;
    v.scalarInt_   = static_cast<std::int64_t>(val);
    v.isScalar_    = true;
    v.isFloat_     = isFloat;
    return v;
  }

  static VplView makeScalar(std::int64_t val,
                            zigzag::CellRef cell = zigzag::noCell) {
    VplView v;
    v.origin_      = cell;
    v.scalarFloat_ = static_cast<double>(val);
    v.scalarInt_   = val;
    v.isScalar_    = true;
    v.isFloat_     = false;
    return v;
  }

  static VplView makeScalar(std::string str,
                            zigzag::CellRef cell = zigzag::noCell) {
    VplView v;
    v.origin_       = cell;
    v.scalarString_ = std::move(str);
    v.isScalar_     = true;
    v.isString_     = true;
    return v;
  }

  [[nodiscard]] zigzag::CellRef origin() const noexcept { return origin_; }
  void setOrigin(zigzag::CellRef origin) noexcept { origin_ = origin; }

  [[nodiscard]] const std::vector<zigzag::DirectedDim> &axes() const noexcept {
    return axes_;
  }
  std::vector<zigzag::DirectedDim> &axes() noexcept { return axes_; }

  [[nodiscard]] const std::vector<std::size_t> &extents() const noexcept {
    return extents_;
  }
  void setExtents(std::vector<std::size_t> extents) noexcept {
    extents_ = std::move(extents);
  }

  /// Valence: number of dimensions indexed along (APL rank).
  [[nodiscard]] std::size_t valence() const noexcept { return axes_.size(); }

  [[nodiscard]] bool isScalar() const noexcept { return isScalar_; }
  [[nodiscard]] bool isFloat() const noexcept { return isFloat_; }
  [[nodiscard]] bool isString() const noexcept { return isString_; }
  [[nodiscard]] bool isRagged() const noexcept { return isRagged_; }
  void setRagged(bool ragged) noexcept { isRagged_ = ragged; }

  [[nodiscard]] bool isEnclosed() const noexcept { return isEnclosed_; }
  [[nodiscard]] const std::shared_ptr<VplView> &enclosedView() const noexcept {
    return enclosedView_;
  }
  void enclose(std::shared_ptr<VplView> inner) noexcept {
    isEnclosed_   = true;
    enclosedView_ = std::move(inner);
  }

  [[nodiscard]] double scalarFloat() const noexcept { return scalarFloat_; }
  [[nodiscard]] std::int64_t scalarInt() const noexcept { return scalarInt_; }
  void setScalarPayload(std::int64_t val) noexcept {
    scalarInt_   = val;
    scalarFloat_ = static_cast<double>(val);
  }
  void setScalarPayload(double val) noexcept {
    scalarFloat_ = val;
    scalarInt_   = static_cast<std::int64_t>(val);
  }
  [[nodiscard]] const std::string &scalarString() const noexcept {
    return scalarString_;
  }

  /**
   * @brief Transpose (⍉ / |:): permutes the axis list. O(1), no cell
   * allocations. Monadic transpose reverses the axis list.
   */
  void transpose() {
    std::reverse(axes_.begin(), axes_.end());
    if (!extents_.empty()) {
      std::reverse(extents_.begin(), extents_.end());
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
    if (!extents_.empty() && extents_.size() == axes_.size()) {
      newExtents.reserve(extents_.size());
    }
    for (std::size_t p : permutation) {
      if (p >= axes_.size()) return false;
      newAxes.push_back(axes_[p]);
      if (!newExtents.empty()) {
        newExtents.push_back(extents_[p]);
      }
    }
    axes_ = std::move(newAxes);
    if (!newExtents.empty()) {
      extents_ = std::move(newExtents);
    }
    return true;
  }

  /**
   * @brief Reverse first axis (⌽ / |.): walks first axis in opposite direction.
   * O(1).
   */
  void reverseFirst() {
    if (!axes_.empty()) {
      axes_[0].dir = -axes_[0].dir;
    }
  }

  /**
   * @brief Reverse last axis (⊖ / |..): walks last axis in opposite direction.
   * O(1).
   */
  void reverseLast() {
    if (!axes_.empty()) {
      axes_.back().dir = -axes_.back().dir;
    }
  }

  /**
   * @brief Computes shape extents along each axis by walking the manifold.
   */
  [[nodiscard]] std::vector<std::size_t>
  shape(const zigzag::ArenaManifold &arena) const {
    if (isScalar_) {
      return {};
    }
    if (!extents_.empty()) {
      return extents_;
    }
    if (axes_.empty() || origin_ == zigzag::noCell) {
      return {0};
    }

    std::vector<std::size_t> dims;
    dims.reserve(axes_.size());

    // Walk primary axis
    std::size_t count0  = 0;
    zigzag::CellRef cur = origin_;
    std::size_t limit   = arena.cellCount() + 1;
    while (cur != zigzag::noCell && limit-- > 0) {
      count0++;
      cur = arena.linked(cur, axes_[0].dim, axes_[0].dir);
    }
    dims.push_back(count0);

    // If higher valence, measure secondary extents along subsequent axes
    for (std::size_t a = 1; a < axes_.size(); ++a) {
      std::size_t firstExtent = 0;
      bool extentSet          = false;
      bool ragged             = false;

      // Sample along previous axis positions
      zigzag::CellRef rowCur = origin_;
      std::size_t rowLimit   = arena.cellCount() + 1;
      while (rowCur != zigzag::noCell && rowLimit-- > 0) {
        std::size_t extent = 0;
        zigzag::CellRef c  = rowCur;
        std::size_t l      = arena.cellCount() + 1;
        while (c != zigzag::noCell && l-- > 0) {
          extent++;
          c = arena.linked(c, axes_[a].dim, axes_[a].dir);
        }
        if (!extentSet) {
          firstExtent = extent;
          extentSet   = true;
        } else if (extent != firstExtent) {
          ragged = true;
        }
        rowCur = arena.linked(rowCur, axes_[0].dim, axes_[0].dir);
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
  [[nodiscard]] zigzag::CellRef
  cellAt(const zigzag::ArenaManifold &arena,
         std::span<const std::int64_t> coords) const {
    if (origin_ == zigzag::noCell) return zigzag::noCell;
    if (coords.empty()) return origin_;

    zigzag::CellRef cur = origin_;
    for (std::size_t i = 0; i < coords.size() && i < axes_.size(); ++i) {
      std::int64_t steps    = coords[i];
      zigzag::DimVector dir = axes_[i].dir;
      if (steps < 0) {
        dir   = -dir;
        steps = -steps;
      }
      while (steps > 0 && cur != zigzag::noCell) {
        cur = arena.linked(cur, axes_[i].dim, dir);
        steps--;
      }
      if (cur == zigzag::noCell) break;
    }
    return cur;
  }

  /**
   * @brief Collects all cell references in standard walk order.
   */
  [[nodiscard]] std::vector<zigzag::CellRef>
  collectCells(const zigzag::ArenaManifold &arena) const {
    std::vector<zigzag::CellRef> cells;
    if (origin_ == zigzag::noCell) {
      return cells;
    }
    if (axes_.empty()) {
      cells.push_back(origin_);
      return cells;
    }

    if (axes_.size() == 1) {
      auto sh             = shape(arena);
      zigzag::CellRef cur = origin_;
      std::size_t limit   = sh.empty() ? arena.cellCount() + 1
                                       : std::min(sh[0], arena.cellCount() + 1);
      while (cur != zigzag::noCell && limit-- > 0) {
        cells.push_back(cur);
        cur = arena.linked(cur, axes_[0].dim, axes_[0].dir);
      }
      return cells;
    }

    // Multi-dimensional traversal
    auto sh = shape(arena);
    if (sh.empty() || sh[0] == 0) return cells;

    std::vector<std::int64_t> coords(axes_.size(), 0);
    std::function<void(std::size_t)> traverse = [&](std::size_t dimIdx) {
      if (dimIdx == axes_.size()) {
        zigzag::CellRef c = cellAt(arena, coords);
        if (c != zigzag::noCell) {
          cells.push_back(c);
        }
        return;
      }
      for (std::size_t step = 0; step < sh[dimIdx]; ++step) {
        coords[dimIdx] = static_cast<std::int64_t>(step);
        traverse(dimIdx + 1);
      }
    };
    traverse(0);
    return cells;
  }

private:
  zigzag::CellRef origin_{zigzag::noCell};
  std::vector<zigzag::DirectedDim> axes_{};
  std::vector<std::size_t> extents_{};
  bool isRagged_{false};

  bool isEnclosed_{false};
  std::shared_ptr<VplView> enclosedView_{nullptr};

  // Scalar payload (for valence 0 numbers / strings)
  double scalarFloat_{0.0};
  std::int64_t scalarInt_{0};
  std::string scalarString_{};
  bool isScalar_{false};
  bool isFloat_{false};
  bool isString_{false};
};

} // namespace xanadu::vpl

#endif // COMMON_XANADU_VPL_VIEW_HPP
