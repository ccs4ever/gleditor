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

TEST(UnifiedTransclusionEngineTest, IncrementalSyncFromStore) {
  xudu::Store store;
  UnifiedTransclusionEngine engine(store);

  EXPECT_EQ(engine.cellCount(), 0U);

  // Insert text into initial root state 0
  const xudu::MicroversionId v0{};
  const auto v1 = store.insert(v0, 0, "Everything is deeply intertwingled.");
  const auto v2 = store.insert(v1, 35, " No boundaries exist in thought.");

  // Sync operations into Zigzag topology
  engine.syncIncremental();

  EXPECT_GE(engine.cellCount(), 2U);

  const auto cell1Id = engine.cellForOp(1);
  const auto cell2Id = engine.cellForOp(2);
  EXPECT_NE(cell1Id, 0U);
  EXPECT_NE(cell2Id, 0U);

  // Check resolved text directly from primedia spool
  const auto text1 = engine.resolveCellText(cell1Id);
  const auto text2 = engine.resolveCellText(cell2Id);
  EXPECT_EQ(text1, "Everything is deeply intertwingled.");
  EXPECT_EQ(text2, " No boundaries exist in thought.");

  // Check 2-rank manifold invariant on synced space
  std::string err;
  EXPECT_TRUE(engine.validate2RankManifold(&err)) << err;

  // Check d.ops_time chronological connection
  const auto *c1 = engine.findCell(cell1Id);
  const auto *c2 = engine.findCell(cell2Id);
  ASSERT_NE(c1, nullptr);
  ASSERT_NE(c2, nullptr);

  EXPECT_EQ(c1->linksOn(DimOrdinal::OpsTime).pos, cell2Id);
  EXPECT_EQ(c2->linksOn(DimOrdinal::OpsTime).neg, cell1Id);
}

TEST(UnifiedTransclusionEngineTest, ManifoldValidationCatchesAsymmetry) {
  xudu::Store store;
  UnifiedTransclusionEngine engine(store);

  CompactZZCell c1;
  c1.id = 1;
  CompactZZCell c2;
  c2.id = 2;

  engine.addCell(c1);
  engine.addCell(c2);

  // Link c1 -> c2 properly
  engine.linkCells(1, 2, DimOrdinal::D1);

  std::string err;
  EXPECT_TRUE(engine.validate2RankManifold(&err)) << err;

  // Intentionally break symmetry
  auto *p1 = engine.findCell(1);
  ASSERT_NE(p1, nullptr);
  p1->setLinks(DimOrdinal::D1, {.pos = 999, .neg = 0}); // Dangling link

  EXPECT_FALSE(engine.validate2RankManifold(&err));
  EXPECT_FALSE(err.empty());
}

TEST(UnifiedTransclusionEngineTest, ToAndFromZzStructureDocument) {
  xudu::Store store;
  UnifiedTransclusionEngine engine(store);

  const xudu::MicroversionId v0{};
  store.insert(v0, 0, "Nelsonian Primordial Stream");
  engine.syncIncremental();

  const auto zzDoc = engine.toZzStructureDocument();
  EXPECT_FALSE(zzDoc.cells.empty());

  // Load into fresh engine
  xudu::Store freshStore;
  UnifiedTransclusionEngine importedEngine(freshStore);
  importedEngine.loadFromZzStructureDocument(zzDoc);

  EXPECT_EQ(importedEngine.cellCount(), engine.cellCount());
  const auto *c = importedEngine.findCell(1);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->ephemeralText, "Nelsonian Primordial Stream");
}

TEST(UnifiedTransclusionEngineTest, StageVisibleCellsForRender) {
  xudu::Store store;
  UnifiedTransclusionEngine engine(store);

  const xudu::MicroversionId v0{};
  const auto v1 = store.insert(v0, 0, "Cell One Content");
  const auto v2 = store.insert(v1, 16, "Cell Two Content");
  engine.syncIncremental();

  // Link on d.1 for spatial traversal
  const auto c1Id = engine.cellForOp(1);
  const auto c2Id = engine.cellForOp(2);
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

  CompactZZCell cellWithheld;
  cellWithheld.id               = 1;
  cellWithheld.resolutionStatus = xudu::ResolutionStatus::WithheldRedacted;
  engine.addCell(cellWithheld);

  CompactZZCell cellLocked;
  cellLocked.id                 = 2;
  cellLocked.resolutionStatus   = xudu::ResolutionStatus::TranscopyrightLocked;
  cellLocked.transcopyrightInfo = xudu::TranscopyrightDescriptor{
      .priceAtomicUnits = 25,
      .currencySymbol   = "XU",
  };
  engine.addCell(cellLocked);

  engine.linkCells(1, 2, DimOrdinal::D1);

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

  /// A chain of cells linked along d.1, each with its own text.
  UnifiedTransclusionEngine::RenderSliceRequest buildChain(const int count) {
    auto version     = xudu::MicroversionId{};
    std::uint32_t at = 0;
    for (int i = 0; i < count; i++) {
      const auto text = "Cell number " + std::to_string(i) +
                        " carrying enough words to be worth shaping.";
      version         = store.insert(version, at, text);
      at += static_cast<std::uint32_t>(text.size());
    }
    engine.syncIncremental();
    for (int i = 1; i < count; i++) {
      engine.linkCells(engine.cellForOp(static_cast<std::uint32_t>(i)),
                       engine.cellForOp(static_cast<std::uint32_t>(i + 1)),
                       DimOrdinal::D1);
    }
    return {.focusCellId = engine.cellForOp(1),
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
  auto version = rig.store.insert(xudu::MicroversionId{}, 0, "Alpha content x");
  rig.engine.syncIncremental();
  const auto req = UnifiedTransclusionEngine::RenderSliceRequest{
      .focusCellId = rig.engine.cellForOp(1),
      .radiusX     = 1,
      .radiusY     = 1,
      .radiusZ     = 1};

  static_cast<void>(
      rig.engine.stageVisibleCells(req, rig.font, *rig.glyphCache));
  const auto afterFirst = rig.engine.shapingCacheStats();

  // A second cell whose text differs only in its last character.
  version = rig.store.insert(version, 15, "Alpha content y");
  rig.engine.syncIncremental();
  const auto req2 = UnifiedTransclusionEngine::RenderSliceRequest{
      .focusCellId = rig.engine.cellForOp(2),
      .radiusX     = 1,
      .radiusY     = 1,
      .radiusZ     = 1};
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

TEST(UnifiedTransclusionEngineTest, SubSpanTransclusionLinksOnDimTransclude) {
  xudu::Store store;
  UnifiedTransclusionEngine engine(store);

  // Op 1: Master document text [0, 51)
  const xudu::MicroversionId v0{};
  const auto v1 =
      store.insert(v0, 0, "The quick brown fox jumps over the lazy dog today.");

  // Op 2: In another branch, transclude a sub-span "fox jumps" [16, 25)
  static_cast<void>(store.transclude(v0, 0, v1, 16, 9));
  engine.syncIncremental();

  const auto masterCellId = engine.cellForOp(1);
  const auto quoteCellId  = engine.cellForOp(2);
  ASSERT_NE(masterCellId, 0U);
  ASSERT_NE(quoteCellId, 0U);

  const auto *cMaster = engine.findCell(masterCellId);
  const auto *cQuote  = engine.findCell(quoteCellId);
  ASSERT_NE(cMaster, nullptr);
  ASSERT_NE(cQuote, nullptr);

  // Sub-span must be connected along DimOrdinal::Transclude
  EXPECT_EQ(cMaster->linksOn(DimOrdinal::Transclude).pos, quoteCellId);
  EXPECT_EQ(cQuote->linksOn(DimOrdinal::Transclude).neg, masterCellId);

  // Manifold must remain strictly valid
  std::string err;
  EXPECT_TRUE(engine.validate2RankManifold(&err)) << err;
}
