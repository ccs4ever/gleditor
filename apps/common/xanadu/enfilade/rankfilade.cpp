/**
 * @file rankfilade.cpp
 * @brief Implementation of the Rankfilade 1D B-enfilade.
 */
#include "common/xanadu/enfilade/rankfilade.hpp"

#include <algorithm>

namespace xanadu::enfilade {

Rankfilade::Rankfilade(const std::span<const zigzag::CellRef> cells)
    : cells_(cells.begin(), cells.end()) {
  buildTree();
}

void Rankfilade::buildTree() {
  crums_.clear();
  handles_.clear();
  leafBase_   = 0;
  totalCount_ = static_cast<std::uint32_t>(cells_.size());

  if (cells_.empty()) {
    return;
  }

  constexpr std::size_t B = BranchingFactor;
  constexpr std::size_t K = LeafChunkSize;

  const std::size_t numCells      = cells_.size();
  const std::size_t numLeafChunks = (numCells + K - 1) / K;

  std::vector<std::vector<Crum>> levels;

  // 1. Level 0: leaf-pointer crums
  {
    std::vector<Crum> leafLevel;
    leafLevel.reserve((numLeafChunks + B - 1) / B);

    for (std::size_t i = 0; i < numLeafChunks; i += B) {
      Crum crum{};
      crum.isLeaf             = 1;
      const std::size_t chunk = std::min(B, numLeafChunks - i);
      crum.childCount         = static_cast<std::uint8_t>(chunk);

      for (std::size_t s = 0; s < chunk; ++s) {
        const std::size_t startCell    = (i + s) * K;
        const std::size_t countInChunk = std::min(K, numCells - startCell);
        crum.wids[s] =
            RankWid{.count = static_cast<std::uint32_t>(countInChunk)};
        crum.children[s] = static_cast<std::uint32_t>(i + s); // chunk index
      }
      leafLevel.push_back(crum);
    }
    levels.push_back(std::move(leafLevel));
  }

  // 2. Interior levels bottom-up
  while (levels.back().size() > 1) {
    const auto &prevLevel = levels.back();
    std::vector<Crum> nextLevel;
    nextLevel.reserve((prevLevel.size() + B - 1) / B);

    for (std::size_t i = 0; i < prevLevel.size(); i += B) {
      Crum parent{};
      parent.isLeaf           = 0;
      const std::size_t chunk = std::min(B, prevLevel.size() - i);
      parent.childCount       = static_cast<std::uint8_t>(chunk);

      for (std::size_t s = 0; s < chunk; ++s) {
        const auto childLocalIdx = static_cast<std::uint32_t>(i + s);
        parent.children[s]       = childLocalIdx;
        RankWid subWid{};
        for (std::size_t k = 0; k < prevLevel[childLocalIdx].childCount; ++k) {
          subWid = subWid.combine(prevLevel[childLocalIdx].wids[k]);
        }
        parent.wids[s] = subWid;
      }
      nextLevel.push_back(parent);
    }
    levels.push_back(std::move(nextLevel));
  }

  // 3. Linearize in top-down level order (Root at index 0)
  const std::size_t numLevels = levels.size();
  std::vector<std::size_t> levelOffsets(numLevels);

  std::size_t offset = 0;
  for (std::size_t l = 0; l < numLevels; ++l) {
    const std::size_t lvl = numLevels - 1 - l;
    levelOffsets[lvl]     = offset;
    offset += levels[lvl].size();
  }

  crums_.resize(offset);

  for (std::size_t lvl = 0; lvl < numLevels; ++lvl) {
    const std::size_t base = levelOffsets[lvl];
    for (std::size_t i = 0; i < levels[lvl].size(); ++i) {
      Crum c               = levels[lvl][i];
      const auto globalIdx = static_cast<std::uint32_t>(base + i);

      if (lvl > 0) {
        const std::size_t childBase = levelOffsets[lvl - 1];
        for (std::size_t s = 0; s < c.childCount; ++s) {
          const std::uint32_t childLocal = c.children[s];
          const auto childGlobal =
              static_cast<std::uint32_t>(childBase + childLocal);
          c.children[s]                           = childGlobal;
          levels[lvl - 1][childLocal].parentIndex = globalIdx;
          levels[lvl - 1][childLocal].reservedZero =
              static_cast<std::uint16_t>(s);
        }
      }
      crums_[globalIdx] = c;
    }
  }

  for (std::size_t lvl = 0; lvl < numLevels - 1; ++lvl) {
    const std::size_t base = levelOffsets[lvl];
    for (std::size_t i = 0; i < levels[lvl].size(); ++i) {
      crums_[base + i].parentIndex  = levels[lvl][i].parentIndex;
      crums_[base + i].reservedZero = levels[lvl][i].reservedZero;
    }
  }

  leafBase_ = static_cast<std::uint32_t>(levelOffsets[0]);

  // 4. Populate LeafHandle table for O(log N) / O(1) ascent
  handles_.reserve(numCells);
  for (std::size_t i = 0; i < numCells; ++i) {
    const std::size_t chunkIdx      = i / K;
    const std::size_t leafLocalIdx  = chunkIdx / B;
    const std::size_t slotInLeaf    = chunkIdx % B;
    const std::size_t offsetInChunk = i % K;

    const auto leafGlobal =
        static_cast<std::uint32_t>(leafBase_ + leafLocalIdx);
    handles_[cells_[i]] = LeafHandle{
        .leafIndex     = leafGlobal,
        .chunkSlot     = static_cast<std::uint16_t>(slotInLeaf),
        .offsetInChunk = static_cast<std::uint16_t>(offsetInChunk),
    };
  }
}

std::optional<zigzag::CellRef>
Rankfilade::cellAtOrdinal(const std::size_t ordinal) const noexcept {
  if (ordinal >= cells_.size() || crums_.empty()) {
    return std::nullopt;
  }

  std::uint32_t currIdx = 0; // Root is at index 0
  std::size_t rem       = ordinal;

  while (crums_[currIdx].isLeaf == 0) {
    const auto &crum = crums_[currIdx];
    bool stepped     = false;

    for (std::size_t s = 0; s < crum.childCount; ++s) {
      const std::size_t count = crum.wids[s].count;
      if (rem < count) {
        currIdx = crum.children[s];
        stepped = true;
        break;
      }
      rem -= count;
    }

    if (!stepped) {
      return std::nullopt;
    }
  }

  // At leaf crum
  const auto &leaf = crums_[currIdx];
  for (std::size_t s = 0; s < leaf.childCount; ++s) {
    const std::size_t count = leaf.wids[s].count;
    if (rem < count) {
      const std::size_t chunkIdx = leaf.children[s];
      const std::size_t cellIdx  = chunkIdx * LeafChunkSize + rem;
      if (cellIdx < cells_.size()) {
        return cells_[cellIdx];
      }
      return std::nullopt;
    }
    rem -= count;
  }

  return std::nullopt;
}

std::optional<std::size_t>
Rankfilade::ordinalOfCell(const zigzag::CellRef cell) const noexcept {
  const auto it = handles_.find(cell);
  if (it == handles_.end()) {
    return std::nullopt;
  }

  const auto &h       = it->second;
  std::size_t ordinal = h.offsetInChunk;

  // Add preceding sibling chunks in this leaf
  const auto &leaf = crums_[h.leafIndex];
  for (std::size_t s = 0; s < h.chunkSlot && s < leaf.childCount; ++s) {
    ordinal += leaf.wids[s].count;
  }

  // Climb up parent pointer chain
  std::uint32_t currIdx = h.leafIndex;
  while (currIdx != 0) {
    const std::uint16_t slotInParent = crums_[currIdx].reservedZero;
    const std::uint32_t parentIdx    = crums_[currIdx].parentIndex;
    const auto &parent               = crums_[parentIdx];

    for (std::size_t s = 0; s < slotInParent && s < parent.childCount; ++s) {
      ordinal += parent.wids[s].count;
    }
    currIdx = parentIdx;
  }

  return ordinal;
}

float Rankfilade::scrollbarThumbRatio(
    const zigzag::CellRef cell) const noexcept {
  if (cells_.size() <= 1) {
    return 0.0F;
  }
  const auto ord = ordinalOfCell(cell);
  if (!ord) {
    return 0.0F;
  }
  return static_cast<float>(*ord) / static_cast<float>(cells_.size() - 1);
}

bool Rankfilade::verifyAgainstLinearScan() const noexcept {
  for (std::size_t i = 0; i < cells_.size(); ++i) {
    const auto c = cellAtOrdinal(i);
    if (!c || *c != cells_[i]) {
      return false;
    }
    const auto ord = ordinalOfCell(cells_[i]);
    if (!ord || *ord != i) {
      return false;
    }
  }
  return true;
}

} // namespace xanadu::enfilade
