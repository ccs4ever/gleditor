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
#include <ranges>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "common/xanadu/zigzag/zzcore.hpp"
#include "common/xanadu/zigzag/zzstructure.hpp"

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

} // namespace

TEST(ZzCoreTest, CellLookupBorrowsAndFormsZeroOrOneElementRange) {
  std::unordered_map<CellID, Cell> cells;
  cells.emplace(7, makeCell(7));

  const auto found = findCell(cells, 7);
  static_assert(std::ranges::view<std::remove_cvref_t<decltype(found)>>);
  ASSERT_TRUE(found);
  EXPECT_EQ(&*found, &cells.at(7));

  std::size_t seen = 0;
  for (const Cell &cell : found) {
    EXPECT_EQ(cell.id, 7U);
    ++seen;
  }
  EXPECT_EQ(seen, 1U);

  const auto missing = findCell(cells, 8);
  EXPECT_FALSE(missing);
  seen = 0;
  for ([[maybe_unused]] const Cell &cell : missing) {
    ++seen;
  }
  EXPECT_EQ(seen, 0U);
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

  // Effective text derivation from master head
  EXPECT_EQ(getEffectiveCellText(cells, 10), "Universal Truth");
  EXPECT_EQ(getEffectiveCellText(cells, 20), "Universal Truth");
  EXPECT_EQ(getEffectiveCellText(cells, 30), "Universal Truth");
  EXPECT_EQ(getEffectiveCellText(cells, 40), "Independent Content");
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

TEST(ZzCoreTest, DiagnosticsCount) {
  Diagnostics diag;
  EXPECT_EQ(diag.count(Severity::Error), 0U);
  EXPECT_EQ(diag.count(Severity::Warning), 0U);

  diag.error("test error");
  diag.warn("test warning");

  EXPECT_EQ(diag.count(Severity::Error), 1U);
  EXPECT_EQ(diag.count(Severity::Warning), 1U);
}
