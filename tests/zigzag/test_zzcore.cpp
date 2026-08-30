/**
 * @file test_zzcore.cpp
 * @brief Unit tests for pure ZigZag structural logic.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "zigzag/core/zzcore.hpp"
#include "zigzag/core/zzstructure.hpp"

using namespace zigzag;
using namespace zigzag::zzcore;

namespace {

zzCell
makeCell(const CellID id, std::string type = {},
         const std::vector<std::pair<DimID, CellID>> &forwardLinks = {}) {
  zzCell cell;
  cell.id        = id;
  cell.type      = std::move(type);
  cell.text_data = "cell " + std::to_string(id);
  for (const auto &[dimension, target] : forwardLinks) {
    cell.dimensions[dimension] = LinkPairs{target, 0};
  }
  return cell;
}

std::vector<ExplicitLink>
collectExplicitLinks(const std::unordered_map<CellID, zzCell> &cells) {
  std::vector<ExplicitLink> links;
  for (const auto &[id, cell] : cells) {
    for (const auto &[dim, pair] : cell.dimensions) {
      if (pair.pos != 0) {
        links.push_back({id, dim, true, pair.pos});
      }
      if (pair.neg != 0) {
        links.push_back({id, dim, false, pair.neg});
      }
    }
  }
  std::ranges::sort(links, [](const ExplicitLink &a, const ExplicitLink &b) {
    if (a.from != b.from) {
      return a.from < b.from;
    }
    if (a.dimension != b.dimension) {
      return a.dimension < b.dimension;
    }
    return a.isPos > b.isPos;
  });
  return links;
}

LinkPairs linksOf(const std::unordered_map<CellID, zzCell> &cells,
                  const CellID id, const DimID &dimension) {
  return linksOn(findCell(cells, id), dimension);
}

} // namespace

TEST(ZzCoreTest, MagnetUriValidation) {
  EXPECT_TRUE(looksLikeBitTorrentMagnet(
      "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567"));
  EXPECT_TRUE(looksLikeBitTorrentMagnet(
      "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=name"));
  EXPECT_TRUE(looksLikeBitTorrentMagnet(
      "magnet:?xt=urn:btih:abcdefghijklmnopqrstuvwxyz234567")); // 32 chars
                                                                // base32

  EXPECT_FALSE(looksLikeBitTorrentMagnet("http://example.com"));
  EXPECT_FALSE(looksLikeBitTorrentMagnet("magnet:?xt=urn:sha1:abc"));
  EXPECT_FALSE(looksLikeBitTorrentMagnet("magnet:?xt=urn:btih:short"));
}

TEST(ZzCoreTest, HexColorParsing) {
  const auto red = parseHexColor("#ff0000");
  ASSERT_TRUE(red.has_value());
  EXPECT_FLOAT_EQ(red->r, 1.0F);
  EXPECT_FLOAT_EQ(red->g, 0.0F);
  EXPECT_FLOAT_EQ(red->b, 0.0F);

  const auto noHash = parseHexColor("00ff00");
  ASSERT_TRUE(noHash.has_value());
  EXPECT_FLOAT_EQ(noHash->g, 1.0F);

  EXPECT_FALSE(parseHexColor("invalid"));
  EXPECT_FALSE(parseHexColor("#fff"));
}

TEST(ZzCoreTest, SplitMetadataEntry) {
  const auto [k1, v1] = splitMetadataEntry("file: slice.yaml");
  EXPECT_EQ(k1, "file");
  EXPECT_EQ(v1, "slice.yaml");

  const auto [k2, v2] = splitMetadataEntry("no_colon");
  EXPECT_EQ(k2, "no_colon");
  EXPECT_EQ(v2, "");
}

TEST(ZzCoreTest, BacklinkDerivation) {
  std::unordered_map<CellID, zzCell> cells;
  cells[1] = makeCell(1, "item", {{"d.1", 2}});
  cells[2] = makeCell(2, "item");

  Diagnostics diag;
  const auto links = collectExplicitLinks(cells);
  deriveBacklinks(cells, links, diag);

  EXPECT_EQ(linksOf(cells, 1, "d.1").pos, 2U);
  EXPECT_EQ(linksOf(cells, 2, "d.1").neg, 1U);
  EXPECT_TRUE(diag.empty());
}

TEST(ZzCoreTest, BacklinkConflictPreservation) {
  std::unordered_map<CellID, zzCell> cells;
  cells[1]                   = makeCell(1, "item", {{"d.1", 2}});
  cells[2]                   = makeCell(2, "item");
  cells[2].dimensions["d.1"] = LinkPairs{0, 3}; // Neg already points at 3

  Diagnostics diag;
  const auto links = collectExplicitLinks(cells);
  deriveBacklinks(cells, links, diag);

  EXPECT_EQ(linksOf(cells, 2, "d.1").neg, 3U);
  EXPECT_FALSE(diag.empty());
  EXPECT_TRUE(diag.mentions("conflict") || diag.mentions("leaving both"));
}

TEST(ZzCoreTest, NeutralizeDanglingLinks) {
  std::unordered_map<CellID, zzCell> cells;
  cells[1] = makeCell(1, "item", {{"d.1", 999}}); // 999 does not exist

  Diagnostics diag;
  neutralizeDanglingLinks(cells, diag);

  EXPECT_EQ(linksOf(cells, 1, "d.1").pos, 0U);
  EXPECT_FALSE(diag.empty());
}

TEST(ZzCoreTest, PrefletChainResolution) {
  std::unordered_map<CellID, zzCell> cells;
  cells[1] = makeCell(1, "chapter", {{"d.preflet", 10}});

  cells[10] = makeCell(10, "preflet_resource", {{"d.preflet", 11}});
  cells[10].text_data =
      "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567";

  cells[11]           = makeCell(11, "preflet_version", {{"d.preflet", 12}});
  cells[11].text_data = "1.0";

  cells[12]           = makeCell(12, "preflet_meta");
  cells[12].text_data = "file: target.yaml";

  Diagnostics diag;
  resolveAllPreflets(cells, diag);

  ASSERT_TRUE(cells[1].preflet.has_value());
  EXPECT_EQ(cells[1].preflet->version, "1.0");
  EXPECT_EQ(cells[1].preflet->resource_identifier,
            "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567");
  ASSERT_EQ(cells[1].preflet->metadata.size(), 1U);
  EXPECT_EQ(cells[1].preflet->metadata[0].first, "file");
  EXPECT_EQ(cells[1].preflet->metadata[0].second, "target.yaml");
}

TEST(ZzCoreTest, PrefletCycleDetection) {
  std::unordered_map<CellID, zzCell> cells;
  cells[1] = makeCell(1, "chapter", {{"d.preflet", 10}});

  cells[10] = makeCell(10, "preflet_resource", {{"d.preflet", 11}});
  cells[10].text_data =
      "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567";

  // Cycle back to 10
  cells[11] = makeCell(11, "preflet_version", {{"d.preflet", 10}});

  Diagnostics diag;
  const auto preflet = resolvePreflet(10, cells, 1, diag);
  EXPECT_TRUE(preflet.has_value());
  EXPECT_TRUE(diag.mentions("loops back"));
}

TEST(ZzCoreTest, AxisNeighbours) {
  std::unordered_map<CellID, zzCell> cells;
  cells[1]                   = makeCell(1, "item");
  cells[1].dimensions["d.1"] = LinkPairs{2, 3};
  cells[1].dimensions["d.2"] = LinkPairs{4, 5};
  cells[1].dimensions["d.3"] = LinkPairs{6, 7};

  const ViewAxisBinding view{"d.1", "d.2", "d.3"};
  const auto neighbours = axisNeighbours(&cells[1], view);

  EXPECT_EQ(neighbours[0], 2U); // x+
  EXPECT_EQ(neighbours[1], 3U); // x-
  EXPECT_EQ(neighbours[2], 4U); // y+
  EXPECT_EQ(neighbours[3], 5U); // y-
  EXPECT_EQ(neighbours[4], 6U); // z+
  EXPECT_EQ(neighbours[5], 7U); // z-
}
