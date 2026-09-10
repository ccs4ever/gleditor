/**
 * @file microversion.cpp
 * @brief Names for states in a branching time.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <xudu/core/microversion.hpp>

namespace {

using xudu::MicroversionId;

/// The names along a path, which is what most of these are really asserting
/// about.
std::vector<std::string> pathNames(const MicroversionId &id) {
  std::vector<std::string> names;
  for (const auto &step : id.path()) {
    names.push_back(step.str());
  }
  return names;
}

TEST(MicroversionTest, theEmptyNameIsStateZero) {
  EXPECT_TRUE(MicroversionId{}.isZero());
  EXPECT_EQ(MicroversionId{}.str(), "0");
  EXPECT_TRUE(MicroversionId::parse("0").isZero());
  EXPECT_TRUE(MicroversionId::parse("").isZero());
}

TEST(MicroversionTest, namesSurviveBeingWrittenAndReadBack) {
  for (const auto *name : {"1", "2", "2a4", "2a4b3", "17c2"}) {
    EXPECT_EQ(MicroversionId::parse(name).str(), name);
  }
}

TEST(MicroversionTest, aBranchOffTheNullDocumentReadsBack) {
  // The null document branches like any other state, and quoting a passage
  // into a second document is how that happens: the new document starts from
  // nothing and its first state is a1.
  //
  // str() always wrote these; parse() refused them, so a store holding a
  // quotation into a second document could be written and never read.
  for (const auto *name : {"a1", "a2", "b1", "a1b3"}) {
    EXPECT_EQ(MicroversionId::parse(name).str(), name);
  }

  // Not merely parseable -- the same name the store would produce.
  EXPECT_EQ(MicroversionId{}.branch(1).str(), "a1");
  EXPECT_EQ(MicroversionId::parse("a1"), MicroversionId{}.branch(1));
  // And it still comes back to the root, which is what makes it a branch of
  // the null document rather than a state of its own.
  EXPECT_TRUE(MicroversionId::parse("a1").parent().isZero());
}

TEST(MicroversionTest, malformedNamesAreRefused) {
  // A zero segment and a trailing letter with no number: neither could have
  // been produced.
  //
  // A leading letter used to be in this list. It did not belong: str()
  // produces one for any branch off the null document, so refusing it made a
  // name the program writes a name the program cannot read. A run of two or
  // more letters used to be in this list too, for the same reason: past z, a
  // branch is aa, ab, ... and str() writes exactly that, so refusing it here
  // made those unreadable as well.
  EXPECT_THROW((void)MicroversionId::parse("2a0"), std::invalid_argument);
  EXPECT_THROW((void)MicroversionId::parse("2a"), std::invalid_argument);
  EXPECT_THROW((void)MicroversionId::parse("2-4"), std::invalid_argument);
}

TEST(MicroversionTest, branchLettersRunPastZLikeASpreadsheetColumn) {
  // Bijective base 26: z (ordinal 26) increments to aa (27), not to a
  // wraparound "ba" or a two-digit-in-base-26 reading.
  EXPECT_EQ(MicroversionId::branchLetters(1), "a");
  EXPECT_EQ(MicroversionId::branchLetters(26), "z");
  EXPECT_EQ(MicroversionId::branchLetters(27), "aa");
  EXPECT_EQ(MicroversionId::branchLetters(28), "ab");
  EXPECT_EQ(MicroversionId::branchLetters(702), "zz");
  EXPECT_EQ(MicroversionId::branchLetters(703), "aaa");
}

TEST(MicroversionTest, multiLetterBranchNamesSurviveBeingWrittenAndReadBack) {
  for (const auto *name : {"2aa1", "2az3", "2ba1", "2zz9", "2aaa1"}) {
    EXPECT_EQ(MicroversionId::parse(name).str(), name);
  }
}

TEST(MicroversionTest, branchOrdinalPastZProducesTheRightLetters) {
  EXPECT_EQ(MicroversionId::parse("2").branch(26).str(), "2z1");
  EXPECT_EQ(MicroversionId::parse("2").branch(27).str(), "2aa1");
  EXPECT_EQ(MicroversionId::parse("2").branch(28).str(), "2ab1");
}

TEST(MicroversionTest, multiLetterBranchesOrderByOrdinalNotLexicographically) {
  // "aa" (ordinal 27) is the branch after "z" (ordinal 26), even though the
  // string "aa" sorts before "z" lexicographically.
  const auto z  = MicroversionId::parse("2").branch(26);
  const auto aa = MicroversionId::parse("2").branch(27);
  EXPECT_LT(z, aa);
  EXPECT_FALSE(aa < z);
}

TEST(MicroversionTest, anImpossiblyLargeNumberIsRefused) {
  EXPECT_THROW((void)MicroversionId::parse("99999999999999999999"),
               std::invalid_argument);
}

TEST(MicroversionTest, theNextStateContinuesTheChain) {
  EXPECT_EQ(MicroversionId::parse("2a4").next().str(), "2a5");
  EXPECT_EQ(MicroversionId::parse("2").next().str(), "3");
  EXPECT_EQ(MicroversionId{}.next().str(), "1");
}

TEST(MicroversionTest, aBranchRestartsTheNumbering) {
  // Nelson's own example: "A branch is given a letter, after which new
  // integers begin with 1 again."
  EXPECT_EQ(MicroversionId::parse("2").branch(1).str(), "2a1");
  EXPECT_EQ(MicroversionId::parse("2a4").branch(2).str(), "2a4b1");
}

TEST(MicroversionTest, theParentIsOneStepBack) {
  EXPECT_EQ(MicroversionId::parse("2a4").parent().str(), "2a3");
  // The first state of a branch hangs off whatever it branched from, so the
  // whole segment goes rather than its number reaching zero.
  EXPECT_EQ(MicroversionId::parse("2a1").parent().str(), "2");
  EXPECT_EQ(MicroversionId::parse("1").parent().str(), "0");
}

TEST(MicroversionTest, walkingBackFromZeroTerminates) {
  // A loop that walks parents until isZero() must not run off the end.
  EXPECT_TRUE(MicroversionId{}.parent().isZero());
}

TEST(MicroversionTest, theNameSpellsOutEveryStateOnTheWayToIt) {
  // The property the whole design rests on: a name is enough to replay from,
  // so nothing has to store a version.
  EXPECT_THAT(pathNames(MicroversionId::parse("2a4")),
              testing::ElementsAre("1", "2", "2a1", "2a2", "2a3", "2a4"));
}

TEST(MicroversionTest, aPathAcrossTwoBranches) {
  EXPECT_THAT(pathNames(MicroversionId::parse("2a2b2")),
              testing::ElementsAre("1", "2", "2a1", "2a2", "2a2b1", "2a2b2"));
}

TEST(MicroversionTest, stateZeroHasNothingToReplay) {
  EXPECT_THAT(pathNames(MicroversionId{}), testing::IsEmpty());
}

TEST(MicroversionTest, aPathEndsAtTheStateItNames) {
  const auto id = MicroversionId::parse("3b7");
  EXPECT_EQ(id.path().back(), id);
}

TEST(MicroversionTest, ancestryFollowsTheChain) {
  const auto two = MicroversionId::parse("2");
  EXPECT_TRUE(two.isAncestorOf(MicroversionId::parse("3")));
  EXPECT_TRUE(two.isAncestorOf(MicroversionId::parse("2a1")));
  EXPECT_TRUE(two.isAncestorOf(MicroversionId::parse("2a4b2")));
  EXPECT_FALSE(two.isAncestorOf(MicroversionId::parse("1")));
  EXPECT_FALSE(two.isAncestorOf(two));
}

TEST(MicroversionTest, branchesAreNotAncestorsOfEachOther) {
  // The point of a branching time: two futures of the same state are unrelated
  // to each other, and neither is behind the other.
  const auto left  = MicroversionId::parse("2a1");
  const auto right = MicroversionId::parse("2b1");
  EXPECT_FALSE(left.isAncestorOf(right));
  EXPECT_FALSE(right.isAncestorOf(left));
}

TEST(MicroversionTest, zeroPrecedesEverythingButItself) {
  EXPECT_TRUE(MicroversionId{}.isAncestorOf(MicroversionId::parse("1")));
  EXPECT_FALSE(MicroversionId{}.isAncestorOf(MicroversionId{}));
}

// -- where the segments are kept ---------------------------------------------
//
// A name holds its first two segments inside itself and spills to the heap
// past that, so every one of these crosses the 2-to-3 boundary in both
// directions. The names below are chosen for their segment counts -- "2" is
// one, "2a4" two, "2a4b3" three -- and none of it should be visible from out
// here, which is the whole assertion.

namespace {
/// One name of each length either side of the inline boundary.
const std::vector<std::string> &lengthsAcrossTheBoundary() {
  static const std::vector<std::string> names{"1", "2a4", "2a4b3", "2a4b3c2",
                                              "2a4b3c2d9"};
  return names;
}
} // namespace

TEST(MicroversionTest, aNameSurvivesBeingCopiedAndMovedAtEveryLength) {
  for (const auto &name : lengthsAcrossTheBoundary()) {
    const auto original = MicroversionId::parse(name);

    const MicroversionId copied{original};
    EXPECT_EQ(copied.str(), name);
    EXPECT_EQ(copied, original) << "a copy is the same name";

    MicroversionId source{original};
    const MicroversionId moved{std::move(source)};
    EXPECT_EQ(moved.str(), name);
    // A moved-from name is state zero rather than whatever it used to be, so
    // reading one is defined rather than merely unlikely to crash.
    EXPECT_TRUE(source.isZero()); // NOLINT(bugprone-use-after-move)

    // Assigning a name to itself is the case that gives back the storage and
    // then copies out of it. Through a reference because writing it directly
    // is a warning about the test rather than about the class.
    MicroversionId selfAssigned{original};
    MicroversionId &alias = selfAssigned;
    selfAssigned          = alias;
    EXPECT_EQ(selfAssigned.str(), name) << "self-assignment freed the storage";
    selfAssigned = std::move(alias);
    EXPECT_EQ(selfAssigned.str(), name) << "self-move freed the storage";
  }
}

TEST(MicroversionTest, assignmentCrossesTheInlineBoundaryBothWays) {
  // The two orders are different code: one grows into a heap block it did not
  // have, the other gives one back. A leak lives in the first and a
  // double-free in the second.
  for (const auto &from : lengthsAcrossTheBoundary()) {
    for (const auto &to : lengthsAcrossTheBoundary()) {
      MicroversionId target = MicroversionId::parse(to);
      const auto wanted     = MicroversionId::parse(from);

      target = wanted;
      EXPECT_EQ(target.str(), from) << to << " := " << from;

      MicroversionId movedFrom = MicroversionId::parse(from);
      target                   = MicroversionId::parse(to);
      target                   = std::move(movedFrom);
      EXPECT_EQ(target.str(), from) << to << " := move " << from;
    }
  }
}

TEST(MicroversionTest, steppingAroundTheInlineBoundaryKeepsTheName) {
  // Two segments is the last inline length, so branching off one spills and
  // walking back from the spilled name comes home again. Both directions are
  // a change of storage that no caller asked for and none should notice.
  const auto twoSegments = MicroversionId::parse("2a4");
  const auto spilled     = twoSegments.branch(2);
  EXPECT_EQ(spilled.str(), "2a4b1");
  EXPECT_EQ(spilled.parent(), twoSegments) << "a branch's first state hangs "
                                              "off what it branched from";
  EXPECT_EQ(spilled.next().str(), "2a4b2");
  EXPECT_EQ(spilled.next().parent(), spilled);

  // And the long way round: five segments back down to one, one step at a
  // time, which is what Store::opsFor walks.
  auto walking = MicroversionId::parse("2a4b3c2d9");
  std::vector<std::string> seen;
  while (!walking.isZero()) {
    seen.push_back(walking.str());
    walking = walking.parent();
  }
  EXPECT_THAT(seen, testing::Contains("2a4b3c2d1"));
  EXPECT_THAT(seen, testing::Contains("2a4b3"));
  EXPECT_EQ(seen.back(), "1");
}

} // namespace
