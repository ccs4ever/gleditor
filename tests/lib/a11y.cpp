/**
 * @file a11y.cpp
 * @brief What a screen reader would be told, checked without one.
 *
 * The accessibility tree is the second description of the user interface: the
 * first is quads on a screen, and nothing can read those back. That makes this
 * the only place the second description is checked at all, and it makes the
 * arithmetic in it worth checking closely -- an offset that is a byte where it
 * should be a character puts the reported cursor somewhere other than where it
 * is drawn, and nothing on screen would look wrong.
 *
 * Nothing here needs a device, a window, or an accessibility bus. That is the
 * point of a11y::Tree being plain data.
 */
#include <gtest/gtest.h>

#include <numeric>
#include <string>
#include <vector>

#include <gleditor/a11y/publisher.hpp>
#include <gleditor/a11y/tree.hpp>

namespace {

namespace a11y = gleditor::a11y;

/// The sum of a slice, for the invariants the platforms rely on.
std::size_t total(const std::vector<std::uint8_t> &values) {
  return std::accumulate(values.begin(), values.end(), std::size_t{0});
}

/// A source that says whatever it is told to, for testing the publisher.
class Fixed : public a11y::Source {
public:
  explicit Fixed(std::string aLabel) : label(std::move(aLabel)) {}

  void describe(a11y::Builder &into) override {
    auto &node   = into.add(0, a11y::Role::Group);
    node.label   = label;
    node.actions = a11y::bit(a11y::Action::Click);
    into.contribute(into.id(0));
    described++;
  }

  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    return revision;
  }

  bool performAction(const std::uint64_t nodeId, const a11y::Action action,
                     const std::string_view value) override {
    acted.emplace_back(nodeId, action, std::string{value});
    return true;
  }

  std::string label;
  std::uint64_t revision{1};
  int described{};
  std::vector<std::tuple<std::uint64_t, a11y::Action, std::string>> acted;
};

} // namespace

// -- counting characters ------------------------------------------------------

TEST(A11yTextTest, everyCharacterIsMeasuredInBytes) {
  // The sum of the lengths must be the length of the string: it is what the
  // platforms use to walk it, and a run whose lengths do not add up is one
  // they read off the end of.
  const std::string text = "aé漢🌍";
  const auto lengths     = a11y::characterLengths(text);
  EXPECT_EQ(lengths, (std::vector<std::uint8_t>{1, 2, 3, 4}));
  EXPECT_EQ(total(lengths), text.size());
}

TEST(A11yTextTest, textThatIsNotQuiteUtf8IsStillDescribable) {
  // A document is whatever somebody typed or whatever a file held. Refusing to
  // describe it would lose the whole document rather than one character of it.
  const std::string text{'a', '\xFF', 'b'};
  EXPECT_EQ(total(a11y::characterLengths(text)), text.size());
}

TEST(A11yTextTest, aByteOffsetBecomesACharacterIndex) {
  const std::string text = "aé漢";
  EXPECT_EQ(a11y::characterIndexOf(text, 0), 0U);
  EXPECT_EQ(a11y::characterIndexOf(text, 1), 1U);
  EXPECT_EQ(a11y::characterIndexOf(text, 3), 2U);
  EXPECT_EQ(a11y::characterIndexOf(text, 6), 3U);
  // Past the end is the end, which is where a caret typed to the end of a
  // document sits.
  EXPECT_EQ(a11y::characterIndexOf(text, 99), 3U);
}

TEST(A11yTextTest, anOffsetInsideACharacterNamesThatCharacter) {
  // Byte 2 is the middle of the two-byte "é". Reporting the character it is
  // inside is what makes a reported cursor agree with a drawn one.
  EXPECT_EQ(a11y::characterIndexOf("aé", 2), 1U);
}

// -- words --------------------------------------------------------------------

TEST(A11yWordTest, aWordBeginsWhereTheSpaceBeforeItEnds) {
  // Sorted character indices, which is what the platforms require. The space
  // belongs to the word in front of it, so "next word" lands on a word.
  EXPECT_EQ(a11y::wordStarts("one two three"),
            (std::vector<std::uint8_t>{0, 4, 8}));
}

TEST(A11yWordTest, leadingWhitespaceIsAWordOfItsOwn) {
  // What indented text needs: moving by words should stop at the start of the
  // line rather than skipping the indent.
  EXPECT_EQ(a11y::wordStarts("    indented"),
            (std::vector<std::uint8_t>{0, 4}));
}

TEST(A11yWordTest, aRunOfNothingButSpacesIsStillOneWord) {
  EXPECT_EQ(a11y::wordStarts("   "), (std::vector<std::uint8_t>{0}));
  EXPECT_TRUE(a11y::wordStarts("").empty()) << "and nothing at all is none";
}

TEST(A11yWordTest, wordsAreCountedInCharactersRatherThanBytes) {
  // "héllo wörld": the accented letters are two bytes each, so a count in
  // bytes would put the second word two places past where it is.
  EXPECT_EQ(a11y::wordStarts("héllo wörld"), (std::vector<std::uint8_t>{0, 6}));
}

// -- cutting text into runs ---------------------------------------------------

TEST(A11yRunTest, aRunIsALineAndKeepsItsLineBreak) {
  const std::string text = "one\ntwo\nthree";
  const auto breaks      = a11y::runBreaks(text);
  ASSERT_EQ(breaks.size(), 4U);
  EXPECT_EQ(std::string_view{text}.substr(breaks[0], breaks[1] - breaks[0]),
            "one\n")
      << "the break belongs to the line it ends";
  EXPECT_EQ(std::string_view{text}.substr(breaks[1], breaks[2] - breaks[1]),
            "two\n");
  EXPECT_EQ(std::string_view{text}.substr(breaks[2], breaks[3] - breaks[2]),
            "three");
}

TEST(A11yRunTest, emptyTextIsOneEmptyRun) {
  // A document with nothing in it still has a place the caret can be, and a
  // document with no runs at all has nowhere to report it.
  const auto breaks = a11y::runBreaks("");
  ASSERT_EQ(breaks.size(), 2U);
  EXPECT_EQ(breaks[0], 0U);
  EXPECT_EQ(breaks[1], 0U);
}

TEST(A11yRunTest, aLineTooLongToDescribeIsCut) {
  // Word starts are character indices carried a byte apiece, so a word
  // beginning past the two hundred and fifty-fifth character of a run cannot
  // be described at all. Every run has to stay short enough that they can be.
  std::string text;
  while (text.size() < a11y::runLimit * 3) {
    text += "word ";
  }
  const auto breaks = a11y::runBreaks(text);
  EXPECT_GT(breaks.size(), 2U) << "it was cut somewhere";
  for (std::size_t run = 0; run + 1 < breaks.size(); run++) {
    const auto piece = std::string_view{text}.substr(
        breaks[run], breaks[run + 1] - breaks[run]);
    const auto starts = a11y::wordStarts(piece);
    EXPECT_LE(a11y::characterLengths(piece).size(), 255U);
    EXPECT_TRUE(starts.empty() || starts.back() < 255U)
        << "every word start fits in the byte the platforms give it";
  }
}

TEST(A11yRunTest, aCutFallsOnAGapWhereThereIsOne) {
  std::string text;
  while (text.size() < a11y::runLimit + 20) {
    text += "word ";
  }
  const auto breaks = a11y::runBreaks(text);
  ASSERT_GE(breaks.size(), 3U);
  // Cutting a word in half is worse than a short line, so a run should end
  // just after a space rather than in the middle of "word".
  EXPECT_EQ(text[breaks[1] - 1], ' ');
}

TEST(A11yRunTest, aVeryLongWordIsCutAnywayRatherThanLeftUndescribable) {
  // No gap to cut at, and it still has to be cut: a run nothing can index is
  // worse than a break in a place nobody chose.
  const std::string text(a11y::runLimit * 2, 'x');
  const auto breaks = a11y::runBreaks(text);
  EXPECT_GT(breaks.size(), 2U);
  for (std::size_t run = 0; run + 1 < breaks.size(); run++) {
    EXPECT_LE(breaks[run + 1] - breaks[run], a11y::runLimit);
  }
}

TEST(A11yRunTest, everyRunTogetherIsTheWholeText) {
  // The runs are what an assistive technology reads; a byte that is in none of
  // them is a byte nobody can read.
  const std::string text =
      "one\n\nthree\n" + std::string(a11y::runLimit + 5, 'y') + "\nlast";
  const auto breaks = a11y::runBreaks(text);
  std::string rebuilt;
  for (std::size_t run = 0; run + 1 < breaks.size(); run++) {
    rebuilt += text.substr(breaks[run], breaks[run + 1] - breaks[run]);
  }
  EXPECT_EQ(rebuilt, text);
}

// -- node identity ------------------------------------------------------------

TEST(A11yIdsTest, anIdSaysWhoseItIs) {
  // What routes an action request back to whatever made the node, and what
  // stops two elements having to agree about numbering.
  const auto id = a11y::Ids::of(a11y::Ids::form, 42);
  EXPECT_EQ(a11y::Ids::ownerOf(id), a11y::Ids::form);
  EXPECT_EQ(a11y::Ids::localOf(id), 42U);
  EXPECT_NE(id, a11y::Ids::of(a11y::Ids::documents, 42))
      << "the same number from two owners is two nodes";
}

// -- the publisher ------------------------------------------------------------

TEST(A11yPublisherTest, everySourceHangsOffOneWindow) {
  a11y::Publisher publisher("test", "gleditor", "0");
  Fixed first("first");
  Fixed second("second");
  publisher.addSource(&first);
  publisher.addSource(&second);

  publisher.rebuild(800, 600);
  const auto tree = publisher.snapshot();

  ASSERT_FALSE(tree.empty());
  const auto *const root = tree.find(tree.root());
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->role, a11y::Role::Window);
  EXPECT_EQ(root->label, "test");
  ASSERT_TRUE(root->bounds.has_value());
  EXPECT_EQ(*root->bounds, (a11y::Rect{0.0, 0.0, 800.0, 600.0}));
  ASSERT_EQ(root->children.size(), 2U);
  EXPECT_EQ(tree.find(root->children[0])->label, "first");
  EXPECT_EQ(tree.find(root->children[1])->label, "second");
}

TEST(A11yPublisherTest, nothingIsRebuiltWhileNothingChanges) {
  // A screen reader that is told about the same tree sixty times a second is a
  // screen reader that says the same thing sixty times a second.
  a11y::Publisher publisher("test", "gleditor", "0");
  Fixed source("thing");
  publisher.addSource(&source);

  publisher.rebuild(800, 600);
  publisher.rebuild(800, 600);
  publisher.rebuild(800, 600);
  EXPECT_EQ(source.described, 1);

  source.revision++;
  publisher.rebuild(800, 600);
  EXPECT_EQ(source.described, 2);
}

TEST(A11yPublisherTest, theWindowTitleIsWhatTheWindowIsCalled) {
  a11y::Publisher publisher("test", "gleditor", "0");
  Fixed source("thing");
  publisher.addSource(&source);
  publisher.rebuild(800, 600);

  publisher.setWindowTitle("Publishing an essay");
  publisher.rebuild(800, 600);
  EXPECT_EQ(publisher.snapshot().find(publisher.snapshot().root())->label,
            "Publishing an essay");
}

TEST(A11yPublisherTest, aTreeCanBeReadAsText) {
  // What --dump-a11y prints, and the only way to check a description without
  // an accessibility bus and somebody listening to it.
  a11y::Publisher publisher("Xudu", "gleditor", "0");
  Fixed source("the links");
  publisher.addSource(&source);
  publisher.rebuild(800, 600);

  const auto text = a11y::Publisher::describe(publisher.snapshot());
  EXPECT_TRUE(text.contains("window \"Xudu\"")) << text;
  EXPECT_TRUE(text.contains("  group \"the links\"")) << text;
  EXPECT_TRUE(text.contains("focus: Xudu")) << text;
}

TEST(A11yPublisherTest, withNoPlatformThereIsNothingToDoAndNothingBreaks) {
  // A build without AccessKit, and a machine with no assistive technology
  // running, are the same case here: the tree is still built, and everything
  // that would send it does nothing.
  a11y::Publisher publisher("test", "gleditor", "0");
  Fixed source("thing");
  publisher.addSource(&source);
  publisher.rebuild(800, 600);

  publisher.publish();
  publisher.setWindowFocused(true);
  publisher.setWindowBounds({0, 0, 100, 100}, {1, 1, 99, 99});
  EXPECT_EQ(publisher.pumpActions(), 0U);
  EXPECT_FALSE(publisher.snapshot().empty()) << "the description is still made";
}

// vi: set sw=2 sts=2 ts=2 et:
