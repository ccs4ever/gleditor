/**
 * @file structure_hyperop_benchmark_test.cpp
 * @brief Reproducible pre-implementation baselines for the Structure plans.
 *
 * These are deliberately disabled: they print measurements, not CI pass/fail
 * performance thresholds. Federation proxies and quotation occurrences do not
 * exist yet. The arena-cell scenarios below measure only their proposed storage
 * substrates, not identity semantics, the resolver or delegated read path.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <vector>

#include <xudu/core/store.hpp>
#include <zigzag/core/arena_manifold.hpp>

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double millis(const Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}

template <typename Hop>
[[nodiscard]] double nsPerHop(const zigzag::CellRef startCell,
                              const std::size_t hopCount, Hop &&hop) {
  constexpr int repetitions = 20;
  zigzag::CellRef sink      = zigzag::noCell;
  const auto start          = Clock::now();
  for (int repeat = 0; repeat < repetitions; ++repeat) {
    auto cursor = startCell;
    for (std::size_t i = 0; i < hopCount; ++i) {
      const auto next = hop(cursor);
      cursor          = next == zigzag::noCell ? startCell : next;
    }
    sink ^= cursor;
  }
  const auto elapsed =
      std::chrono::duration<double, std::nano>(Clock::now() - start).count();
  EXPECT_NE(sink, zigzag::noCell - 1U);
  return elapsed / static_cast<double>(hopCount * repetitions);
}

TEST(StructureHyperopBenchmark, DISABLED_ColdFoldAndArenaSubstrate) {
  constexpr std::size_t cellCount = 10000;
  constexpr int foldRepeats       = 20;
  constexpr std::size_t selected  = 100;

  xanadu::Store store;
  auto at              = store.sliceGenesis(xanadu::MicroversionId{});
  const auto mintedDim = store.makeDimension(at, "d.benchmark");
  at                   = mintedDim.version;
  const auto dim       = mintedDim.dim;

  std::vector<zigzag::CellRef> cells;
  cells.reserve(cellCount);
  for (std::size_t i = 0; i < cellCount; ++i) {
    at = store.makeCell(at, "c");
    cells.push_back(store.cellRefOf(at));
  }
  for (std::size_t i = 1; i < cellCount; ++i) {
    at = store.setLink(at, cells[i - 1], dim, zigzag::DimVector::POS, cells[i]);
  }

  double totalFoldMs      = 0.0;
  std::size_t foldedCells = 0;
  for (int repeat = 0; repeat < foldRepeats; ++repeat) {
    const auto start = Clock::now();
    auto manifold    = store.rebuildManifold(at);
    totalFoldMs += millis(start);
    foldedCells = manifold.cellCount();
  }
  std::cout << "cold fold: ops=" << store.opCount() << " cells=" << foldedCells
            << " repeats=" << foldRepeats
            << " mean_ms=" << totalFoldMs / foldRepeats << '\n';

  auto base = store.rebuildManifold(at);
  zigzag::ArenaManifold readThrough(&base);
  std::unordered_map<zigzag::CellRef, zigzag::CellRef> proxyToForeign;
  std::unordered_map<zigzag::CellRef, zigzag::CellRef> foreignToProxy;
  proxyToForeign.reserve(cellCount);
  foreignToProxy.reserve(cellCount);
  for (const auto cell : cells) {
    proxyToForeign.emplace(cell, cell);
    foreignToProxy.emplace(cell, cell);
  }
  const auto directNs =
      nsPerHop(cells.front(), cellCount, [&](const auto cell) {
        return base.linked(cell, dim, zigzag::DimVector::POS);
      });
  const auto arenaNs = nsPerHop(cells.front(), cellCount, [&](const auto cell) {
    return readThrough.linked(cell, dim, zigzag::DimVector::POS);
  });
  const auto mapModelNs =
      nsPerHop(cells.front(), cellCount, [&](const auto proxy) {
        const auto foreign = proxyToForeign.at(proxy);
        const auto next    = base.linked(foreign, dim, zigzag::DimVector::POS);
        return next == zigzag::noCell ? next : foreignToProxy.at(next);
      });
  std::cout << "hop baseline/model: direct_ns=" << directNs
            << " arena_readthrough_ns=" << arenaNs
            << " two_map_model_ns=" << mapModelNs << '\n';

  for (const std::size_t proxyCount : {100U, 1000U, 10000U}) {
    zigzag::ArenaManifold arena(&base);
    std::unordered_map<zigzag::CellRef, zigzag::CellRef> byForeign;
    byForeign.reserve(proxyCount);
    const auto start         = Clock::now();
    zigzag::CellRef previous = zigzag::noCell;
    for (std::size_t i = 0; i < proxyCount; ++i) {
      const auto proxy = arena.makeCell();
      byForeign.emplace(cells[i], proxy);
      if (previous != zigzag::noCell) {
        EXPECT_TRUE(arena.link(previous, dim, zigzag::DimVector::POS, proxy));
      }
      previous = proxy;
    }
    const double elapsedMs = millis(start);
    std::cout << "arena proxy substrate: touched=" << proxyCount
              << " own_cells=" << arena.cellCount()
              << " map_entries=" << byForeign.size()
              << " map_buckets=" << byForeign.bucket_count()
              << " elapsed_ms=" << elapsedMs << '\n';
  }

  for (const std::size_t quoteCount : {1U, 2U, 4U, 8U}) {
    zigzag::ArenaManifold arena(&base);
    const auto start        = Clock::now();
    std::size_t linkedEdges = 0;
    for (std::size_t quote = 0; quote < quoteCount; ++quote) {
      zigzag::CellRef previous = zigzag::noCell;
      for (std::size_t i = 0; i < selected; ++i) {
        const auto occurrence = arena.makeCell();
        if (previous != zigzag::noCell) {
          EXPECT_TRUE(
              arena.link(previous, dim, zigzag::DimVector::POS, occurrence));
          ++linkedEdges;
        }
        previous = occurrence;
      }
    }
    const double elapsedMs = millis(start);
    std::cout << "arena occurrence substrate: selected=" << selected
              << " quotes=" << quoteCount << " own_cells=" << arena.cellCount()
              << " linked_edges=" << linkedEdges << " elapsed_ms=" << elapsedMs
              << " cell_slot_bytes=" << sizeof(zigzag::CellSlot) << '\n';
  }
}

} // namespace
