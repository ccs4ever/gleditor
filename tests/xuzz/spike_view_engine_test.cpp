/**
 * @file spike_view_engine_test.cpp
 * @brief THROWAWAY spikes S1, S3, S5 of
 * design/view-system-implementation-plan.md §3. Not for merging: measurements
 * only, results printed to stdout.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/store_loader.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace zigzag {
extern bool spikeReleaseGuard;
extern std::atomic<std::uint64_t> spikeArenaDimensionNamedCalls;
extern std::atomic<std::uint64_t> spikeManifoldDimensionNamedCalls;
extern std::atomic<std::uint64_t> spikeManifoldDimensionScans;
} // namespace zigzag

namespace {

namespace fs = std::filesystem;
using Clock  = std::chrono::steady_clock;
using xanadu::MicroversionId;
using xanadu::PrimediaSpan;
using xanadu::Store;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimVector;

double nsSince(const Clock::time_point start) {
  return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}

struct Stats {
  double median{0};
  double p10{0};
  double p90{0};
  double min{0};
  double max{0};
};

Stats stats(std::vector<double> v) {
  std::ranges::sort(v);
  const auto at = [&](double q) {
    return v[static_cast<std::size_t>(q * static_cast<double>(v.size() - 1))];
  };
  return {at(0.5), at(0.1), at(0.9), v.front(), v.back()};
}

fs::path scratchDir(const std::string &name) {
  const auto dir = fs::temp_directory_path() / ("gleditor-spike-" + name);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

std::shared_ptr<xanadu::UserPermascroll> freshScroll(const std::string &name) {
  xanadu::UserPermascroll::Config config;
  config.storageDir = scratchDir(name);
  return std::make_shared<xanadu::UserPermascroll>(std::move(config));
}

// --------------------------------------------------------------------------
// S1
// --------------------------------------------------------------------------

/// A small real slice: home, d.dims, two dimensions, a few linked cells.
struct SmallSlice {
  std::unique_ptr<Store> store;
  MicroversionId at;
  SmallSlice() : store(std::make_unique<Store>(freshScroll("s1"))) {
    at            = store->sliceGenesis(MicroversionId{});
    const auto d1 = store->makeDimension(at, "d.1");
    at            = d1.version;
    const auto d2 = store->makeDimension(at, "d.2");
    at            = d2.version;
    CellRef prev  = zigzag::noCell;
    for (int i = 0; i < 8; ++i) {
      at              = store->makeCell(at, "c" + std::to_string(i));
      const auto cell = store->cellRefOf(at);
      if (prev != zigzag::noCell) {
        at = store->setLink(at, prev, d1.dim, DimVector::POS, cell);
      }
      prev = cell;
    }
  }
};

struct ReleaseRun {
  Stats ns;
  std::uint64_t arenaLookups{0};
  std::uint64_t baseLookups{0};
  std::uint64_t baseScans{0};
  std::size_t shadowAfterMint{0};
  std::size_t trailAfterMint{0};
  std::size_t cellsAfterRelease{0};
};

/// Evicts the caches, so a small arena's release() can be timed as cold as a
/// large one's is after minting a million cells.
void flushCaches() {
  static std::vector<std::uint64_t> junk(8UZ << 20); // 64 MiB
  std::uint64_t acc = 0;
  for (std::size_t i = 0; i < junk.size(); i += 8) {
    junk[i] += i;
    acc += junk[i];
  }
  asm volatile("" : : "r"(acc) : "memory");
}

ReleaseRun timeRelease(const zigzag::Manifold &base, const std::size_t minted,
                       const int reps, const bool linkThem,
                       const bool cold = false) {
  ArenaManifold arena(&base, nullptr);
  EXPECT_EQ(arena.spaceCount(), 0U);
  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(reps));
  ReleaseRun run;
  for (int r = 0; r < reps; ++r) {
    const auto empty = arena.mark();
    if (minted > 0) {
      // A view-owned dimension: a bare cell used as a link key.
      const auto dim = arena.makeCell();
      CellRef prev   = zigzag::noCell;
      for (std::size_t i = 0; i + 1 < minted; ++i) {
        const auto cell = arena.makeCell();
        if (linkThem && prev != zigzag::noCell && (i % 2) == 0) {
          zigzag::expectWritten(arena.link(prev, dim, DimVector::POS, cell));
        }
        prev = cell;
      }
    }
    // shadowCount via a nested mark: the count it records is
    // shadowOrder_.size().
    const auto probe    = arena.mark();
    run.shadowAfterMint = probe.shadowCount;
    run.trailAfterMint  = arena.trailSize();
    std::ignore         = arena.discard(probe);
    if (cold) {
      flushCaches();
    }
    const auto aBefore = zigzag::spikeArenaDimensionNamedCalls.load();
    const auto bBefore = zigzag::spikeManifoldDimensionNamedCalls.load();
    const auto sBefore = zigzag::spikeManifoldDimensionScans.load();
    const auto start   = Clock::now();
    std::ignore        = arena.release(empty);
    samples.push_back(nsSince(start));
    run.arenaLookups += zigzag::spikeArenaDimensionNamedCalls.load() - aBefore;
    run.baseLookups +=
        zigzag::spikeManifoldDimensionNamedCalls.load() - bBefore;
    run.baseScans += zigzag::spikeManifoldDimensionScans.load() - sBefore;
    run.cellsAfterRelease = arena.cellCount();
  }
  run.ns = stats(std::move(samples));
  return run;
}

TEST(SpikeViewEngine, S1_ReleaseOnAViewShapedArena) {
  static_assert(std::is_trivially_destructible_v<zigzag::CellSlot>);
  static_assert(std::is_trivially_destructible_v<zigzag::DimLink>);
  static_assert(std::is_trivially_destructible_v<xanadu::PrimediaSpan>);

  const SmallSlice slice;
  const auto base = slice.store->rebuildManifold(slice.at);
  std::printf("S1 base: %zu cells, %zu dims\n", base.cellCount(),
              base.dimensions().size());

  for (const bool guard : {false, true}) {
    zigzag::spikeReleaseGuard = guard;
    for (const bool linkThem : {false, true}) {
      for (const std::size_t n : {0UZ, 1000UZ, 1000000UZ}) {
        const int reps = n >= 1000000 ? 15 : 2000;
        // Warm-up: grow the vectors once so the timed runs are steady state.
        std::ignore    = timeRelease(base, n, n >= 1000000 ? 1 : 10, linkThem);
        const auto run = timeRelease(base, n, reps, linkThem);
        std::printf(
            "S1 guard=%d linked=%d minted=%7zu reps=%4d release ns: median "
            "%9.0f p10 %9.0f p90 %9.0f min %9.0f max %9.0f | lookups/release: "
            "arena %.2f base %.2f base-scans %.2f | shadow %zu trail %zu "
            "cellsAfter %zu\n",
            guard, linkThem, n, reps, run.ns.median, run.ns.p10, run.ns.p90,
            run.ns.min, run.ns.max,
            static_cast<double>(run.arenaLookups) / reps,
            static_cast<double>(run.baseLookups) / reps,
            static_cast<double>(run.baseScans) / reps, run.shadowAfterMint,
            run.trailAfterMint, run.cellsAfterRelease);
        EXPECT_EQ(run.shadowAfterMint, 0U);
        EXPECT_EQ(run.trailAfterMint, 0U);
        EXPECT_EQ(run.cellsAfterRelease, 0U);
        if (guard) {
          EXPECT_EQ(run.arenaLookups, 0U);
          EXPECT_EQ(run.baseLookups, 0U);
        }
      }
    }
  }
  // Cold-cache control: is the residual growth at 10^6 cache misses or work?
  for (const bool guard : {false, true}) {
    zigzag::spikeReleaseGuard = guard;
    for (const std::size_t n : {0UZ, 1000UZ, 100000UZ, 1000000UZ, 3000000UZ}) {
      const auto run = timeRelease(base, n, 15, true, true);
      std::printf("S1 COLD guard=%d linked=1 minted=%7zu release ns: median "
                  "%9.0f p10 %9.0f p90 %9.0f\n",
                  guard, n, run.ns.median, run.ns.p10, run.ns.p90);
    }
  }
  zigzag::spikeReleaseGuard = false;
}

// --------------------------------------------------------------------------
// S3
// --------------------------------------------------------------------------

CellRef ancestor(const Store &store, CellRef index, int steps) {
  for (int i = 0; i < steps && index > 1; ++i) {
    const auto *node = store.getCompactOp(index);
    if (node == nullptr || node->parentIndex == 0) {
      break;
    }
    index = node->parentIndex;
  }
  return index;
}

std::size_t structureOpsUpTo(const Store &store, CellRef index) {
  std::size_t n = 0;
  while (index != 0) {
    const auto *node = store.getCompactOp(index);
    if (node == nullptr) {
      break;
    }
    if (node->kind == xanadu::OpKind::Structure) {
      ++n;
    }
    index = node->parentIndex;
  }
  return n;
}

void timeRebuilds(const char *label, const Store &store, int reps) {
  const auto latest = store.cellRefOf(store.latest());
  std::printf("S3 %s: opCount %zu, latest index %u\n", label, store.opCount(),
              latest);
  for (const int back : {0, 1, 5, 50}) {
    const auto idx = ancestor(store, latest, back);
    std::vector<double> samples;
    std::size_t cells = 0;
    for (int r = 0; r < reps; ++r) {
      const auto start = Clock::now();
      const auto m     = store.rebuildManifoldFromIndex(idx);
      samples.push_back(nsSince(start));
      cells = m.cellCount();
    }
    const auto s = stats(samples);
    std::printf("S3 %s latest-%-2d idx %7u structOps-on-path %7zu cells %7zu "
                "rebuildManifold ms: median %.3f p10 %.3f p90 %.3f\n",
                label, back, idx, structureOpsUpTo(store, idx), cells,
                s.median / 1e6, s.p10 / 1e6, s.p90 / 1e6);
  }
  // The "keep a recent state" alternative: the step forward is advance().
  if (latest > 1) {
    const auto prev = ancestor(store, latest, 1);
    std::vector<double> samples;
    const auto latestId = store.latest();
    for (int r = 0; r < reps; ++r) {
      auto m           = store.rebuildManifoldFromIndex(prev);
      const auto start = Clock::now();
      const auto res   = m.advance(store, latestId);
      samples.push_back(nsSince(start));
      EXPECT_TRUE(res.has_value());
    }
    const auto s = stats(samples);
    std::printf("S3 %s advance(latest-1 -> latest) us: median %.3f\n", label,
                s.median / 1e3);
    const auto lt = Clock::now();
    std::ignore   = store.latest();
    std::printf("S3 %s Store::latest() itself us: %.3f\n", label,
                nsSince(lt) / 1e3);
    // VU12's alternative: the accursed cell's own history.
    const auto m     = store.rebuildManifoldFromIndex(latest);
    CellRef sample   = zigzag::noCell;
    std::size_t hist = 0;
    for (CellRef c = latest; c > 0; --c) {
      if (m.contains(c)) {
        sample = c;
        break;
      }
    }
    if (sample != zigzag::noCell) {
      const auto start = Clock::now();
      hist             = m.historyOf(sample).size();
      std::printf("S3 %s historyOf(cell %u) -> %zu entries in %.3f us\n", label,
                  sample, hist, nsSince(start) / 1e3);
    }
  }
}

/// KJV as a slice: one cell per verse line, on d.verse; chapter and book
/// heads on d.chapter and d.book. Spans cut from the imported text.
TEST(SpikeViewEngine, S3_RebuildManifoldKjv) {
  const fs::path kjv = "tests/samples/kjv.txt";
  ASSERT_TRUE(fs::exists(kjv));
  auto scroll   = freshScroll("s3-kjv");
  auto imported = xanadu::importFileStore(kjv, scroll);
  auto &store   = *imported;
  const auto t0 = Clock::now();
  timeRebuilds("kjv-import (text only)", store, 20);

  std::ifstream in(kjv, std::ios::binary);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) {
    lines.push_back(line);
  }
  auto at          = store.latest();
  const auto text  = store.rebuild(at);
  const auto total = static_cast<std::uint32_t>(fs::file_size(kjv));
  auto spans       = text.spansFor(0, total);
  std::printf("S3 kjv: %zu lines, %zu source spans\n", lines.size(),
              spans.size());
  // Cut the source spans into per-line pieces (content is incidental here).
  std::vector<PrimediaSpan> pieces;
  {
    std::size_t s    = 0;
    std::uint32_t in = 0;
    for (const auto &line : lines) {
      auto want = static_cast<std::uint32_t>(line.size() + 1);
      if (s >= spans.size()) {
        break;
      }
      const auto avail = static_cast<std::uint32_t>(spans[s].length - in);
      const auto take  = std::max<std::uint32_t>(
          1, std::min<std::uint32_t>(want, static_cast<std::uint32_t>(avail)));
      pieces.push_back(PrimediaSpan{.scroll = spans[s].scroll,
                                    .start  = spans[s].start + in,
                                    .length = take});
      in += take;
      if (in >= spans[s].length) {
        ++s;
        in = 0;
      }
    }
  }

  at                = store.sliceGenesis(at);
  const auto dVerse = store.makeDimension(at, "d.verse");
  at                = dVerse.version;
  const auto dChap  = store.makeDimension(at, "d.chapter");
  at                = dChap.version;
  const auto dBook  = store.makeDimension(at, "d.book");
  at                = dBook.version;
  CellRef prev      = zigzag::noCell;
  CellRef prevChap  = zigzag::noCell;
  CellRef prevBook  = zigzag::noCell;
  std::string book;
  std::string chapter;
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    at              = store.makeCell(at, pieces[i]);
    const auto cell = store.cellRefOf(at);
    if (prev != zigzag::noCell) {
      at = store.setLink(at, prev, dVerse.dim, DimVector::POS, cell);
    }
    prev = cell;
    // "Genesis 1:1\t..." -> book "Genesis", chapter "Genesis 1".
    const auto &line = lines[i];
    const auto colon = line.find(':');
    const auto space = line.rfind(' ', colon);
    if (colon == std::string::npos || space == std::string::npos) {
      continue;
    }
    const auto b = line.substr(0, space);
    const auto c = line.substr(0, colon);
    if (c != chapter) {
      if (prevChap != zigzag::noCell) {
        at = store.setLink(at, prevChap, dChap.dim, DimVector::POS, cell);
      }
      prevChap = cell;
      chapter  = c;
    }
    if (b != book) {
      if (prevBook != zigzag::noCell) {
        at = store.setLink(at, prevBook, dBook.dim, DimVector::POS, cell);
      }
      prevBook = cell;
      book     = b;
    }
  }
  std::printf("S3 kjv slice built in %.1f s\n", nsSince(t0) / 1e9);
  timeRebuilds("kjv-slice", store, 10);
}

TEST(SpikeViewEngine, S3_RebuildManifoldSynthetic100k) {
  Store store(freshScroll("s3-syn"));
  auto at          = store.insert({}, 0, "abcdefghijklmnopqrstuvwxyz");
  const auto spans = store.rebuild(at).spansFor(0, 26);
  ASSERT_FALSE(spans.empty());
  at = store.sliceGenesis(at);
  std::vector<zigzag::DimRef> dims;
  for (int d = 0; d < 10; ++d) {
    const auto minted = store.makeDimension(at, "d.s" + std::to_string(d));
    at                = minted.version;
    dims.push_back(minted.dim);
  }
  std::vector<CellRef> tail(dims.size(), zigzag::noCell);
  std::mt19937 rng(7);
  const auto t0 = Clock::now();
  while (store.opCount() < 100000) {
    at              = store.makeCell(at, spans.front());
    const auto cell = store.cellRefOf(at);
    const auto d    = rng() % dims.size();
    if (tail[d] != zigzag::noCell) {
      at = store.setLink(at, tail[d], dims[d], DimVector::POS, cell);
    }
    tail[d] = cell;
  }
  std::printf("S3 synthetic built in %.1f s\n", nsSince(t0) / 1e9);
  timeRebuilds("synthetic-100k", store, 10);
}

TEST(SpikeViewEngine, S3_RebuildManifoldMultimedia) {
  xanadu::UserPermascroll::Config config;
  config.storageDir = "tests/samples/xudu/permascroll";
  auto scroll = std::make_shared<xanadu::UserPermascroll>(std::move(config));
  for (const char *name :
       {"multimedia/02_pdf_linked_xanadoc", "multimedia/06_embedded_media_page",
        "../xuzz/slice_then_xanadoc"}) {
    const auto dir = fs::path("tests/samples/xudu") / name;
    auto store     = xanadu::loadStore(dir, scroll);
    timeRebuilds(name, *store, 200);
  }
}

// --------------------------------------------------------------------------
// S5
// --------------------------------------------------------------------------

struct Constituent {
  CellRef real;
  std::uint16_t lane;
};

TEST(SpikeViewEngine, S5_OccurrenceScanVersusMap) {
  constexpr std::size_t kDims = 100;
  std::mt19937 rng(11);
  for (const std::size_t window : {300UZ, 1000UZ, 3000UZ, 10000UZ}) {
    // Packs of up to kDims constituents; a real cell appears in several packs
    // (that is what an occurrence is), drawn from a pool smaller than the
    // window.
    const std::size_t pool = std::max<std::size_t>(window / 4, 16);
    std::vector<Constituent> visible;
    visible.reserve(window);
    for (std::size_t i = 0; i < window; ++i) {
      visible.push_back({static_cast<CellRef>(1000 + (rng() % pool)),
                         static_cast<std::uint16_t>(rng() % kDims)});
    }
    std::vector<CellRef> queries;
    for (int q = 0; q < 4096; ++q) {
      queries.push_back(visible[rng() % window].real);
    }

    // Linear scan.
    std::vector<double> scan;
    std::size_t sink = 0;
    std::vector<std::uint32_t> hits;
    hits.reserve(64);
    for (const auto target : queries) {
      const auto start = Clock::now();
      hits.clear();
      for (std::uint32_t i = 0; i < visible.size(); ++i) {
        if (visible[i].real == target) {
          hits.push_back(i);
        }
      }
      scan.push_back(nsSince(start));
      sink += hits.size();
    }

    // Per-frame map: build once, then query.
    std::vector<double> build;
    std::unordered_map<CellRef, std::vector<std::uint32_t>> map;
    for (int r = 0; r < 200; ++r) {
      const auto start = Clock::now();
      map.clear();
      map.reserve(window);
      for (std::uint32_t i = 0; i < visible.size(); ++i) {
        map[visible[i].real].push_back(i);
      }
      build.push_back(nsSince(start));
    }
    std::vector<double> lookup;
    for (const auto target : queries) {
      const auto start = Clock::now();
      const auto it    = map.find(target);
      sink += it == map.end() ? 0 : it->second.size();
      lookup.push_back(nsSince(start));
    }
    const auto s = stats(scan);
    const auto b = stats(build);
    const auto l = stats(lookup);
    std::printf("S5 window %5zu (pool %4zu): scan/query ns median %.0f p90 "
                "%.0f max %.0f | map build/frame ns median %.0f p90 %.0f | "
                "map lookup ns median %.0f | sink %zu\n",
                window, pool, s.median, s.p90, s.max, b.median, b.p90, l.median,
                sink);
  }
}

} // namespace
