/**
 * @file test_unified_transclusion_engine.cpp
 * @brief Comprehensive unit tests for CompactZZCell and
 * UnifiedTransclusionEngine.
 */
#include <gtest/gtest.h>

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
