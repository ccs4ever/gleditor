/**
 * @file vlace_xanadu_test.cpp
 * @brief Stage 7 tests: the std:xanadu authoring interface
 *        (design/vlace-functional-language.md §13).
 *
 * Authoring lowers to the ephemeral ArenaManifold write path (§13.1): makeText/
 * makeDim/setLink/cellText/linked are PURE in vlace's type system (they build
 * in the arena, mint no persistent op), and `promote` is the one authoring
 * effect, typed `Cell -> IO Int`. These tests check: (1) the verbs type as the
 * spec states; (2) a cell's content round-trips through the arena; (3) a link
 * set along a dimension is readable back (bidirectional by construction); and
 * (4) promote appears in a program's type as IO, not as a pure value.
 */
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "common/xanadu/vlace/driver.hpp"

namespace {

using xanadu::vlace::Driver;

std::string typeOf(const std::string &src) {
  Driver d;
  auto r = d.run(src);
  return r ? r->type : std::string("ERR:") + Driver::formatError(r.error());
}

// Evaluate with type-checking on; return the rendered value or an ERR string.
std::string valueOf(const std::string &src) {
  Driver d;
  auto r = d.run(src);
  return r ? r->value : std::string("ERR:") + Driver::formatError(r.error());
}

// Run an IO program and return the final wrapped value (for promote).
std::string runIOValue(const std::string &src) {
  Driver d;
  std::istringstream in;
  std::ostringstream out;
  auto r = d.runIO(src, in, out);
  return r ? r->value : std::string("ERR:") + Driver::formatError(r.error());
}

// --- Verbs type as §13 states --------------------------------------------

TEST(VlaceXanaduTest, MakeTextIsCell) {
  EXPECT_EQ(typeOf("makeText \"hello\""), "Cell");
}

TEST(VlaceXanaduTest, MakeDimIsDim) {
  EXPECT_EQ(typeOf("makeDim \"d.doc\""), "Dim");
}

TEST(VlaceXanaduTest, CellTextIsString) {
  EXPECT_EQ(typeOf("cellText (makeText \"x\")"), "String");
}

TEST(VlaceXanaduTest, SetLinkIsUnit) {
  EXPECT_EQ(
      typeOf("setLink (makeText \"a\") (makeDim \"d.doc\") (makeText \"b\")"),
      "Unit");
}

TEST(VlaceXanaduTest, DimensionNamedIsOptionDim) {
  EXPECT_EQ(typeOf("dimensionNamed \"d.doc\""), "Option Dim");
}

TEST(VlaceXanaduTest, LinkedIsOptionCell) {
  EXPECT_EQ(typeOf("linked (makeText \"a\") (makeDim \"d.doc\")"),
            "Option Cell");
}

TEST(VlaceXanaduTest, PromoteIsIOEffect) {
  // The one authoring effect: promote appears in the type as IO (§13.1).
  EXPECT_EQ(typeOf("promote (makeText \"x\")"), "IO Int");
}

TEST(VlaceXanaduTest, PureArenaVerbsAreNotIO) {
  // makeText/cellText are pure: no IO in their type (§13.1 arena is pure).
  EXPECT_EQ(typeOf("cellText (makeText \"pure\")"), "String");
}

// --- Content round-trips through the arena --------------------------------

TEST(VlaceXanaduTest, CellTextRoundTripsContent) {
  EXPECT_EQ(valueOf("cellText (makeText \"round trip\")"), "round trip");
}

TEST(VlaceXanaduTest, DistinctCellsHoldDistinctContent) {
  // Build two cells, read the first; content is not confused across cells.
  const char *src = "let a = makeText \"first\" in "
                    "let b = makeText \"second\" in cellText a";
  EXPECT_EQ(valueOf(src), "first");
}

// --- A link along a dimension is readable back ----------------------------

TEST(VlaceXanaduTest, SetLinkThenLinkedReadsNeighbour) {
  // Author a -> b on d.doc, then read a's posward neighbour and its text. The
  // engine sets both reciprocal edges, so `linked a dDoc` resolves to b.
  //
  // setLink's arena write is an effect-looking pure value (Unit); in a lazy
  // language it happens only when forced, so the write is *sequenced* by
  // scrutinising its Unit result before the read -- the honest way to order
  // two arena writes without a mutable builder (§13.5 threads an Arena for the
  // same reason).
  const char *src = "let dDoc = makeDim \"d.doc\" in "
                    "let a = makeText \"head\" in "
                    "let b = makeText \"tail\" in "
                    "case setLink a dDoc b of "
                    "  Unit => (case linked a dDoc of "
                    "             None => \"no link\" ; "
                    "             Some n => cellText n)";
  Driver d;
  d.withTypeChecking(false);
  auto r = d.run(src);
  ASSERT_TRUE(r) << Driver::formatError(r.error());
  EXPECT_EQ(r->value, "tail");
}

// --- promote is performed only at the boundary ----------------------------

TEST(VlaceXanaduTest, PromoteRunsAtBoundary) {
  // Running `promote cell` performs the one authoring effect; the driverless
  // runner reports the arena op-count (>= 1 after minting one cell).
  auto v = runIOValue("promote (makeText \"persist me\")");
  ASSERT_NE(v.rfind("ERR:", 0), 0u) << v;
  // The value is an Int (the reachable/op count); at least the one cell minted.
  EXPECT_GE(std::stoll(v), 1);
}

TEST(VlaceXanaduTest, PromoteSequencedWithConsole) {
  // promote threads through >>= like any IO action: print then promote.
  Driver d;
  std::istringstream in;
  std::ostringstream out;
  auto r = d.runIO("putStr \"saving\" >> promote (makeText \"doc\")", in, out);
  ASSERT_TRUE(r) << Driver::formatError(r.error());
  EXPECT_EQ(out.str(), "saving");
  EXPECT_GE(std::stoll(r->value), 1);
}

} // namespace
