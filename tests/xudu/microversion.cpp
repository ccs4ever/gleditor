/**
 * @file microversion.cpp
 * @brief Names for states in a branching time.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
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
  EXPECT_EQ(MicroversionId{}.branch('a').str(), "a1");
  EXPECT_EQ(MicroversionId::parse("a1"), MicroversionId{}.branch('a'));
  // And it still comes back to the root, which is what makes it a branch of
  // the null document rather than a state of its own.
  EXPECT_TRUE(MicroversionId::parse("a1").parent().isZero());
}

TEST(MicroversionTest, malformedNamesAreRefused) {
  // Two letters running, a zero segment, and a trailing letter with no
  // number: none of these could have been produced.
  //
  // A leading letter used to be in this list. It did not belong: str()
  // produces one for any branch off the null document, so refusing it made a
  // name the program writes a name the program cannot read.
  EXPECT_THROW((void)MicroversionId::parse("2ab1"), std::invalid_argument);
  EXPECT_THROW((void)MicroversionId::parse("2a0"), std::invalid_argument);
  EXPECT_THROW((void)MicroversionId::parse("2a"), std::invalid_argument);
  EXPECT_THROW((void)MicroversionId::parse("2-4"), std::invalid_argument);
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
  EXPECT_EQ(MicroversionId::parse("2").branch('a').str(), "2a1");
  EXPECT_EQ(MicroversionId::parse("2a4").branch('b').str(), "2a4b1");
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

} // namespace

// vi: set sw=2 sts=2 ts=2 et:
