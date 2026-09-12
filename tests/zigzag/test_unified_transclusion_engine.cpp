/**
 * @file test_unified_transclusion_engine.cpp
 * @brief Comprehensive unit tests for CompactZZCell and
 * UnifiedTransclusionEngine.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "../lib/mocks/device.hpp"
#include "gleditor/glyphcache/cache.hpp"
#include "gleditor/text/font.hpp"
#include "xudu/core/microversion.hpp"
#include "xudu/core/ops.hpp"
#include "xudu/core/store.hpp"
#include "zigzag/core/compact_zzcell.hpp"
#include "zigzag/core/unified_transclusion_engine.hpp"

using namespace zigzag;

TEST(CompactZZCellTest, DimOrdinalLookup) {
  // Test numeric coordinate dimensions
  EXPECT_EQ(dimOrdinalFromString("d.1"), DimOrdinal::D1);
  EXPECT_EQ(dimOrdinalFromString("d.2"), DimOrdinal::D2);
  EXPECT_EQ(dimOrdinalFromString("d.3"), DimOrdinal::D3);
  EXPECT_EQ(dimOrdinalFromString("d.4"), DimOrdinal::D4);
  EXPECT_EQ(dimOrdinalFromString("d.5"), DimOrdinal::D5);

  // Test semantic Xanadu dimensions
  EXPECT_EQ(dimOrdinalFromString("d.doc"), DimOrdinal::Doc);
  EXPECT_EQ(dimOrdinalFromString("d.transclude"), DimOrdinal::Transclude);
  EXPECT_EQ(dimOrdinalFromString("d.ops_time"), DimOrdinal::OpsTime);
  EXPECT_EQ(dimOrdinalFromString("d.ops_dag"), DimOrdinal::OpsDag);
  EXPECT_EQ(dimOrdinalFromString("d.version"), DimOrdinal::Version);
  EXPECT_EQ(dimOrdinalFromString("d.link"), DimOrdinal::Link);
  EXPECT_EQ(dimOrdinalFromString("d.clone"), DimOrdinal::Clone);

  // Unknown dimensions
  EXPECT_FALSE(dimOrdinalFromString("d.custom_user_dim").has_value());

  // String roundtrips
  EXPECT_EQ(dimOrdinalToString(DimOrdinal::D1), "d.1");
  EXPECT_EQ(dimOrdinalToString(DimOrdinal::D2), "d.2");
  EXPECT_EQ(dimOrdinalToString(DimOrdinal::D3), "d.3");
  EXPECT_EQ(dimOrdinalToString(DimOrdinal::D4), "d.4");
  EXPECT_EQ(dimOrdinalToString(DimOrdinal::D5), "d.5");
  EXPECT_EQ(dimOrdinalToString(DimOrdinal::Doc), "d.doc");
  EXPECT_EQ(dimOrdinalToString(DimOrdinal::Transclude), "d.transclude");
  EXPECT_EQ(dimOrdinalToString(DimOrdinal::OpsTime), "d.ops_time");
  EXPECT_EQ(dimOrdinalToString(DimOrdinal::OpsDag), "d.ops_dag");
  EXPECT_EQ(dimOrdinalToString(DimOrdinal::Version), "d.version");
  EXPECT_EQ(dimOrdinalToString(DimOrdinal::Link), "d.link");
  EXPECT_EQ(dimOrdinalToString(DimOrdinal::Clone), "d.clone");
}

TEST(CompactZZCellTest, LinkPairsAndDynamicDimensions) {
  CompactZZCell cell;
  cell.id = 42;

  // Set standard dimensions via enum and string
  cell.setLinks(DimOrdinal::D1, {.pos = 43, .neg = 41});
  EXPECT_EQ(cell.linksOn(DimOrdinal::D1).pos, 43U);
  EXPECT_EQ(cell.linksOn(DimOrdinal::D1).neg, 41U);
  EXPECT_EQ(cell.linksOn("d.1").pos, 43U);

  cell.setLinks("d.transclude", {.pos = 100, .neg = 99});
  EXPECT_EQ(cell.linksOn(DimOrdinal::Transclude).pos, 100U);
  EXPECT_EQ(cell.linksOn("d.transclude").neg, 99U);

  // Set custom dynamic dimension
  cell.setLinks("d.author_provenance", {.pos = 500, .neg = 0});
  EXPECT_EQ(cell.linksOn("d.author_provenance").pos, 500U);
  EXPECT_EQ(cell.linksOn("d.author_provenance").neg, 0U);
  EXPECT_EQ(cell.dynamicDimensions.size(), 1U);
}

TEST(UnifiedTransclusionEngineTest, TextOperationsMintNoCells) {
  xudu::Store store;
  UnifiedTransclusionEngine engine(store);
  EXPECT_EQ(engine.cellCount(), 0U);

  // The change this test exists to record. buildCellFromOp() used to invent a
  // cell for every operation, an Insert included, and this suite asserted it.
  // A CellRef is the index of the operation that *minted* a cell, so typing
  // into a document mints nothing: a xanadoc's pieces become cells when
  // something says they are cells, which is sliceToStore() or the verbs below.
  const auto v1 = store.insert(xudu::MicroversionId{}, 0, "Everything is");
  static_cast<void>(store.insert(v1, 13, " deeply intertwingled."));
  engine.syncIncremental();
  EXPECT_EQ(engine.cellCount(), 0U);
}

TEST(UnifiedTransclusionEngineTest, IncrementalSyncFoldsMintedCells) {
  xudu::Store store;
  UnifiedTransclusionEngine engine(store);

  const auto first  = engine.addCell("Everything is deeply intertwingled.");
  const auto second = engine.addCell(" No boundaries exist in thought.");
  EXPECT_NE(first, zigzag::noCell);
  EXPECT_NE(second, zigzag::noCell);

  // Genesis mints home and d.dims, so the count is the two cells plus those.
  EXPECT_EQ(engine.cellCount(), 4U);
  EXPECT_EQ(engine.resolveCellText(first),
            "Everything is deeply intertwingled.");
  EXPECT_EQ(engine.resolveCellText(second), " No boundaries exist in thought.");

  // A CellRef *is* an operation index, which is why cellForOp() is gone rather
  // than ported: the mapping it held is the identity.
  EXPECT_EQ(store.cellRefOf(store.segmentedOps().idOf(first)), first);

  engine.linkCells(first, second, DimOrdinal::OpsTime);
  const auto opsTime = engine.dimensionFor("d.ops_time");
  EXPECT_EQ(engine.manifold().linked(first, opsTime, DimVector::POS), second);
  EXPECT_EQ(engine.manifold().linked(second, opsTime, DimVector::NEG), first);

  std::string err;
  EXPECT_TRUE(engine.validate2RankManifold(&err)) << err;
  EXPECT_TRUE(engine.manifold().verifyAgainstFullRebuild(store));
}

TEST(UnifiedTransclusionEngineTest, AsymmetryCanNoLongerBeConstructed) {
  // This used to link two cells, reach in through findCell() and write a
  // dangling pos link, and check that validation caught it. The write is now
  // impossible: a CellSlot exposes no setter, and the only way to make a link
  // is an operation whose fold maintains both ends. So what is left to assert
  // is that the invariant holds through the operations that used to break it.
  xudu::Store store;
  UnifiedTransclusionEngine engine(store);

  const auto one   = engine.addCell("one");
  const auto two   = engine.addCell("two");
  const auto three = engine.addCell("three");
  engine.linkCells(one, two, DimOrdinal::D1);
  engine.linkCells(one, three, DimOrdinal::D1); // displaces two

  std::string err;
  EXPECT_TRUE(engine.validate2RankManifold(&err)) << err;

  const auto d1 = engine.dimensionFor("d.1");
  EXPECT_EQ(engine.manifold().linked(one, d1, DimVector::POS), three);
  EXPECT_EQ(engine.manifold().linked(two, d1, DimVector::NEG), zigzag::noCell)
      << "the displaced cell kept a backlink to a cell that no longer names it";

  engine.unlinkPositive(one, DimOrdinal::D1);
  EXPECT_EQ(engine.manifold().linked(one, d1, DimVector::POS), zigzag::noCell);
  EXPECT_EQ(engine.manifold().linked(three, d1, DimVector::NEG),
            zigzag::noCell);
  EXPECT_TRUE(engine.validate2RankManifold(&err)) << err;
}

TEST(UnifiedTransclusionEngineTest, StageVisibleCellsForRender) {
  xudu::Store store;
  UnifiedTransclusionEngine engine(store);

  const auto c1Id = engine.addCell("Cell One Content");
  const auto c2Id = engine.addCell("Cell Two Content");
  engine.linkCells(c1Id, c2Id, DimOrdinal::D1);

  // Font and glyph cache
  auto &fm  = gleditor::text::FontManager::instance();
  auto font = fm.getFont("Monospace 12");
  ASSERT_NE(font, nullptr);

  testing::NiceMock<MockRenderDevice> device;
  ON_CALL(device, textureLimits())
      .WillByDefault(testing::Return(render::TextureLimits{4096, 64}));
  ON_CALL(device,
          createTextureArray(testing::_, testing::_, testing::_, testing::_))
      .WillByDefault(testing::Return(render::TextureHandle{1}));

  gleditor::GlyphCache glyphCache(&device);

  UnifiedTransclusionEngine::RenderSliceRequest req{
      .focusCellId = c1Id,
      .axisX       = "d.1",
      .axisY       = "d.2",
      .axisZ       = "d.3",
      .radiusX     = 2,
      .radiusY     = 1,
      .radiusZ     = 1,
  };

  const auto batch = engine.stageVisibleCells(req, font, glyphCache);
  EXPECT_GT(batch.instanceCount, 0U);
  EXPECT_EQ(batch.rows.size(), batch.instanceCount);
}

TEST(CompactZZCellTest, WithheldAndTranscopyrightHoles) {
  CompactZZCell withheldCell;
  withheldCell.id               = 10;
  withheldCell.resolutionStatus = xudu::ResolutionStatus::WithheldRedacted;
  EXPECT_TRUE(withheldCell.isWithheld());
  EXPECT_FALSE(withheldCell.isTranscopyrightLocked());

  xudu::PrimediaSpool primedia;
  xudu::Resolver resolver;
  std::vector<xudu::Scroll> externals;
  EXPECT_EQ(withheldCell.readText(primedia, resolver, externals),
            "[Redacted - Withheld]");

  CompactZZCell tcCell;
  tcCell.id                 = 11;
  tcCell.resolutionStatus   = xudu::ResolutionStatus::TranscopyrightLocked;
  tcCell.transcopyrightInfo = xudu::TranscopyrightDescriptor{
      .priceAtomicUnits = 50,
      .currencySymbol   = "XU",
  };
  EXPECT_FALSE(tcCell.isWithheld());
  EXPECT_TRUE(tcCell.isTranscopyrightLocked());
  EXPECT_EQ(tcCell.readText(primedia, resolver, externals), "[🔒 50 XU]");
}

TEST(UnifiedTransclusionEngineTest, StageWithheldAndTranscopyrightCells) {
  xudu::Store store;
  UnifiedTransclusionEngine engine(store);

  // Why a cell is not showing its content is render-side, so it goes in the
  // cold table rather than into the cell: the span is the same address whether
  // or not this reader holds the key.
  const auto withheld = engine.addCell("secret");
  engine.setCold(
      withheld, {.resolutionStatus = xudu::ResolutionStatus::WithheldRedacted});
  const auto locked = engine.addCell("for sale");
  engine.setCold(
      locked, {.resolutionStatus = xudu::ResolutionStatus::TranscopyrightLocked,
               .transcopyrightInfo = xudu::TranscopyrightDescriptor{
                   .priceAtomicUnits = 25,
                   .currencySymbol   = "XU",
               }});

  engine.linkCells(withheld, locked, DimOrdinal::D1);

  auto &fm  = gleditor::text::FontManager::instance();
  auto font = fm.getFont("Monospace 12");
  ASSERT_NE(font, nullptr);

  testing::NiceMock<MockRenderDevice> device;
  ON_CALL(device, textureLimits())
      .WillByDefault(testing::Return(render::TextureLimits{4096, 64}));
  ON_CALL(device,
          createTextureArray(testing::_, testing::_, testing::_, testing::_))
      .WillByDefault(testing::Return(render::TextureHandle{1}));

  gleditor::GlyphCache glyphCache(&device);

  UnifiedTransclusionEngine::RenderSliceRequest req{
      .focusCellId = 1,
      .axisX       = "d.1",
      .axisY       = "d.2",
      .axisZ       = "d.3",
      .radiusX     = 2,
      .radiusY     = 1,
      .radiusZ     = 1,
  };

  const auto batch = engine.stageVisibleCells(req, font, glyphCache);
  EXPECT_GT(batch.instanceCount, 0U);
}

// ============================================================================
namespace {

/// A store of @p ops operations, half plain inserts and half transclusions of
/// overlapping windows into one seed. The transclusions are what put the three
/// costs this test is about on the per-operation path: resolving a source
/// version, finding the master cell for a span, and joining a d.transclude
/// rank.
xudu::MicroversionId buildMixedStore(xudu::Store &store, const int ops) {
  const auto seed =
      store.insert(xudu::MicroversionId{}, 0,
                   "Everything is deeply intertwingled. No boundaries.");
  auto version = seed;
  for (int i = 1; i < ops; i++) {
    if (i % 2 == 0) {
      version = store.insert(version, 0, "x");
    } else {
      version = store.transclude(version, 0, seed,
                                 static_cast<std::uint32_t>(i % 20), 6);
    }
  }
  return version;
}

/// Microseconds per operation to sync a store of @p ops operations.
double syncCostPerOp(const int ops) {
  xudu::Store store;
  buildMixedStore(store, ops);
  UnifiedTransclusionEngine engine(store);
  const auto before = std::chrono::steady_clock::now();
  engine.syncIncremental();
  const auto after = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::micro>(after - before).count() /
         ops;
}

} // namespace

TEST(UnifiedTransclusionEngineTest, SyncCostPerOperationDoesNotGrowWithSize) {
  // buildCellFromOp() used to carry three costs proportional to how much had
  // already been synced: a full store rebuild per transclusion, a scan of
  // every recorded master span, and a walk of a whole d.transclude rank. Each
  // is invisible in a test with five operations and quadratic in a real
  // document, which is why this measures the shape of the curve rather than
  // any single time.
  const auto small = syncCostPerOp(2000);
  const auto large = syncCostPerOp(8000);

  // Four times the work per operation should cost the same per operation.
  // Measured at 1.07 us/op against 0.84 before and after quadrupling; the
  // quadratic version went from 3.57 to 38.73, so 3x leaves room for a slow
  // or contended machine without leaving room for the bug to come back.
  EXPECT_LT(large, small * 3.0)
      << "per-operation sync cost grew from " << small << " us at 2000 ops to "
      << large
      << " us at 8000 -- something in buildCellFromOp() is scanning "
         "what has already been synced";
}

// ============================================================================
// Shaping cache
// ============================================================================

namespace {

/// The staging fixture the tests above build by hand, in one place.
struct StagingRig {
  xudu::Store store;
  UnifiedTransclusionEngine engine{store};
  gleditor::text::FontFacePtr font;
  testing::NiceMock<MockRenderDevice> device;
  std::unique_ptr<gleditor::GlyphCache> glyphCache;

  StagingRig() {
    font = gleditor::text::FontManager::instance().getFont("Monospace 12");
    ON_CALL(device, textureLimits())
        .WillByDefault(testing::Return(render::TextureLimits{4096, 64}));
    ON_CALL(device,
            createTextureArray(testing::_, testing::_, testing::_, testing::_))
        .WillByDefault(testing::Return(render::TextureHandle{1}));
    glyphCache = std::make_unique<gleditor::GlyphCache>(&device);
  }

  std::vector<CellRef> chain;

  /// A chain of cells linked along d.1, each with its own text. Minted rather
  /// than inserted: typing into a document makes no cells.
  UnifiedTransclusionEngine::RenderSliceRequest buildChain(const int count) {
    for (int i = 0; i < count; i++) {
      chain.push_back(engine.addCell("Cell number " + std::to_string(i) +
                                     " carrying enough words to be worth "
                                     "shaping."));
    }
    for (std::size_t i = 1; i < chain.size(); i++) {
      engine.linkCells(chain[i - 1], chain[i], DimOrdinal::D1);
    }
    return {.focusCellId = chain.empty() ? zigzag::noCell : chain.front(),
            .axisX       = "d.1",
            .axisY       = "d.2",
            .axisZ       = "d.3",
            .radiusX     = count,
            .radiusY     = 1,
            .radiusZ     = 1};
  }
};

} // namespace

// The property that matters: a cache that returns anything other than what
// shaping would have produced is worse than no cache. Same request twice, and
// the second is served entirely from cache.
TEST(ShapingCacheTest, CachedStagingMatchesUncachedExactly) {
  StagingRig rig;
  ASSERT_NE(rig.font, nullptr);
  const auto req = rig.buildChain(6);

  const auto first =
      rig.engine.stageVisibleCells(req, rig.font, *rig.glyphCache);
  const auto afterFirst = rig.engine.shapingCacheStats();
  ASSERT_GT(afterFirst.misses, 0U) << "nothing was shaped at all";

  const auto second =
      rig.engine.stageVisibleCells(req, rig.font, *rig.glyphCache);
  const auto afterSecond = rig.engine.shapingCacheStats();

  EXPECT_EQ(afterSecond.misses, afterFirst.misses)
      << "the second pass re-shaped text it had already shaped";
  EXPECT_GT(afterSecond.hits, afterFirst.hits);

  ASSERT_EQ(first.rows.size(), second.rows.size());
  for (std::size_t i = 0; i < first.rows.size(); i++) {
    EXPECT_EQ(std::memcmp(&first.rows[i], &second.rows[i], sizeof(Doc::VBORow)),
              0)
        << "row " << i << " differs between a shaped and a cached pass";
  }
}

// Keyed on the text, not the cell id, because a cell's text changes and its
// id does not. Keying on the id would serve the old words forever.
TEST(ShapingCacheTest, ChangedTextIsNotServedFromCache) {
  StagingRig rig;
  ASSERT_NE(rig.font, nullptr);

  const gleditor::text::LayoutOptions opts{.maxWidthPx      = 380.0F,
                                           .maxHeightPx     = 240.0F,
                                           .singleParagraph = false,
                                           .ellipsize       = true,
                                           .decoratedRanges = {}};

  // Reaching shapedPage through stageVisibleCells needs cells; going at the
  // observable behaviour instead: two texts differing by one character must
  // not share an entry.
  const auto first = rig.engine.addCell("Alpha content x");
  const auto req   = UnifiedTransclusionEngine::RenderSliceRequest{
      .focusCellId = first, .radiusX = 1, .radiusY = 1, .radiusZ = 1};

  static_cast<void>(
      rig.engine.stageVisibleCells(req, rig.font, *rig.glyphCache));
  const auto afterFirst = rig.engine.shapingCacheStats();

  // A second cell whose text differs only in its last character.
  const auto second = rig.engine.addCell("Alpha content y");
  const auto req2   = UnifiedTransclusionEngine::RenderSliceRequest{
      .focusCellId = second, .radiusX = 1, .radiusY = 1, .radiusZ = 1};
  static_cast<void>(
      rig.engine.stageVisibleCells(req2, rig.font, *rig.glyphCache));
  const auto afterSecond = rig.engine.shapingCacheStats();

  EXPECT_GT(afterSecond.misses, afterFirst.misses)
      << "different text was answered from another text's cache entry";
}

// Unbounded is not a cache, it is a leak. A long pan visits far more cells
// than the capacity and must not grow past it.
TEST(ShapingCacheTest, StaysWithinItsCapacity) {
  StagingRig rig;
  ASSERT_NE(rig.font, nullptr);
  const auto cells =
      static_cast<int>(UnifiedTransclusionEngine::kShapingCacheCapacity) + 40;
  const auto req = rig.buildChain(cells);

  static_cast<void>(
      rig.engine.stageVisibleCells(req, rig.font, *rig.glyphCache));
  const auto stats = rig.engine.shapingCacheStats();

  EXPECT_LE(stats.entries, UnifiedTransclusionEngine::kShapingCacheCapacity);
  EXPECT_GT(stats.evictions, 0U) << "capacity was never actually reached";
}

TEST(ShapingCacheTest, ClearingDropsEverything) {
  StagingRig rig;
  ASSERT_NE(rig.font, nullptr);
  const auto req = rig.buildChain(4);
  static_cast<void>(
      rig.engine.stageVisibleCells(req, rig.font, *rig.glyphCache));
  ASSERT_GT(rig.engine.shapingCacheStats().entries, 0U);

  rig.engine.clearShapingCache();
  EXPECT_EQ(rig.engine.shapingCacheStats().entries, 0U);
}

// Not a pass/fail assertion on wall-clock -- a machine under load would make
// that flaky, and a flaky timing test gets disabled and then deleted. This
// reports what a staging pass costs with and without the cache, so the claim
// in compact_zzcell.hpp's comment can be checked rather than believed.
TEST(ShapingCacheTest, ReportsTheCostOfAStagingPass) {
  StagingRig rig;
  ASSERT_NE(rig.font, nullptr);
  const auto req = rig.buildChain(60);

  const auto timeOne = [&](const bool cached) {
    if (!cached) {
      rig.engine.clearShapingCache();
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto batch =
        rig.engine.stageVisibleCells(req, rig.font, *rig.glyphCache);
    const auto t1 = std::chrono::steady_clock::now();
    EXPECT_GT(batch.instanceCount, 0U);
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
  };

  static_cast<void>(timeOne(false)); // warm the glyph atlas

  double cold        = 0.0;
  double warm        = 0.0;
  constexpr int reps = 10;
  for (int i = 0; i < reps; i++) {
    cold += timeOne(false);
  }
  for (int i = 0; i < reps; i++) {
    warm += timeOne(true);
  }
  cold /= reps;
  warm /= reps;

  const auto stats = rig.engine.shapingCacheStats();
  std::printf("  staging 60 cells: shaping every pass %.3f ms | cached %.3f ms "
              "| %.1fx | budget at 120 FPS 8.33 ms\n",
              cold, warm, cold / std::max(warm, 1e-9));
  std::printf("  cache: %zu entries, %llu hits, %llu misses, %llu evictions\n",
              stats.entries, static_cast<unsigned long long>(stats.hits),
              static_cast<unsigned long long>(stats.misses),
              static_cast<unsigned long long>(stats.evictions));

  // The one thing worth asserting: caching did not make it slower.
  EXPECT_LT(warm, cold * 1.5);
}

// SubSpanTransclusionLinksOnDimTransclude was here. It asserted that syncing a
// Transclude operation wove the quoting cell onto the master's d.transclude
// rank -- which buildCellFromOp() did by inventing cells for text operations
// and writing their links directly. Both halves of that are gone. A rank is a
// stored structure now, so d.transclude is minted where a transclusion is
// recorded (§7's "at write time"), not derived where it is displayed, and the
// test belongs with whatever mints it.
