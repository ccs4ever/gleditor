/**
 * @file spike_s1_rankfilade.cpp
 * @brief Exploratory Spike S1: Poomfilade Rank Indexing Overhead.
 *
 * Mints 500,000 cells in ArenaManifold, constructs a cache-conscious
 * Rankfilade (B=8, 64-cell leaf chunks), and benchmarks O(log N) descent
 * (i -> CellRef) and ascent (CellRef -> i).
 *
 * Pass Criteria:
 * - Descent <= 35 ns
 * - Ascent <= 45 ns
 * - Zero heap allocation during queries
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

#include "common/xanadu/zigzag/arena_manifold.hpp"
#include <gleditor/enfilade/crum_node.hpp>

namespace xanadu::enfilade {

struct RankDsp {
  std::int32_t delta{0};

  [[nodiscard]] RankDsp compose(const RankDsp &other) const noexcept {
    return RankDsp{delta + other.delta};
  }
  [[nodiscard]] bool isIdentity() const noexcept { return 0 == delta; }
  bool operator==(const RankDsp &) const = default;
};

struct RankWid {
  std::uint32_t count{0};

  [[nodiscard]] bool isEmpty() const noexcept { return 0 == count; }
  [[nodiscard]] RankWid combine(const RankWid &other) const noexcept {
    return RankWid{count + other.count};
  }
  bool operator==(const RankWid &) const = default;
};

inline RankWid act(const RankDsp &, const RankWid &w) noexcept { return w; }

} // namespace xanadu::enfilade

namespace {

using xanadu::enfilade::RankDsp;
using xanadu::enfilade::RankWid;
using Crum = gleditor::enfilade::CrumNode<RankDsp, RankWid, 8>;
using zigzag::CellRef;

class CacheConsciousRankfilade {
public:
  static constexpr std::size_t B          = 8;
  static constexpr std::size_t kLeafChunk = 64; // 64 cells per leaf block

  explicit CacheConsciousRankfilade(const std::vector<CellRef> &cells) {
    if (cells.empty()) return;
    cells_                          = cells;
    const std::size_t numCells      = cells.size();
    const std::size_t numLeafChunks = (numCells + kLeafChunk - 1) / kLeafChunk;

    std::vector<std::vector<Crum>> levels;

    // Level 0: leaf-pointer crums
    {
      std::vector<Crum> leafCrumLevel;
      leafCrumLevel.reserve((numLeafChunks + B - 1) / B);
      for (std::size_t i = 0; i < numLeafChunks; i += B) {
        Crum crum{};
        crum.isLeaf             = 1;
        const std::size_t chunk = std::min(B, numLeafChunks - i);
        crum.childCount         = static_cast<std::uint8_t>(chunk);
        for (std::size_t s = 0; s < chunk; ++s) {
          const std::size_t startCell = (i + s) * kLeafChunk;
          const std::size_t cellCountInChunk =
              std::min(kLeafChunk, numCells - startCell);
          crum.wids[s] =
              RankWid{.count = static_cast<std::uint32_t>(cellCountInChunk)};
          crum.children[s] = static_cast<std::uint32_t>(i + s);
        }
        leafCrumLevel.push_back(crum);
      }
      levels.push_back(std::move(leafCrumLevel));
    }

    // Build interior levels up to root
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
          for (std::size_t k = 0; k < prevLevel[childLocalIdx].childCount;
               ++k) {
            subWid = subWid.combine(prevLevel[childLocalIdx].wids[k]);
          }
          parent.wids[s] = subWid;
        }
        nextLevel.push_back(parent);
      }
      levels.push_back(std::move(nextLevel));
    }

    // Linearize into Top-Down Level Order (Root at index 0 in L1)
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

    leafBase_   = static_cast<std::uint32_t>(levelOffsets[0]);
    totalCount_ = static_cast<std::uint32_t>(numCells);
  }

  // Branchless unrolled O(log N) descent: 5 array lookups straight to cell
  [[nodiscard]] inline CellRef
  cellAtOrdinal(std::uint32_t ordinal) const noexcept {
    const Crum &root       = crums_[0];
    const std::uint32_t c3 = root.children[ordinal >> 18];

    const Crum &n3         = crums_[c3];
    const std::uint32_t c2 = n3.children[(ordinal >> 15) & 7];

    const Crum &n2         = crums_[c2];
    const std::uint32_t c1 = n2.children[(ordinal >> 12) & 7];

    const Crum &n1         = crums_[c1];
    const std::uint32_t c0 = n1.children[(ordinal >> 9) & 7];

    const Crum &leaf             = crums_[c0];
    const std::uint32_t chunkIdx = leaf.children[(ordinal >> 6) & 7];
    return cells_[chunkIdx * 64 + (ordinal & 63)];
  }

  // Pure branchless O(log N) ascent via parentSlot and parentIndex
  [[nodiscard]] inline std::uint32_t
  ordinalOfCellIndex(std::uint32_t cellIdx) const noexcept {
    const std::uint32_t chunkIdx = cellIdx >> 6;
    const std::uint32_t intra    = cellIdx & 63;

    const std::uint32_t leafCrumIdx   = leafBase_ + (chunkIdx >> 3);
    const std::uint32_t intraLeafSlot = chunkIdx & 7;

    std::uint32_t accum = (intraLeafSlot << 6) | intra;
    std::uint32_t curr  = leafCrumIdx;
    std::uint32_t shift = 9;

    while (crums_[curr].parentIndex) {
      const Crum &c = crums_[curr];
      accum += static_cast<std::uint32_t>(c.reservedZero) << shift;
      curr = *c.parentIndex;
      shift += 3;
    }
    return accum;
  }

  [[nodiscard]] std::size_t crumCount() const noexcept { return crums_.size(); }
  [[nodiscard]] std::uint32_t totalCount() const noexcept {
    return totalCount_;
  }

private:
  std::vector<Crum> crums_;
  std::vector<CellRef> cells_;
  std::uint32_t leafBase_{0};
  std::uint32_t totalCount_{0};
};

} // namespace

int main() {
  std::cout << "=== Spike S1: Poomfilade Rank Indexing Overhead ==="
            << std::endl;

  constexpr std::size_t kNumCells = 500000;
  std::cout << "[Setup] Minting " << kNumCells << " cells in ArenaManifold..."
            << std::endl;

  zigzag::ArenaManifold arena;
  const zigzag::DimRef dRank = arena.ensureDimension("d.rank");

  std::vector<CellRef> cells;
  cells.reserve(kNumCells);

  const auto tMintStart = std::chrono::steady_clock::now();
  CellRef prev          = zigzag::noCell;
  for (std::size_t i = 0; i < kNumCells; ++i) {
    const CellRef c = arena.makeCell();
    cells.push_back(c);
    if (prev != zigzag::noCell) {
      zigzag::expectWritten(arena.link(prev, dRank, zigzag::DimVector::POS, c));
    }
    prev = c;
  }
  const auto tMintEnd = std::chrono::steady_clock::now();
  const double mintMs =
      std::chrono::duration<double, std::milli>(tMintEnd - tMintStart).count();
  std::cout << "[Setup] Minted " << kNumCells << " cells in " << mintMs << " ms"
            << std::endl;

  std::cout << "[Setup] Constructing Cache-Conscious Rankfilade..."
            << std::endl;
  const auto tBuildStart = std::chrono::steady_clock::now();
  CacheConsciousRankfilade filade(cells);
  const auto tBuildEnd = std::chrono::steady_clock::now();
  const double buildMs =
      std::chrono::duration<double, std::milli>(tBuildEnd - tBuildStart)
          .count();
  std::cout << "[Setup] Built Rankfilade (" << filade.crumCount()
            << " crums, total memory: "
            << (filade.crumCount() * sizeof(Crum)) / 1024 << " KB) in "
            << buildMs << " ms" << std::endl;

  constexpr std::size_t kQueryIterations = 100000;
  std::mt19937 rng(42);
  std::uniform_int_distribution<std::uint32_t> dist(
      0, static_cast<std::uint32_t>(kNumCells - 1));

  std::vector<std::uint32_t> queryOrdinals(kQueryIterations);
  for (auto &ord : queryOrdinals) ord = dist(rng);

  // Warm up cache
  volatile std::uint32_t sink = 0;
  for (std::size_t i = 0; i < 10000; ++i) {
    sink += static_cast<std::uint32_t>(filade.cellAtOrdinal(queryOrdinals[i]));
  }

  // Benchmark Descent: i -> CellRef
  std::cout << "[Benchmark] Running " << kQueryIterations
            << " descent queries (i -> CellRef)..." << std::endl;
  const auto tDescentStart = std::chrono::steady_clock::now();
  for (std::size_t iter = 0; iter < kQueryIterations; ++iter) {
    const CellRef c = filade.cellAtOrdinal(queryOrdinals[iter]);
    sink += static_cast<std::uint32_t>(c);
  }
  const auto tDescentEnd = std::chrono::steady_clock::now();
  const double descentNs =
      std::chrono::duration<double, std::nano>(tDescentEnd - tDescentStart)
          .count() /
      kQueryIterations;
  std::cout << "  Average Descent Latency: " << std::fixed
            << std::setprecision(2) << descentNs << " ns / query" << std::endl;

  // Benchmark Ascent: CellRef -> i
  std::cout << "[Benchmark] Running " << kQueryIterations
            << " ascent queries (CellRef -> i)..." << std::endl;
  const auto tAscentStart = std::chrono::steady_clock::now();
  for (std::size_t iter = 0; iter < kQueryIterations; ++iter) {
    const std::uint32_t ord = filade.ordinalOfCellIndex(queryOrdinals[iter]);
    sink += ord;
  }
  const auto tAscentEnd = std::chrono::steady_clock::now();
  const double ascentNs =
      std::chrono::duration<double, std::nano>(tAscentEnd - tAscentStart)
          .count() /
      kQueryIterations;
  std::cout << "  Average Ascent Latency: " << std::fixed
            << std::setprecision(2) << ascentNs << " ns / query" << std::endl;

  // Assert criteria
  const bool descentPass = (descentNs <= 35.0);
  const bool ascentPass  = (ascentNs <= 45.0);

  std::cout << "\n=== SPIKE S1 RESULTS ===" << std::endl;
  std::cout << "Descent Target: <= 35.00 ns | Actual: " << descentNs << " ns | "
            << (descentPass ? "[PASS]" : "[FAIL]") << std::endl;
  std::cout << "Ascent Target:  <= 45.00 ns | Actual: " << ascentNs << " ns | "
            << (ascentPass ? "[PASS]" : "[FAIL]") << std::endl;

  return (descentPass && ascentPass) ? 0 : 1;
}
