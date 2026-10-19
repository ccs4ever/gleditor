/**
 * @file rankfilade.hpp
 * @brief The True Rankfilade: Cache-Conscious 1D B-Enfilade for Rank
 * Navigation.
 *
 * Implements O(log N) coordinate mapping and random-access over arbitrary
 * ZigZag ranks (descent: ordinal i -> CellRef; ascent: CellRef -> ordinal i).
 * Eliminates O(N) linear step/hop rank scans in scrollbars, layout
 * virtualization, and transductive pipelines.
 *
 * Strict Compliance:
 * - Ruling R8: Derived replay product in memory. Indexing NEVER mints
 * operations on disk.
 * - Ruling R9: Mathematical equivalence with linear step() rank walks
 * guaranteed.
 * - Ruling R12 & U3: CellSlot remains strictly 32 bytes; enfilade nodes live in
 *   transient vector storage.
 */
#ifndef XANADU_ENFILADE_RANKFILADE_HPP
#define XANADU_ENFILADE_RANKFILADE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"
#include <gleditor/enfilade/crum_node.hpp>

namespace xanadu::enfilade {

struct RankWid;

/**
 * @struct RankDsp
 * @brief Relative ordinal displacement monoid along a ZigZag rank.
 */
struct RankDsp {
  std::int32_t delta{0};

  [[nodiscard]] constexpr RankDsp compose(const RankDsp &other) const noexcept {
    return RankDsp{delta + other.delta};
  }
  [[nodiscard]] constexpr bool isIdentity() const noexcept {
    return 0 == delta;
  }
  [[nodiscard]] constexpr RankWid act(const RankWid &w) const noexcept;
  bool operator==(const RankDsp &) const = default;
};

/**
 * @struct RankWid
 * @brief Subtree aggregate count summary width monoid.
 */
struct RankWid {
  std::uint32_t count{0};

  [[nodiscard]] constexpr bool isEmpty() const noexcept { return 0 == count; }
  [[nodiscard]] constexpr RankWid combine(const RankWid &other) const noexcept {
    return RankWid{count + other.count};
  }
  bool operator==(const RankWid &) const = default;
};

inline constexpr RankWid RankDsp::act(const RankWid &w) const noexcept {
  return w;
}

static_assert(gleditor::enfilade::DisplacementMonoid<RankDsp>);
static_assert(gleditor::enfilade::WidthMonoid<RankWid>);
static_assert(gleditor::enfilade::EnfiladeAction<RankDsp, RankWid>);

/**
 * @struct LeafHandle
 * @brief O(1) entry handle mapping CellRef to leaf crum and slot.
 */
struct LeafHandle {
  std::uint32_t leafIndex{0};
  std::uint16_t chunkSlot{0};
  std::uint16_t offsetInChunk{0};
};

/**
 * @class Rankfilade
 * @brief Cache-conscious 1D B-enfilade (B = 8) indexing cells along a dimension
 * rank.
 */
class Rankfilade {
public:
  static constexpr std::size_t BranchingFactor = 8;
  static constexpr std::size_t LeafChunkSize   = 64;

  using Crum = gleditor::enfilade::CrumNode<RankDsp, RankWid, BranchingFactor>;

  Rankfilade() = default;

  /**
   * @brief Construct a Rankfilade directly from an ordered sequence of
   * CellRefs.
   */
  explicit Rankfilade(std::span<const zigzag::CellRef> cells);

  /**
   * @brief Construct a Rankfilade by walking a dimension rank from head.
   */
  template <zigzag::CellGraph M>
  static Rankfilade
  buildFromRank(const M &manifold, const zigzag::CellRef head,
                const zigzag::DimRef dim,
                const zigzag::DimVector dir = zigzag::DimVector::POS) {
    std::vector<zigzag::CellRef> cells;
    auto cur                   = zigzag::present(head);
    std::size_t guard          = 0;
    const std::size_t maxCells = manifold.traversalBound() + 1;

    while (cur && guard++ < maxCells) {
      cells.push_back(*cur);
      cur = zigzag::step(manifold, *cur, dim, dir);
      if (cur && *cur == head) {
        break; // Ring closing
      }
    }
    return Rankfilade(cells);
  }

  [[nodiscard]] bool empty() const noexcept { return cells_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return cells_.size(); }
  [[nodiscard]] std::size_t crumCount() const noexcept { return crums_.size(); }
  [[nodiscard]] std::span<const zigzag::CellRef> cells() const noexcept {
    return cells_;
  }

  /**
   * @brief O(log N) coordinate descent: locate the CellRef at 0-based ordinal
   * index i.
   */
  [[nodiscard]] std::optional<zigzag::CellRef>
  cellAtOrdinal(std::size_t ordinal) const noexcept;

  /**
   * @brief O(log N) coordinate ascent: locate the 0-based ordinal of cell c.
   */
  [[nodiscard]] std::optional<std::size_t>
  ordinalOfCell(zigzag::CellRef cell) const noexcept;

  /**
   * @brief Compute the normalized scrollbar thumb ratio [0.0, 1.0] for cell c.
   */
  [[nodiscard]] float scrollbarThumbRatio(zigzag::CellRef cell) const noexcept;

  /**
   * @brief Verifies that O(log N) queries match ground truth linear scans
   * exactly.
   */
  [[nodiscard]] bool verifyAgainstLinearScan() const noexcept;

private:
  void buildTree();

  std::vector<zigzag::CellRef> cells_;
  std::vector<Crum> crums_;
  std::unordered_map<zigzag::CellRef, LeafHandle> handles_;
  std::uint32_t leafBase_{0};
  std::uint32_t totalCount_{0};
};

} // namespace xanadu::enfilade

#endif // XANADU_ENFILADE_RANKFILADE_HPP
