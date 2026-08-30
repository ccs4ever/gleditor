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

  EXPECT_EQ(raster.text, "Chapter One. The Beginning.\n\nChapter Two. The Continuation.");
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
