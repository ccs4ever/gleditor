/**
 * @file arrayfilade.hpp
 * @brief The True Arrayfilade: Multi-Valence Coordinate & Query Planning
 * B-Enfilade.
 *
 * Implements Frontier 5 of the Grand Enfilade architecture:
 * 1. Multi-Valence Arrayfilade (for VPL):
 *    - Relative multidimensional displacement monoid ArrayDsp (Delta d1..dm)
 *    - Width monoid ArrayWid: Bounding shape, parallel aggregation monoids
 *      (count, sum, min, max, product) for O(1) VPL reductions (+/A, |_/A).
 *    - O(log N) ordinal descent subscripting A[i].
 *    - Sublinear slicing A[i1..i2, j1..j2] without cell copying.
 * 2. Topological Query Planning Index (for VQL):
 *    - Ranks and manifolds indexed by enfilades summarizing R6 canonical scalar
 *      bounds ([minScalar, maxScalar]) and Bloom text functors.
 *    - Predicate pushdown (couldMatch) pruning subtrees in O(1) for range,
 *      equality, and text predicates.
 *
 * Strict Compliance:
 * - Ruling R6: Canonical scalar bits (CompactOpNode::value /
 * CellSlot::valueBits).
 * - Ruling R8: Derived replay product in memory; queries NEVER mint disk ops.
 * - Ruling R9: Mathematical equivalence verified via verifyAgainstLinearScan().
 * - Ruling R12 & U3: CellSlot remains strictly 32 bytes.
 * - Ruling V3: Ephemeral derived index with rebuild-on-demand policy.
 */
#ifndef XANADU_ENFILADE_ARRAYFILADE_HPP
#define XANADU_ENFILADE_ARRAYFILADE_HPP

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/enfilade/crum_node.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu {
class SpanReader;
} // namespace xanadu

namespace xanadu::enfilade {

inline constexpr std::size_t MaxValence = 4;

struct ArrayWid;

/**
 * @struct ArrayDsp
 * @brief Relative displacement monoid across multidimensional axes.
 */
struct ArrayDsp {
  std::uint8_t valence{1};
  std::array<std::int64_t, MaxValence> deltaCoords{};

  [[nodiscard]] ArrayDsp compose(const ArrayDsp &other) const noexcept {
    ArrayDsp res{};
    res.valence = std::max(valence, other.valence);
    for (std::size_t i = 0; i < MaxValence; ++i) {
      res.deltaCoords[i] = deltaCoords[i] + other.deltaCoords[i];
    }
    return res;
  }

  [[nodiscard]] bool isIdentity() const noexcept {
    for (std::size_t i = 0; i < MaxValence; ++i) {
      if (deltaCoords[i] != 0) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] ArrayWid act(const ArrayWid &w) const noexcept;

  bool operator==(const ArrayDsp &) const = default;
};

/**
 * @struct ArrayWid
 * @brief Subtree bounding shape, parallel aggregation, and query planning
 * summary.
 */
struct ArrayWid {
  std::uint8_t valence{1};
  std::uint64_t count{0}; ///< Total active cells in subtree.

  // Multidimensional bounding coordinates:
  std::array<std::int64_t, MaxValence> minCoord{
      std::numeric_limits<std::int64_t>::max(),
      std::numeric_limits<std::int64_t>::max(),
      std::numeric_limits<std::int64_t>::max(),
      std::numeric_limits<std::int64_t>::max()};
  std::array<std::int64_t, MaxValence> maxCoord{
      std::numeric_limits<std::int64_t>::lowest(),
      std::numeric_limits<std::int64_t>::lowest(),
      std::numeric_limits<std::int64_t>::lowest(),
      std::numeric_limits<std::int64_t>::lowest()};

  // Parallel Aggregation Monoids for O(1) VPL Reductions:
  double sum{0.0};                                         ///< +/A
  double minVal{std::numeric_limits<double>::infinity()};  ///< |_/A
  double maxVal{-std::numeric_limits<double>::infinity()}; ///< |~/A
  double product{1.0};                                     ///< x/A

  // Topological Query Planning Bounds (R6 Canonical Bits & Bloom Functors):
  double minScalar{std::numeric_limits<double>::infinity()};
  double maxScalar{-std::numeric_limits<double>::infinity()};
  std::uint64_t bloomFilter{0};    ///< 64-bit Bloom filter for text functors.
  std::uint32_t scalarTypeMask{0}; ///< Bitmask of (1 << ValueKind) present.

  [[nodiscard]] bool isEmpty() const noexcept { return 0 == count; }

  [[nodiscard]] ArrayWid combine(const ArrayWid &other) const noexcept {
    if (isEmpty()) {
      return other;
    }
    if (other.isEmpty()) {
      return *this;
    }
    ArrayWid res{};
    res.valence = std::max(valence, other.valence);
    res.count   = count + other.count;

    for (std::size_t i = 0; i < MaxValence; ++i) {
      res.minCoord[i] = std::min(minCoord[i], other.minCoord[i]);
      res.maxCoord[i] = std::max(maxCoord[i], other.maxCoord[i]);
    }

    res.sum     = sum + other.sum;
    res.minVal  = std::min(minVal, other.minVal);
    res.maxVal  = std::max(maxVal, other.maxVal);
    res.product = product * other.product;

    res.minScalar      = std::min(minScalar, other.minScalar);
    res.maxScalar      = std::max(maxScalar, other.maxScalar);
    res.bloomFilter    = bloomFilter | other.bloomFilter;
    res.scalarTypeMask = scalarTypeMask | other.scalarTypeMask;
    return res;
  }

  [[nodiscard]] bool overlapsBox(
      const std::array<std::int64_t, MaxValence> &boxMin,
      const std::array<std::int64_t, MaxValence> &boxMax) const noexcept {
    if (isEmpty()) {
      return false;
    }
    for (std::size_t i = 0; i < valence; ++i) {
      if (maxCoord[i] < boxMin[i] || minCoord[i] > boxMax[i]) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool containsCoord(
      const std::array<std::int64_t, MaxValence> &coord) const noexcept {
    if (isEmpty()) {
      return false;
    }
    for (std::size_t i = 0; i < valence; ++i) {
      if (coord[i] < minCoord[i] || coord[i] > maxCoord[i]) {
        return false;
      }
    }
    return true;
  }

  bool operator==(const ArrayWid &) const = default;
};

inline ArrayWid ArrayDsp::act(const ArrayWid &w) const noexcept {
  if (w.isEmpty() || isIdentity()) {
    return w;
  }
  ArrayWid res = w;
  for (std::size_t i = 0; i < MaxValence; ++i) {
    if (res.minCoord[i] != std::numeric_limits<std::int64_t>::max()) {
      res.minCoord[i] += deltaCoords[i];
    }
    if (res.maxCoord[i] != std::numeric_limits<std::int64_t>::lowest()) {
      res.maxCoord[i] += deltaCoords[i];
    }
  }
  return res;
}

static_assert(DisplacementMonoid<ArrayDsp>);
static_assert(WidthMonoid<ArrayWid>);
static_assert(EnfiladeAction<ArrayDsp, ArrayWid>);

/**
 * @struct ArrayCellEntry
 * @brief Cache-conscious leaf descriptor for a cell in an array or rank.
 */
struct ArrayCellEntry {
  std::array<std::int64_t, MaxValence>
      coords{};                            ///< Multidimensional coordinate.
  zigzag::CellRef cellRef{zigzag::noCell}; ///< Underlying cell ref.
  xanadu::ValueKind valueKind{xanadu::ValueKind::None};
  std::uint64_t valueBits{0};
  std::uint64_t textHash{0};
  double numericValue{0.0};

  bool operator==(const ArrayCellEntry &) const = default;

  [[nodiscard]] static ArrayCellEntry fromCell(
      zigzag::CellRef ref, const std::array<std::int64_t, MaxValence> &coords,
      xanadu::ValueKind kind, std::uint64_t bits, std::string_view text = {});

  [[nodiscard]] static ArrayCellEntry
  fromDouble(zigzag::CellRef ref, std::int64_t index, double val);

  [[nodiscard]] static ArrayCellEntry
  fromInt(zigzag::CellRef ref, std::int64_t index, std::int64_t val);
};

inline constexpr std::uint64_t fnv1aOffsetBasis = 14695981039346656037ULL;
inline constexpr std::uint64_t fnv1aPrime       = 1099511628211ULL;

[[nodiscard]] inline std::uint64_t hashString(std::string_view s) noexcept {
  std::uint64_t h = fnv1aOffsetBasis;
  for (const unsigned char c : s) {
    h ^= static_cast<std::uint64_t>(c);
    h *= fnv1aPrime;
  }
  return h;
}

[[nodiscard]] inline std::uint64_t
textToBloomMask(std::string_view s) noexcept {
  if (s.empty()) {
    return 0ULL;
  }
  const std::uint64_t h1   = hashString(s);
  const std::uint64_t h2   = h1 ^ (h1 >> 32);
  const std::uint64_t bit1 = 1ULL << (h1 % 64);
  const std::uint64_t bit2 = 1ULL << ((h1 >> 8) % 64);
  const std::uint64_t bit3 = 1ULL << (h2 % 64);
  return bit1 | bit2 | bit3;
}

/**
 * @enum PredicateOp
 * @brief Relational, equality, and text predicate operators for VQL queries.
 */
enum class PredicateOp : std::uint8_t {
  Equal              = 0,
  NotEqual           = 1,
  LessThan           = 2,
  LessThanOrEqual    = 3,
  GreaterThan        = 4,
  GreaterThanOrEqual = 5,
  Between            = 6,
  TextEqual          = 7,
};

/**
 * @struct Predicate
 * @brief Structured query predicate supporting subtree pushdown pruning.
 */
struct Predicate {
  PredicateOp op{PredicateOp::Equal};
  double numericThreshold{0.0};
  double numericUpperBound{0.0}; ///< For PredicateOp::Between.
  std::string textValue{};
  std::uint64_t textHash{0};
  std::uint64_t bloomMask{0};

  static Predicate gt(double val) noexcept {
    Predicate p{};
    p.op               = PredicateOp::GreaterThan;
    p.numericThreshold = val;
    return p;
  }

  static Predicate gte(double val) noexcept {
    Predicate p{};
    p.op               = PredicateOp::GreaterThanOrEqual;
    p.numericThreshold = val;
    return p;
  }

  static Predicate lt(double val) noexcept {
    Predicate p{};
    p.op               = PredicateOp::LessThan;
    p.numericThreshold = val;
    return p;
  }

  static Predicate lte(double val) noexcept {
    Predicate p{};
    p.op               = PredicateOp::LessThanOrEqual;
    p.numericThreshold = val;
    return p;
  }

  static Predicate eq(double val) noexcept {
    Predicate p{};
    p.op               = PredicateOp::Equal;
    p.numericThreshold = val;
    return p;
  }

  static Predicate between(double low, double high) noexcept {
    Predicate p{};
    p.op                = PredicateOp::Between;
    p.numericThreshold  = low;
    p.numericUpperBound = high;
    return p;
  }

  static Predicate textEq(std::string_view text) {
    Predicate p{};
    p.op        = PredicateOp::TextEqual;
    p.textValue = std::string(text);
    p.textHash  = hashString(text);
    p.bloomMask = textToBloomMask(text);
    return p;
  }

  [[nodiscard]] bool couldMatch(const ArrayWid &wid) const noexcept {
    if (wid.isEmpty()) {
      return false;
    }
    if (op != PredicateOp::TextEqual) {
      constexpr std::uint32_t numericMask =
          (1U << static_cast<std::uint8_t>(xanadu::ValueKind::Double)) |
          (1U << static_cast<std::uint8_t>(xanadu::ValueKind::Int64)) |
          (1U << static_cast<std::uint8_t>(xanadu::ValueKind::Bool));
      if (0 == (wid.scalarTypeMask & numericMask)) {
        return false;
      }
    }

    switch (op) {
    case PredicateOp::GreaterThan:
      return wid.maxScalar > numericThreshold;
    case PredicateOp::GreaterThanOrEqual:
      return wid.maxScalar >= numericThreshold;
    case PredicateOp::LessThan:
      return wid.minScalar < numericThreshold;
    case PredicateOp::LessThanOrEqual:
      return wid.minScalar <= numericThreshold;
    case PredicateOp::Equal:
      return !(numericThreshold < wid.minScalar ||
               numericThreshold > wid.maxScalar);
    case PredicateOp::NotEqual:
      return !(wid.minScalar == numericThreshold &&
               wid.maxScalar == numericThreshold);
    case PredicateOp::Between:
      return !(wid.maxScalar < numericThreshold ||
               wid.minScalar > numericUpperBound);
    case PredicateOp::TextEqual:
      if (0 != bloomMask && (wid.bloomFilter & bloomMask) != bloomMask) {
        return false;
      }
      return true;
    }
    return true;
  }

  [[nodiscard]] bool matches(const ArrayCellEntry &entry) const noexcept {
    if (op != PredicateOp::TextEqual &&
        entry.valueKind == xanadu::ValueKind::None) {
      return false;
    }
    switch (op) {
    case PredicateOp::GreaterThan:
      return entry.numericValue > numericThreshold;
    case PredicateOp::GreaterThanOrEqual:
      return entry.numericValue >= numericThreshold;
    case PredicateOp::LessThan:
      return entry.numericValue < numericThreshold;
    case PredicateOp::LessThanOrEqual:
      return entry.numericValue <= numericThreshold;
    case PredicateOp::Equal:
      return entry.numericValue == numericThreshold;
    case PredicateOp::NotEqual:
      return entry.numericValue != numericThreshold;
    case PredicateOp::Between:
      return entry.numericValue >= numericThreshold &&
             entry.numericValue <= numericUpperBound;
    case PredicateOp::TextEqual:
      return entry.textHash == textHash;
    }
    return false;
  }
};

/**
 * @struct QueryPlanStats
 * @brief Telemetry for query planning pruning performance.
 */
struct QueryPlanStats {
  std::uint64_t subtreesExamined{0};
  std::uint64_t subtreesPruned{0};
  std::uint64_t leavesExamined{0};
  std::uint64_t leavesMatched{0};
};

/**
 * @class Arrayfilade
 * @brief Multi-Valence Coordinate & Query Planning B-Enfilade.
 */
class Arrayfilade {
public:
  static constexpr std::size_t BranchingFactor = 8;
  using Crum = CrumNode<ArrayDsp, ArrayWid, BranchingFactor>;

  Arrayfilade() = default;

  // -- Construction -----------------------------------------------------------

  [[nodiscard]] static Arrayfilade
  fromEntries(std::span<const ArrayCellEntry> entries,
              std::uint8_t valence = 1);

  [[nodiscard]] static Arrayfilade
  fromRank(const zigzag::Manifold &m, zigzag::CellRef head, zigzag::DimRef dim,
           const xanadu::SpanReader *reader = nullptr);

  [[nodiscard]] static Arrayfilade
  fromArenaRank(const zigzag::ArenaManifold &am, zigzag::CellRef head,
                zigzag::DimRef dim, const xanadu::SpanReader *reader = nullptr);

  [[nodiscard]] static Arrayfilade
  fromMatrix(const zigzag::Manifold &m, zigzag::CellRef origin,
             zigzag::DimRef rowDim, zigzag::DimRef colDim,
             const xanadu::SpanReader *reader = nullptr);

  // -- VPL Operations ---------------------------------------------------------

  /// Subscripting A[coords] in O(log N) ordinal descent.
  [[nodiscard]] std::optional<ArrayCellEntry>
  subscript(const std::array<std::int64_t, MaxValence> &coords) const;

  /// 1D Subscripting A[index] in O(log N).
  [[nodiscard]] std::optional<ArrayCellEntry>
  subscript1D(std::int64_t index) const;

  /// Slicing A[min..max] in sublinear time without cell copying.
  [[nodiscard]] Arrayfilade
  slice(const std::array<std::int64_t, MaxValence> &sliceMin,
        const std::array<std::int64_t, MaxValence> &sliceMax) const;

  /// 1D Slicing A[start..end].
  [[nodiscard]] Arrayfilade slice1D(std::int64_t start, std::int64_t end) const;

  // -- O(1) Parallel Reductions -----------------------------------------------

  [[nodiscard]] double sum() const noexcept;
  [[nodiscard]] double min() const noexcept;
  [[nodiscard]] double max() const noexcept;
  [[nodiscard]] double product() const noexcept;
  [[nodiscard]] std::uint64_t count() const noexcept;
  [[nodiscard]] std::array<std::int64_t, MaxValence> shape() const noexcept;
  [[nodiscard]] const ArrayWid &rootSummary() const noexcept;

  // -- VQL Query Planning -----------------------------------------------------

  [[nodiscard]] std::vector<ArrayCellEntry>
  planQuery(const Predicate &pred) const;

  [[nodiscard]] std::vector<zigzag::CellRef>
  planQueryCells(const Predicate &pred) const;

  [[nodiscard]] std::vector<ArrayCellEntry>
  planQueryWithStats(const Predicate &pred, QueryPlanStats &stats) const;

  // -- Ruling R9 Verification -------------------------------------------------

  [[nodiscard]] bool
  verifyAgainstLinearScan(const Predicate &pred,
                          std::span<const ArrayCellEntry> linearCells) const;

  [[nodiscard]] bool verifyReductionsAgainstLinear(
      std::span<const ArrayCellEntry> linearCells) const;

  [[nodiscard]] bool verifySubscriptAgainstLinear(
      std::span<const ArrayCellEntry> linearCells) const;

  [[nodiscard]] bool
  verifySliceAgainstLinear(const std::array<std::int64_t, MaxValence> &sliceMin,
                           const std::array<std::int64_t, MaxValence> &sliceMax,
                           std::span<const ArrayCellEntry> linearCells) const;

  // -- Tree Introspection -----------------------------------------------------

  [[nodiscard]] std::size_t nodeCount() const noexcept { return nodes_.size(); }
  [[nodiscard]] std::size_t leafCount() const noexcept {
    return leaves_.size();
  }
  [[nodiscard]] std::size_t depth() const noexcept;
  [[nodiscard]] bool empty() const noexcept { return leaves_.empty(); }
  [[nodiscard]] std::uint8_t valence() const noexcept { return valence_; }

private:
  void buildTree(std::span<const ArrayCellEntry> entries);

  void querySubtree(std::uint32_t nodeIdx, const ArrayDsp &cumDsp,
                    const Predicate &pred, std::vector<ArrayCellEntry> &results,
                    QueryPlanStats &stats) const;

  void sliceSubtree(std::uint32_t nodeIdx, const ArrayDsp &cumDsp,
                    const std::array<std::int64_t, MaxValence> &sliceMin,
                    const std::array<std::int64_t, MaxValence> &sliceMax,
                    std::vector<ArrayCellEntry> &slicedLeaves) const;

  std::optional<ArrayCellEntry>
  subscriptSubtree(std::uint32_t nodeIdx, const ArrayDsp &cumDsp,
                   const std::array<std::int64_t, MaxValence> &coords) const;

  std::vector<Crum> nodes_{};
  std::vector<ArrayCellEntry> leaves_{};
  std::uint32_t rootIndex_{0};
  std::uint8_t valence_{1};
  ArrayWid cachedRootSummary_{};
};

} // namespace xanadu::enfilade

#endif // XANADU_ENFILADE_ARRAYFILADE_HPP
