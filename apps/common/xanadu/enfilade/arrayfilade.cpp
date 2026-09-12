/**
 * @file arrayfilade.cpp
 * @brief Implementation of the True Arrayfilade and VQL Query Planning Index.
 */
#include "common/xanadu/enfilade/arrayfilade.hpp"

#include <cmath>
#include <limits>
#include <queue>

namespace xanadu::enfilade {

using zigzag::DimVector;

// -- ArrayCellEntry Helper Methods -------------------------------------------

ArrayCellEntry
ArrayCellEntry::fromCell(const zigzag::CellRef ref,
                         const std::array<std::int64_t, MaxValence> &coords,
                         const xanadu::ValueKind kind, const std::uint64_t bits,
                         const std::string_view text) {
  ArrayCellEntry entry{};
  entry.coords    = coords;
  entry.cellRef   = ref;
  entry.valueKind = kind;
  entry.valueBits = bits;

  switch (kind) {
  case xanadu::ValueKind::Double:
    entry.numericValue = std::bit_cast<double>(bits);
    break;
  case xanadu::ValueKind::Int64:
    entry.numericValue = static_cast<double>(std::bit_cast<std::int64_t>(bits));
    break;
  case xanadu::ValueKind::Bool:
    entry.numericValue = (0 != bits) ? 1.0 : 0.0;
    break;
  case xanadu::ValueKind::None:
    entry.numericValue = 0.0;
    break;
  }

  if (!text.empty()) {
    entry.textHash = hashString(text);
  } else if (0 != bits) {
    entry.textHash = bits ^ fnv1aOffsetBasis;
  }

  return entry;
}

ArrayCellEntry ArrayCellEntry::fromDouble(const zigzag::CellRef ref,
                                          const std::int64_t index,
                                          const double val) {
  ArrayCellEntry entry{};
  entry.coords[0]    = index;
  entry.cellRef      = ref;
  entry.valueKind    = xanadu::ValueKind::Double;
  entry.valueBits    = std::bit_cast<std::uint64_t>(val);
  entry.numericValue = val;
  entry.textHash     = entry.valueBits ^ fnv1aOffsetBasis;
  return entry;
}

ArrayCellEntry ArrayCellEntry::fromInt(const zigzag::CellRef ref,
                                       const std::int64_t index,
                                       const std::int64_t val) {
  ArrayCellEntry entry{};
  entry.coords[0]    = index;
  entry.cellRef      = ref;
  entry.valueKind    = xanadu::ValueKind::Int64;
  entry.valueBits    = std::bit_cast<std::uint64_t>(val);
  entry.numericValue = static_cast<double>(val);
  entry.textHash     = entry.valueBits ^ fnv1aOffsetBasis;
  return entry;
}

// -- Construction -------------------------------------------------------------

Arrayfilade Arrayfilade::fromEntries(std::span<const ArrayCellEntry> entries,
                                     const std::uint8_t valence) {
  Arrayfilade filade{};
  filade.valence_ =
      std::max<std::uint8_t>(1, std::min<std::uint8_t>(valence, MaxValence));
  filade.buildTree(entries);
  return filade;
}

Arrayfilade Arrayfilade::fromRank(const zigzag::Manifold &m,
                                  const zigzag::CellRef head,
                                  const zigzag::DimRef dim,
                                  const xanadu::SpanReader *reader) {
  std::vector<ArrayCellEntry> entries{};
  zigzag::CellRef cur = head;
  std::int64_t idx    = 0;

  for (std::size_t steps = 0; steps < m.cellCount() && zigzag::noCell != cur;
       ++steps) {
    const auto *slot = m.slot(cur);
    if (nullptr == slot) {
      break;
    }
    const auto kind = static_cast<xanadu::ValueKind>(slot->valueKind);
    std::string text{};
    if (nullptr != reader) {
      text = m.textOf(cur, *reader);
    }
    std::array<std::int64_t, MaxValence> coords{};
    coords[0] = idx++;

    entries.push_back(
        ArrayCellEntry::fromCell(cur, coords, kind, slot->valueBits, text));

    const auto next = m.linked(cur, dim, DimVector::POS);
    if (next == cur) {
      break;
    }
    cur = next;
  }

  return fromEntries(entries, 1);
}

Arrayfilade Arrayfilade::fromArenaRank(const zigzag::ArenaManifold &am,
                                       const zigzag::CellRef head,
                                       const zigzag::DimRef dim,
                                       const xanadu::SpanReader *reader) {
  std::vector<ArrayCellEntry> entries{};
  zigzag::CellRef cur = head;
  std::int64_t idx    = 0;

  for (std::size_t steps = 0; steps < am.cellCount() && zigzag::noCell != cur;
       ++steps) {
    const auto *slot = am.slot(cur);
    if (nullptr == slot) {
      break;
    }
    const auto kind        = static_cast<xanadu::ValueKind>(slot->valueKind);
    const std::string text = am.textOf(cur, reader);
    std::array<std::int64_t, MaxValence> coords{};
    coords[0] = idx++;

    entries.push_back(
        ArrayCellEntry::fromCell(cur, coords, kind, slot->valueBits, text));

    const auto next = am.linked(cur, dim, DimVector::POS);
    if (next == cur) {
      break;
    }
    cur = next;
  }

  return fromEntries(entries, 1);
}

Arrayfilade Arrayfilade::fromMatrix(const zigzag::Manifold &m,
                                    const zigzag::CellRef origin,
                                    const zigzag::DimRef rowDim,
                                    const zigzag::DimRef colDim,
                                    const xanadu::SpanReader *reader) {
  std::vector<ArrayCellEntry> entries{};
  zigzag::CellRef rowCur = origin;
  std::int64_t r         = 0;

  for (std::size_t rowSteps = 0;
       rowSteps < m.cellCount() && zigzag::noCell != rowCur; ++rowSteps) {
    zigzag::CellRef colCur = rowCur;
    std::int64_t c         = 0;

    for (std::size_t colSteps = 0;
         colSteps < m.cellCount() && zigzag::noCell != colCur; ++colSteps) {
      const auto *slot = m.slot(colCur);
      if (nullptr != slot) {
        const auto kind = static_cast<xanadu::ValueKind>(slot->valueKind);
        std::string text{};
        if (nullptr != reader) {
          text = m.textOf(colCur, *reader);
        }
        std::array<std::int64_t, MaxValence> coords{};
        coords[0] = r;
        coords[1] = c;

        entries.push_back(ArrayCellEntry::fromCell(colCur, coords, kind,
                                                   slot->valueBits, text));
      }

      const auto nextCol = m.linked(colCur, colDim, DimVector::POS);
      if (nextCol == colCur || zigzag::noCell == nextCol) {
        break;
      }
      colCur = nextCol;
      ++c;
    }

    const auto nextRow = m.linked(rowCur, rowDim, DimVector::POS);
    if (nextRow == rowCur || zigzag::noCell == nextRow) {
      break;
    }
    rowCur = nextRow;
    ++r;
  }

  return fromEntries(entries, 2);
}

void Arrayfilade::buildTree(std::span<const ArrayCellEntry> entries) {
  leaves_.assign(entries.begin(), entries.end());
  nodes_.clear();

  if (leaves_.empty()) {
    Crum root{};
    root.isLeaf     = 1;
    root.childCount = 0;
    nodes_.push_back(root);
    rootIndex_         = 0;
    cachedRootSummary_ = ArrayWid{.valence = valence_};
    return;
  }

  // Sort entries lexicographically by coordinates for cache locality and
  // ordinal descent
  std::sort(leaves_.begin(), leaves_.end(),
            [](const ArrayCellEntry &a, const ArrayCellEntry &b) {
              return a.coords < b.coords;
            });

  std::vector<std::uint32_t> currentLevelNodeIndices{};

  // Level 0: Leaf crums pointing to leaves_
  for (std::size_t i = 0; i < leaves_.size(); i += BranchingFactor) {
    Crum leafCrum{};
    leafCrum.isLeaf = 1;
    const auto count =
        std::min<std::size_t>(BranchingFactor, leaves_.size() - i);
    leafCrum.childCount = static_cast<std::uint8_t>(count);

    for (std::size_t k = 0; k < count; ++k) {
      const auto leafIdx = static_cast<std::uint32_t>(i + k);
      const auto &entry  = leaves_[leafIdx];

      ArrayWid entryWid{};
      entryWid.valence = valence_;
      entryWid.count   = 1;
      for (std::size_t d = 0; d < MaxValence; ++d) {
        entryWid.minCoord[d] = entry.coords[d];
        entryWid.maxCoord[d] = entry.coords[d];
      }

      if (entry.valueKind != xanadu::ValueKind::None) {
        entryWid.sum            = entry.numericValue;
        entryWid.minVal         = entry.numericValue;
        entryWid.maxVal         = entry.numericValue;
        entryWid.product        = entry.numericValue;
        entryWid.minScalar      = entry.numericValue;
        entryWid.maxScalar      = entry.numericValue;
        entryWid.scalarTypeMask = 1U
                                  << static_cast<std::uint8_t>(entry.valueKind);
      } else {
        entryWid.sum       = 0.0;
        entryWid.minVal    = std::numeric_limits<double>::infinity();
        entryWid.maxVal    = -std::numeric_limits<double>::infinity();
        entryWid.product   = 1.0;
        entryWid.minScalar = std::numeric_limits<double>::infinity();
        entryWid.maxScalar = -std::numeric_limits<double>::infinity();
        entryWid.scalarTypeMask =
            1U << static_cast<std::uint8_t>(xanadu::ValueKind::None);
      }

      if (0 != entry.textHash) {
        const auto h1        = entry.textHash;
        const auto h2        = h1 ^ (h1 >> 32);
        entryWid.bloomFilter = (1ULL << (h1 % 64)) |
                               (1ULL << ((h1 >> 8) % 64)) | (1ULL << (h2 % 64));
      }

      leafCrum.wids[k]     = entryWid;
      leafCrum.children[k] = leafIdx;
      leafCrum.dsps[k]     = ArrayDsp{.valence = valence_};
    }

    const auto nodeIndex = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(leafCrum);
    currentLevelNodeIndices.push_back(nodeIndex);
  }

  // Build interior levels bottom-up
  while (currentLevelNodeIndices.size() > 1) {
    std::vector<std::uint32_t> nextLevelIndices{};

    for (std::size_t i = 0; i < currentLevelNodeIndices.size();
         i += BranchingFactor) {
      Crum interiorCrum{};
      interiorCrum.isLeaf = 0;
      const auto count    = std::min<std::size_t>(
          BranchingFactor, currentLevelNodeIndices.size() - i);
      interiorCrum.childCount = static_cast<std::uint8_t>(count);

      const auto parentIdx =
          static_cast<std::uint32_t>(nodes_.size() + nextLevelIndices.size());

      for (std::size_t k = 0; k < count; ++k) {
        const auto childIdx          = currentLevelNodeIndices[i + k];
        interiorCrum.children[k]     = childIdx;
        interiorCrum.dsps[k]         = ArrayDsp{.valence = valence_};
        interiorCrum.wids[k]         = nodes_[childIdx].totalWid();
        nodes_[childIdx].parentIndex = parentIdx;
      }

      nextLevelIndices.push_back(static_cast<std::uint32_t>(nodes_.size()));
      nodes_.push_back(interiorCrum);
    }

    currentLevelNodeIndices = std::move(nextLevelIndices);
  }

  rootIndex_         = currentLevelNodeIndices.front();
  cachedRootSummary_ = nodes_[rootIndex_].totalWid();
}

// -- VPL Subscripting and Slicing ---------------------------------------------

std::optional<ArrayCellEntry> Arrayfilade::subscript(
    const std::array<std::int64_t, MaxValence> &coords) const {
  if (empty() || !cachedRootSummary_.containsCoord(coords)) {
    return std::nullopt;
  }
  return subscriptSubtree(rootIndex_, ArrayDsp{.valence = valence_}, coords);
}

std::optional<ArrayCellEntry> Arrayfilade::subscriptSubtree(
    const std::uint32_t nodeIdx, const ArrayDsp &cumDsp,
    const std::array<std::int64_t, MaxValence> &coords) const {
  const auto &crum = nodes_[nodeIdx];

  if (1 == crum.isLeaf) {
    for (std::size_t i = 0; i < crum.childCount; ++i) {
      const auto &entry = leaves_[crum.children[i]];
      if (entry.coords == coords) {
        return entry;
      }
    }
    return std::nullopt;
  }

  for (std::size_t i = 0; i < crum.childCount; ++i) {
    const auto effectiveWid = cumDsp.act(crum.dsps[i].act(crum.wids[i]));
    if (effectiveWid.containsCoord(coords)) {
      auto res = subscriptSubtree(crum.children[i],
                                  cumDsp.compose(crum.dsps[i]), coords);
      if (res.has_value()) {
        return res;
      }
    }
  }
  return std::nullopt;
}

std::optional<ArrayCellEntry>
Arrayfilade::subscript1D(const std::int64_t index) const {
  std::array<std::int64_t, MaxValence> coords{};
  coords[0] = index;
  return subscript(coords);
}

Arrayfilade
Arrayfilade::slice(const std::array<std::int64_t, MaxValence> &sliceMin,
                   const std::array<std::int64_t, MaxValence> &sliceMax) const {
  if (empty() || !cachedRootSummary_.overlapsBox(sliceMin, sliceMax)) {
    return Arrayfilade::fromEntries({}, valence_);
  }

  std::vector<ArrayCellEntry> slicedLeaves{};
  sliceSubtree(rootIndex_, ArrayDsp{.valence = valence_}, sliceMin, sliceMax,
               slicedLeaves);
  return Arrayfilade::fromEntries(slicedLeaves, valence_);
}

Arrayfilade Arrayfilade::slice1D(const std::int64_t start,
                                 const std::int64_t end) const {
  std::array<std::int64_t, MaxValence> sMin{};
  std::array<std::int64_t, MaxValence> sMax{};
  for (std::size_t i = 0; i < MaxValence; ++i) {
    sMin[i] = std::numeric_limits<std::int64_t>::lowest();
    sMax[i] = std::numeric_limits<std::int64_t>::max();
  }
  sMin[0] = start;
  sMax[0] = end;
  return slice(sMin, sMax);
}

void Arrayfilade::sliceSubtree(
    const std::uint32_t nodeIdx, const ArrayDsp &cumDsp,
    const std::array<std::int64_t, MaxValence> &sliceMin,
    const std::array<std::int64_t, MaxValence> &sliceMax,
    std::vector<ArrayCellEntry> &slicedLeaves) const {
  const auto &crum = nodes_[nodeIdx];

  if (1 == crum.isLeaf) {
    for (std::size_t i = 0; i < crum.childCount; ++i) {
      const auto &entry = leaves_[crum.children[i]];
      bool inRange      = true;
      for (std::size_t d = 0; d < valence_; ++d) {
        if (entry.coords[d] < sliceMin[d] || entry.coords[d] > sliceMax[d]) {
          inRange = false;
          break;
        }
      }
      if (inRange) {
        slicedLeaves.push_back(entry);
      }
    }
    return;
  }

  for (std::size_t i = 0; i < crum.childCount; ++i) {
    const auto effectiveWid = cumDsp.act(crum.dsps[i].act(crum.wids[i]));
    if (!effectiveWid.overlapsBox(sliceMin, sliceMax)) {
      continue;
    }
    sliceSubtree(crum.children[i], cumDsp.compose(crum.dsps[i]), sliceMin,
                 sliceMax, slicedLeaves);
  }
}

// -- O(1) Reductions ----------------------------------------------------------

double Arrayfilade::sum() const noexcept { return cachedRootSummary_.sum; }

double Arrayfilade::min() const noexcept { return cachedRootSummary_.minVal; }

double Arrayfilade::max() const noexcept { return cachedRootSummary_.maxVal; }

double Arrayfilade::product() const noexcept {
  return cachedRootSummary_.product;
}

std::uint64_t Arrayfilade::count() const noexcept {
  return cachedRootSummary_.count;
}

std::array<std::int64_t, MaxValence> Arrayfilade::shape() const noexcept {
  std::array<std::int64_t, MaxValence> shp{};
  if (empty()) {
    return shp;
  }
  for (std::size_t i = 0; i < valence_; ++i) {
    if (cachedRootSummary_.maxCoord[i] >= cachedRootSummary_.minCoord[i]) {
      shp[i] =
          cachedRootSummary_.maxCoord[i] - cachedRootSummary_.minCoord[i] + 1;
    }
  }
  return shp;
}

const ArrayWid &Arrayfilade::rootSummary() const noexcept {
  return cachedRootSummary_;
}

std::size_t Arrayfilade::depth() const noexcept {
  if (nodes_.empty()) {
    return 0;
  }
  std::size_t d        = 1;
  std::uint32_t curIdx = rootIndex_;
  while (0 == nodes_[curIdx].isLeaf && !nodes_[curIdx].empty()) {
    curIdx = nodes_[curIdx].children[0];
    ++d;
  }
  return d;
}

// -- VQL Query Planning -------------------------------------------------------

std::vector<ArrayCellEntry>
Arrayfilade::planQuery(const Predicate &pred) const {
  QueryPlanStats stats{};
  return planQueryWithStats(pred, stats);
}

std::vector<zigzag::CellRef>
Arrayfilade::planQueryCells(const Predicate &pred) const {
  const auto entries = planQuery(pred);
  std::vector<zigzag::CellRef> refs{};
  refs.reserve(entries.size());
  for (const auto &e : entries) {
    refs.push_back(e.cellRef);
  }
  return refs;
}

std::vector<ArrayCellEntry>
Arrayfilade::planQueryWithStats(const Predicate &pred,
                                QueryPlanStats &stats) const {
  std::vector<ArrayCellEntry> results{};
  if (empty()) {
    return results;
  }

  if (!pred.couldMatch(cachedRootSummary_)) {
    stats.subtreesExamined++;
    stats.subtreesPruned++;
    return results;
  }

  querySubtree(rootIndex_, ArrayDsp{.valence = valence_}, pred, results, stats);
  return results;
}

void Arrayfilade::querySubtree(const std::uint32_t nodeIdx,
                               const ArrayDsp &cumDsp, const Predicate &pred,
                               std::vector<ArrayCellEntry> &results,
                               QueryPlanStats &stats) const {
  stats.subtreesExamined++;
  const auto &crum = nodes_[nodeIdx];

  if (1 == crum.isLeaf) {
    for (std::size_t i = 0; i < crum.childCount; ++i) {
      stats.leavesExamined++;
      const auto &entry = leaves_[crum.children[i]];
      if (pred.matches(entry)) {
        results.push_back(entry);
        stats.leavesMatched++;
      }
    }
    return;
  }

  for (std::size_t i = 0; i < crum.childCount; ++i) {
    const auto effectiveWid = cumDsp.act(crum.dsps[i].act(crum.wids[i]));
    if (!pred.couldMatch(effectiveWid)) {
      stats.subtreesPruned++;
      continue;
    }
    querySubtree(crum.children[i], cumDsp.compose(crum.dsps[i]), pred, results,
                 stats);
  }
}

// -- Ruling R9 Verification ---------------------------------------------------

bool Arrayfilade::verifyAgainstLinearScan(
    const Predicate &pred, std::span<const ArrayCellEntry> linearCells) const {
  std::vector<ArrayCellEntry> expected{};
  for (const auto &cell : linearCells) {
    if (pred.matches(cell)) {
      expected.push_back(cell);
    }
  }

  const auto actual = planQuery(pred);
  if (expected.size() != actual.size()) {
    return false;
  }
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (expected[i].cellRef != actual[i].cellRef ||
        expected[i].coords != actual[i].coords ||
        expected[i].numericValue != actual[i].numericValue) {
      return false;
    }
  }
  return true;
}

bool Arrayfilade::verifyReductionsAgainstLinear(
    std::span<const ArrayCellEntry> linearCells) const {
  if (linearCells.empty()) {
    return 0 == count() && 0.0 == sum();
  }

  double expectedSum         = 0.0;
  double expectedMin         = std::numeric_limits<double>::infinity();
  double expectedMax         = -std::numeric_limits<double>::infinity();
  double expectedProduct     = 1.0;
  std::uint64_t validScalars = 0;

  for (const auto &cell : linearCells) {
    if (cell.valueKind != xanadu::ValueKind::None) {
      expectedSum += cell.numericValue;
      expectedMin = std::min(expectedMin, cell.numericValue);
      expectedMax = std::max(expectedMax, cell.numericValue);
      expectedProduct *= cell.numericValue;
      validScalars++;
    }
  }

  if (count() != linearCells.size()) {
    return false;
  }

  constexpr double eps = 1e-6;
  if (std::abs(sum() - expectedSum) > eps) {
    return false;
  }
  if (validScalars > 0) {
    if (std::abs(min() - expectedMin) > eps) {
      return false;
    }
    if (std::abs(max() - expectedMax) > eps) {
      return false;
    }
    if (std::abs(product() - expectedProduct) > eps) {
      return false;
    }
  }
  return true;
}

bool Arrayfilade::verifySubscriptAgainstLinear(
    std::span<const ArrayCellEntry> linearCells) const {
  for (const auto &cell : linearCells) {
    const auto res = subscript(cell.coords);
    if (!res.has_value()) {
      return false;
    }
    if (res->cellRef != cell.cellRef ||
        res->numericValue != cell.numericValue) {
      return false;
    }
  }
  return true;
}

bool Arrayfilade::verifySliceAgainstLinear(
    const std::array<std::int64_t, MaxValence> &sliceMin,
    const std::array<std::int64_t, MaxValence> &sliceMax,
    std::span<const ArrayCellEntry> linearCells) const {
  std::vector<ArrayCellEntry> expected{};
  for (const auto &cell : linearCells) {
    bool inRange = true;
    for (std::size_t d = 0; d < valence_; ++d) {
      if (cell.coords[d] < sliceMin[d] || cell.coords[d] > sliceMax[d]) {
        inRange = false;
        break;
      }
    }
    if (inRange) {
      expected.push_back(cell);
    }
  }

  const auto slicedFilade = slice(sliceMin, sliceMax);
  if (slicedFilade.count() != expected.size()) {
    return false;
  }

  for (const auto &cell : expected) {
    const auto found = slicedFilade.subscript(cell.coords);
    if (!found.has_value() || found->cellRef != cell.cellRef) {
      return false;
    }
  }
  return true;
}

} // namespace xanadu::enfilade
