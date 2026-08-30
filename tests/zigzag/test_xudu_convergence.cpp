/**
 * @file test_xudu_convergence.cpp
 * @brief Unit tests for bidirectional convergence between Xudu and Zigzag.
 */
#include <gtest/gtest.h>

#include "xudu/core/link_package.hpp"
#include "xudu/core/microversion.hpp"
#include "xudu/core/ops.hpp"
#include "xudu/core/scroll.hpp"
#include "xudu/core/store.hpp"
#include "xudu/core/swarm.hpp"
#include "zigzag/core/zz_xudu_projector.hpp"
#include "zigzag/core/zzcore.hpp"
#include "zigzag/core/zzstructure.hpp"

using namespace zigzag;

TEST(ZzXuduConvergenceTest, ProjectXuduDocsToZigzag) {
  // Document A and Document B share a transcluded span and have a link
  const xudu::PrimediaSpan spanA{1, 0, 20};
  const xudu::PrimediaSpan spanB{1, 10, 30}; // Overlaps spanA
  const xudu::PrimediaSpan spanC{2, 0, 15};

  std::vector<XuduDocInput> docs;
  docs.push_back(XuduDocInput{
      .name    = "doc_a",
      .text    = "Introduction paragraph\n\nSecond paragraph of doc A",
      .version = xudu::MicroversionId::parse("1"),
      .spans   = {spanA, spanC},
  });

  docs.push_back(XuduDocInput{
      .name    = "doc_b",
      .text    = "Transcluded intro\n\nIndependent conclusion",
      .version = xudu::MicroversionId::parse("1"),
      .spans   = {spanB, spanC},
  });

  xudu::Link xLink;
  xLink.id    = 100;
  xLink.type  = xudu::LinkType::Comment;
  xLink.left  = {spanA};
  xLink.right = {spanC};

  auto zzDoc = projectXuduToZigzag(docs, {xLink});

  // Verify cells were created
  EXPECT_FALSE(zzDoc.cells.empty());
  EXPECT_EQ(zzDoc.focus, 1U);

  // Check 2-rank manifold invariant
  std::string error;
  EXPECT_TRUE(validate2RankManifold(zzDoc, &error)) << error;

  // Check that d.doc connects sequential paragraphs
  const auto &cell1 = zzDoc.cells[1];
  EXPECT_EQ(cell1.dimensions.at("d.doc").pos, 2U);
  EXPECT_EQ(zzDoc.cells[2].dimensions.at("d.doc").neg, 1U);

  // Check transclusion link between cell 1 (doc_a) and cell 3 (doc_b)
  EXPECT_EQ(cell1.dimensions.at("d.transclude").pos, 3U);
  EXPECT_EQ(zzDoc.cells[3].dimensions.at("d.transclude").neg, 1U);

  // Check xanalink between cell 1 (left) and cell 2 (right)
  EXPECT_EQ(cell1.dimensions.at("d.link").pos, 2U);
}

TEST(ZzXuduConvergenceTest, RasterizeZzStructure) {
  ZzStructureDocument doc;
  doc.focus = 1;

  // Row 1: Cell 1 -> Cell 2
  // Row 2: Cell 3 -> Cell 4 (Cell 1 is -d.2 of Cell 3)
  zzCell c1{.id = 1, .text_data = "Chapter One.", .type = "cell"};
  c1.dimensions["d.1"].pos = 2;
  c1.dimensions["d.2"].pos = 3;

  zzCell c2{.id = 2, .text_data = "The Beginning.", .type = "cell"};
  c2.dimensions["d.1"].neg = 1;

  zzCell c3{.id = 3, .text_data = "Chapter Two.", .type = "cell"};
  c3.dimensions["d.2"].neg = 1;
  c3.dimensions["d.1"].pos = 4;

  zzCell c4{.id = 4, .text_data = "The Continuation.", .type = "cell"};
  c4.dimensions["d.1"].neg = 3;

  doc.cells[1] = c1;
  doc.cells[2] = c2;
  doc.cells[3] = c3;
  doc.cells[4] = c4;

  const auto raster = rasterizeZzStructure(doc, "d.1", "d.2", 1);
  EXPECT_EQ(raster.cell_sequence.size(), 4U);
  EXPECT_EQ(raster.cell_sequence[0], 1U);
  EXPECT_EQ(raster.cell_sequence[1], 2U);
  EXPECT_EQ(raster.cell_sequence[2], 3U);
  EXPECT_EQ(raster.cell_sequence[3], 4U);

  EXPECT_EQ(raster.text,
            "Chapter One. The Beginning.\n\nChapter Two. The Continuation.");
}

TEST(ZzXuduConvergenceTest, LinkPackageRoundTrip) {
  ZzStructureDocument doc;
  doc.meta.name = "Test Convergence Slice";
  doc.focus     = 1;

  zzCell c1{.id = 1, .text_data = "Cell Alpha", .type = "cell"};
  c1.dimensions["d.1"].pos = 2;

  zzCell c2{.id = 2, .text_data = "Cell Beta", .type = "cell"};
  c2.dimensions["d.1"].neg = 1;

  doc.cells[1] = c1;
  doc.cells[2] = c2;

  const auto keys = xudu::createMutableKeys();
  const auto pkg  = zzStructureToLinkPackage(doc, keys, "slice_salt", 1);

  EXPECT_EQ(pkg.title, "Test Convergence Slice");
  EXPECT_EQ(pkg.links.size(), 1U);
  EXPECT_EQ(pkg.links.front().type, xudu::LinkType::Dimension);
  EXPECT_EQ(pkg.links.front().owner, "dim:d.1");

  // Reconstruct back to Zigzag space
  const auto decodedDoc = linkPackageToZzStructure(pkg);
  EXPECT_EQ(decodedDoc.meta.name, "Test Convergence Slice");
  EXPECT_EQ(decodedDoc.cells.size(), 2U);

  std::string error;
  EXPECT_TRUE(validate2RankManifold(decodedDoc, &error)) << error;
}

TEST(ZzXuduConvergenceTest, ManifoldValidation) {
  ZzStructureDocument doc;
  zzCell c1{.id = 1, .text_data = "Cell 1", .type = "cell"};
  c1.dimensions["d.1"].pos = 2;

  zzCell c2{.id = 2, .text_data = "Cell 2", .type = "cell"};
  // Missing backlink from 2 to 1 (asymmetric)
  c2.dimensions["d.1"].neg = 0;

  doc.cells[1] = c1;
  doc.cells[2] = c2;

  std::string error;
  EXPECT_FALSE(validate2RankManifold(doc, &error));
  EXPECT_FALSE(error.empty());

  // Fix the backlink
  doc.cells[2].dimensions["d.1"].neg = 1;
  EXPECT_TRUE(validate2RankManifold(doc, &error));
}

TEST(ZzXuduConvergenceTest, XuduHypertimeUnchangedSpansBecomeCloneCells) {
  // Create a Xudu store with two microversions:
  // Version 1: Paragraph 1 (spool 1, offset 0, len 20), Paragraph 2 (spool 1,
  // offset 20, len 20) Version 2: Paragraph 1 (spool 1, offset 0, len 20 -
  // UNCHANGED), Paragraph 3 (spool 2, offset 0, len 25 - EDITED)
  const xudu::PrimediaSpan span1{1, 0, 20};
  const xudu::PrimediaSpan span2{1, 20, 20};
  const xudu::PrimediaSpan span3{2, 0, 25};

  std::vector<XuduDocInput> docs;
  docs.push_back(XuduDocInput{
      .name    = "v1",
      .text    = "First unchanged text\n\nOriginal second text",
      .version = xudu::MicroversionId::parse("1"),
      .spans   = {span1, span2},
  });

  docs.push_back(XuduDocInput{
      .name    = "v2",
      .text    = "First unchanged text\n\nEdited second text here",
      .version = xudu::MicroversionId::parse("2"),
      .spans   = {span1, span3},
  });

  auto zzDoc = projectXuduToZigzag(docs, {});

  std::string error;
  EXPECT_TRUE(validate2RankManifold(zzDoc, &error)) << error;

  // We should have 4 cells total:
  // Cell 1: v1 para 1 (Master)
  // Cell 2: v1 para 2
  // Cell 3: v2 para 1 (Clone of Cell 1)
  // Cell 4: v2 para 2 (New span3)
  ASSERT_EQ(zzDoc.cells.size(), 4U);

  const auto &c1 = zzDoc.cells[1];
  const auto &c2 = zzDoc.cells[2];
  const auto &c3 = zzDoc.cells[3];
  const auto &c4 = zzDoc.cells[4];

  EXPECT_EQ(c1.type, "xudu_span");
  EXPECT_EQ(c1.text_data, "First unchanged text");

  // Cell 3 is an unchanged span across microversions, so it is a clone of Cell
  // 1
  EXPECT_EQ(c3.type, "xudu_clone");
  EXPECT_TRUE(c3.text_data.empty()); // Clones do not duplicate text

  // d.clone rank links
  EXPECT_EQ(c1.dimensions.at("d.clone").pos, 3U);
  EXPECT_EQ(c3.dimensions.at("d.clone").neg, 1U);

  EXPECT_FALSE(zzcore::isCloneCell(zzDoc.cells, 1));
  EXPECT_TRUE(zzcore::isCloneCell(zzDoc.cells, 3));
  EXPECT_EQ(zzcore::findCloneMaster(zzDoc.cells, 3), 1U);

  // Content lookup
  EXPECT_EQ(zzcore::getEffectiveCellText(zzDoc.cells, 1),
            "First unchanged text");
  EXPECT_EQ(zzcore::getEffectiveCellText(zzDoc.cells, 3),
            "First unchanged text");

  // d.version links connect microversions
  EXPECT_EQ(c1.dimensions.at("d.version").pos, 3U);
  EXPECT_EQ(c3.dimensions.at("d.version").neg, 1U);
  EXPECT_EQ(c2.dimensions.at("d.version").pos, 4U);
  EXPECT_EQ(c4.dimensions.at("d.version").neg, 2U);
}

TEST(ZzXuduConvergenceTest, RasterizeZzStructureWithCloneCells) {
  ZzStructureDocument doc;
  doc.focus = 1;

  // Master cell
  zzCell c1;
  c1.id                        = 1;
  c1.text_data                 = "Shared Header.";
  c1.type                      = "master";
  c1.dimensions["d.doc"].pos   = 2;
  c1.dimensions["d.clone"].pos = 3;

  zzCell c2;
  c2.id                      = 2;
  c2.text_data               = "Doc 1 Body.";
  c2.type                    = "cell";
  c2.dimensions["d.doc"].neg = 1;

  // Clone cell (empty text_data, pos of c1 on d.clone)
  zzCell c3;
  c3.id                        = 3;
  c3.text_data                 = "";
  c3.type                      = "xudu_clone";
  c3.dimensions["d.clone"].neg = 1;
  c3.dimensions["d.doc"].pos   = 4;

  zzCell c4;
  c4.id                      = 4;
  c4.text_data               = "Doc 2 Body.";
  c4.type                    = "cell";
  c4.dimensions["d.doc"].neg = 3;

  doc.cells[1] = c1;
  doc.cells[2] = c2;
  doc.cells[3] = c3;
  doc.cells[4] = c4;

  // Rasterize starting from clone cell 3
  const auto raster = rasterizeZzStructure(doc, "d.doc", "d.clone", 3);
  EXPECT_EQ(raster.text,
            "Shared Header. Doc 1 Body.\n\nShared Header. Doc 2 Body.");
}
