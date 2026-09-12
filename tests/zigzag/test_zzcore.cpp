/**
 * @file test_zzcore.cpp
 * @brief Unit tests for pure ZigZag structural logic.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "zigzag/core/zzcore.hpp"
#include "zigzag/core/zzstructure.hpp"

using namespace zigzag;
using namespace zigzag::zzcore;

namespace {

Cell makeCell(const CellID id, std::string type = {},
              const std::vector<std::pair<DimID, CellID>> &forwardLinks = {}) {
  Cell cell;
  cell.id   = id;
  cell.role = std::move(type);
  cell.data = "cell " + std::to_string(id);
  for (const auto &[dimension, target] : forwardLinks) {
    cell.dimensions[dimension] = LinkPairs{target, 0};
  }
  return cell;
}

std::vector<ExplicitLink>
collectExplicitLinks(const std::unordered_map<CellID, Cell> &cells) {
  std::vector<ExplicitLink> links;
  for (const auto &[id, cell] : cells) {
    for (const auto &[dim, pair] : cell.dimensions) {
      if (pair.pos != 0) {
        links.push_back({id, dim, DimVector::POS, pair.pos});
      }
      if (pair.neg != 0) {
        links.push_back({id, dim, DimVector::NEG, pair.neg});
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
    return a.dir > b.dir;
  });
  return links;
}

LinkPairs linksOf(const std::unordered_map<CellID, Cell> &cells,
                  const CellID id, const DimID &dimension) {
  return linksOn(findCell(cells, id), dimension);
}

} // namespace

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

TEST(ZzCoreTest, BacklinkDerivation) {
  std::unordered_map<CellID, Cell> cells;
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
  std::unordered_map<CellID, Cell> cells;
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
  std::unordered_map<CellID, Cell> cells;
  cells[1] = makeCell(1, "item", {{"d.1", 999}}); // 999 does not exist

  Diagnostics diag;
  neutralizeDanglingLinks(cells, diag);

  EXPECT_EQ(linksOf(cells, 1, "d.1").pos, 0U);
  EXPECT_FALSE(diag.empty());
}

TEST(ZzCoreTest, AxisNeighbours) {
  std::unordered_map<CellID, Cell> cells;
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

TEST(ZzCoreTest, CloneMasterResolutionAlongDClone) {
  std::unordered_map<CellID, Cell> cells;

  // Rank: Cell 10 (Master) -> Cell 20 (Clone 1) -> Cell 30 (Clone 2)
  cells[10]                       = makeCell(10, "master");
  cells[10].data                  = "Universal Truth";
  cells[10].dimensions["d.clone"] = LinkPairs{.pos = 20, .neg = 0};

  cells[20]                       = makeCell(20, "xudu_clone");
  cells[20].data                  = ""; // Clone stores no text
  cells[20].dimensions["d.clone"] = LinkPairs{.pos = 30, .neg = 10};

  cells[30]                       = makeCell(30, "xudu_clone");
  cells[30].data                  = ""; // Clone stores no text
  cells[30].dimensions["d.clone"] = LinkPairs{.pos = 0, .neg = 20};

  // Standalone unlinked cell 40
  cells[40]      = makeCell(40, "standalone");
  cells[40].data = "Independent Content";

  EXPECT_EQ(findCloneMaster(cells, 10), 10U);
  EXPECT_EQ(findCloneMaster(cells, 20), 10U);
  EXPECT_EQ(findCloneMaster(cells, 30), 10U);
  EXPECT_EQ(findCloneMaster(cells, 40), 40U);

  EXPECT_FALSE(isCloneCell(cells, 10));
  EXPECT_TRUE(isCloneCell(cells, 20));
  EXPECT_TRUE(isCloneCell(cells, 30));
  EXPECT_FALSE(isCloneCell(cells, 40));

  // Effective text derivation from master head
  EXPECT_EQ(getEffectiveCellText(cells, 10), "Universal Truth");
  EXPECT_EQ(getEffectiveCellText(cells, 20), "Universal Truth");
  EXPECT_EQ(getEffectiveCellText(cells, 30), "Universal Truth");
  EXPECT_EQ(getEffectiveCellText(cells, 40), "Independent Content");

  // Rank retrieval
  const auto rank = getCloneRank(cells, 20);
  ASSERT_EQ(rank.size(), 3U);
  EXPECT_EQ(rank[0], 10U);
  EXPECT_EQ(rank[1], 20U);
  EXPECT_EQ(rank[2], 30U);

  // Updating text on clone 20 updates the master cell 10 and reflects across
  // the rank
  updateMasterText(cells, 20, "Updated Universal Truth");
  EXPECT_EQ(cells[10].text(), "Updated Universal Truth");
  EXPECT_EQ(getEffectiveCellText(cells, 10), "Updated Universal Truth");
  EXPECT_EQ(getEffectiveCellText(cells, 20), "Updated Universal Truth");
  EXPECT_EQ(getEffectiveCellText(cells, 30), "Updated Universal Truth");
}

TEST(ZzCoreTest, ACloneOfANonStringCellReadsItsMasterAndNotItself) {
  // A clone shows its master's content. That held for text and quietly failed
  // for everything else: Cell::text() answers with an empty view for the
  // double, bool and blob alternatives, so "the master has no text" and "the
  // master is not a string" were the same condition, and the clone fell back
  // to whatever it happened to be carrying itself.
  std::unordered_map<CellID, Cell> cells;

  cells[10]                       = makeCell(10, "master");
  cells[10].data                  = 42.5;
  cells[10].dimensions["d.clone"] = LinkPairs{.pos = 20, .neg = 0};

  // Stale text on the clone, which is exactly what the fallback would return.
  cells[20]                       = makeCell(20, "xudu_clone");
  cells[20].data                  = std::string{"stale"};
  cells[20].dimensions["d.clone"] = LinkPairs{.pos = 0, .neg = 10};

  EXPECT_EQ(getEffectiveCellText(cells, 10), "42.5");
  EXPECT_EQ(getEffectiveCellText(cells, 20), "42.5")
      << "the clone read its own text instead of its master's value";

  // The other two alternatives, and a master that really is an empty string --
  // which must read as empty rather than reaching for the clone's own text.
  std::unordered_map<CellID, Cell> flags;
  flags[1]                       = makeCell(1, "master");
  flags[1].data                  = true;
  flags[1].dimensions["d.clone"] = LinkPairs{.pos = 2, .neg = 0};
  flags[2]                       = makeCell(2, "xudu_clone");
  flags[2].data                  = std::string{"stale"};
  flags[2].dimensions["d.clone"] = LinkPairs{.pos = 0, .neg = 1};
  EXPECT_EQ(getEffectiveCellText(flags, 2), "true");

  std::unordered_map<CellID, Cell> blank;
  blank[1]                       = makeCell(1, "master");
  blank[1].data                  = std::string{};
  blank[1].dimensions["d.clone"] = LinkPairs{.pos = 2, .neg = 0};
  blank[2]                       = makeCell(2, "xudu_clone");
  blank[2].data                  = std::string{"stale"};
  blank[2].dimensions["d.clone"] = LinkPairs{.pos = 0, .neg = 1};
  EXPECT_EQ(getEffectiveCellText(blank, 2), "");

  // A blob is not text, and saying so is not the same as saying it is empty:
  // callers that want the bytes ask Cell::blob().
  std::unordered_map<CellID, Cell> binary;
  binary[1]      = makeCell(1, "master");
  binary[1].data = std::vector<std::uint8_t>{0x89, 0x50, 0x4e, 0x47};
  EXPECT_EQ(getEffectiveCellText(binary, 1), "");
  ASSERT_NE(binary[1].blob(), nullptr);
  EXPECT_EQ(binary[1].blob()->size(), 4U);
}

TEST(ZzCoreTest, ADoubleRendersShortestAndParsesBackToItself) {
  // Whatever a cell's number is, reading it as text and parsing it again has
  // to give the number back -- that is what makes a scalar cell addressable
  // as primedia at all.
  for (const double value : {42.5, 0.1, 1.0, -0.0, 1e300, 3.141592653589793,
                             std::numeric_limits<double>::min()}) {
    const auto text = cellDataAsText(CellData{value});
    ASSERT_FALSE(text.empty()) << value;
    double parsed = 0;
    const auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), parsed);
    EXPECT_EQ(ec, std::errc{}) << text;
    EXPECT_EQ(ptr, text.data() + text.size()) << text;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(parsed),
              std::bit_cast<std::uint64_t>(value))
        << "round trip changed " << text;
  }
  EXPECT_EQ(cellDataAsText(CellData{true}), "true");
  EXPECT_EQ(cellDataAsText(CellData{false}), "false");
}

TEST(ZzCoreTest, DescribeLoadErrorsAndDiagnosticsCount) {
  EXPECT_FALSE(describe(LoadError::Kind::FileUnreadable).empty());
  EXPECT_FALSE(describe(LoadError::Kind::MalformedYaml).empty());
  EXPECT_FALSE(describe(LoadError::Kind::SchemaViolation).empty());
  EXPECT_FALSE(describe(LoadError::Kind::DanglingFocus).empty());

  Diagnostics diag;
  EXPECT_EQ(diag.count(Severity::Error), 0U);
  EXPECT_EQ(diag.count(Severity::Warning), 0U);

  diag.error("test error");
  diag.warn("test warning");

  EXPECT_EQ(diag.count(Severity::Error), 1U);
  EXPECT_EQ(diag.count(Severity::Warning), 1U);
}

TEST(ZzCoreTest, SelectSliceFileAndResolveXdgPath) {
  std::vector<std::string> files = {
      "readme.txt",
      "assets/icon.png",
      "data/home_slice.yaml",
      "data/secondary.zz.yaml",
  };

  // Preferred match
  EXPECT_EQ(selectSliceFile(files, "secondary.zz.yaml"),
            "data/secondary.zz.yaml");

  // Fallback to first .yaml file
  EXPECT_EQ(selectSliceFile(files, "nonexistent.yaml"), "data/home_slice.yaml");

  // No yaml files
  std::vector<std::string> noYaml = {"file1.txt", "file2.png"};
  EXPECT_EQ(selectSliceFile(noYaml, ""), "");

  // resolveXdgPath
  const auto path =
      resolveXdgPath("XDG_DATA_HOME", ".local/share", "zigzag", "default.yaml");
  EXPECT_FALSE(path.empty());
}
