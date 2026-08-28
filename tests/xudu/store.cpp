/**
 * @file store.cpp
 * @brief The two spools, versioning on demand, and a time that branches.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <xudu/core/microversion.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/store.hpp>

namespace {

using xudu::Link;
using xudu::LinkType;
using xudu::localScroll;
using xudu::MicroversionId;
using xudu::Op;
using xudu::OpKind;
using xudu::PrimediaSpan;
using xudu::Store;

std::vector<std::string> names(const std::vector<MicroversionId> &ids) {
  std::vector<std::string> out;
  for (const auto &id : ids) {
    out.push_back(id.str());
  }
  return out;
}

TEST(StoreTest, typingIntoTheNullDocument) {
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "hello");
  EXPECT_EQ(one.str(), "1");
  EXPECT_EQ(store.textOf(one), "hello");
  EXPECT_EQ(store.textOf(MicroversionId{}), "");
}

TEST(StoreTest, editsContinueTheChain) {
  Store store;
  const auto one   = store.insert(MicroversionId{}, 0, "world");
  const auto two   = store.insert(one, 0, "hello ");
  const auto three = store.insert(two, 11, "!");

  EXPECT_EQ(names({one, two, three}),
            (std::vector<std::string>{"1", "2", "3"}));
  EXPECT_EQ(store.textOf(three), "hello world!");
}

TEST(StoreTest, everyEarlierStateIsStillThere) {
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "a");
  const auto two = store.insert(one, 1, "b");
  store.insert(two, 2, "c");

  EXPECT_EQ(store.textOf(one), "a");
  EXPECT_EQ(store.textOf(two), "ab");
}

TEST(StoreTest, editingAnEarlierStateBranchesRatherThanOverwriting) {
  // The whole of OSMIC's objection to ordinary undo: going back and typing
  // must not destroy what already followed.
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "hello");
  const auto two = store.insert(one, 5, " world");

  const auto branched = store.insert(one, 5, " there");

  EXPECT_EQ(branched.str(), "1a1");
  EXPECT_EQ(store.textOf(two), "hello world");
  EXPECT_EQ(store.textOf(branched), "hello there");
}

TEST(StoreTest, aThirdFutureGetsAThirdBranch) {
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "x");
  store.insert(one, 1, "a");
  EXPECT_EQ(store.insert(one, 1, "b").str(), "1a1");
  EXPECT_EQ(store.insert(one, 1, "c").str(), "1b1");
}

TEST(StoreTest, deletingKeepsTheContentInTheSpool) {
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "hello, world");
  const auto two = store.erase(one, 5, 2);

  EXPECT_EQ(store.textOf(two), "helloworld");
  // Not one byte less than before: deletion is "rearrange to limbo", and the
  // earlier state has to keep resolving.
  EXPECT_EQ(store.primedia().size(), 12U);
  EXPECT_EQ(store.textOf(one), "hello, world");
}

TEST(StoreTest, rearrangingMovesText) {
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "abcdef");
  EXPECT_EQ(store.textOf(store.rearrange(one, 4, 2, 0)), "efabcd");
}

TEST(StoreTest, insertBreakRecordsAPageBreakOp) {
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "abcdef");
  const auto two = store.insertBreak(one, 3);

  EXPECT_EQ(two.str(), "2");
  // A break op changes no text, same as a link op.
  EXPECT_EQ(store.textOf(two), "abcdef");
  // rebuild() replays from the null document every time -- nothing here
  // stores a version -- so this is proof the break comes back from the op
  // itself, not from some Version object it happened to be created on.
  EXPECT_THAT(store.rebuild(two).forcedBreaks(), testing::ElementsAre(3U));
}

TEST(StoreTest, aBreakSurvivesFurtherEditsInTheSameChain) {
  Store store;
  const auto one   = store.insert(MicroversionId{}, 0, "abcdef");
  const auto two   = store.insertBreak(one, 3);
  const auto three = store.insert(two, 0, "XY");

  EXPECT_EQ(store.textOf(three), "XYabcdef");
  EXPECT_THAT(store.rebuild(three).forcedBreaks(), testing::ElementsAre(5U));
}

TEST(StoreTest, aBreakDoesNotSurviveTranscludingThePassageItSitsIn) {
  // The property the whole feature exists for: content quoted elsewhere
  // carries its links (see aLinkIsPresentOnEveryDocumentQuotingTheContent
  // below) but must not carry a break that only meant something about how
  // the source happened to be laid out.
  Store store;
  const auto typed  = store.insert(MicroversionId{}, 0, "abcdef");
  const auto source = store.insertBreak(typed, 3);

  const auto quoting = store.insert(MicroversionId{}, 0, "prefix-");
  const auto quoted  = store.transclude(quoting, 7, source, 0, 6);

  EXPECT_EQ(store.textOf(quoted), "prefix-abcdef");
  EXPECT_THAT(store.rebuild(quoted).forcedBreaks(), testing::IsEmpty());
}

TEST(StoreTest, nothingStoresAVersion) {
  // Versioning on demand: what is kept is content and operations, and the
  // number of operations is the number of edits -- no version among them.
  Store store;
  auto at = MicroversionId{};
  for (int i = 0; i < 5; i++) {
    at = store.insert(at, 0, "x");
  }
  EXPECT_EQ(store.opCount(), 5U);
  EXPECT_EQ(store.textOf(at), "xxxxx");
}

TEST(StoreTest, theOperationsSpoolIsAppendOnly) {
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "a");
  Op op;
  op.kind   = OpKind::Insert;
  op.parent = MicroversionId{};
  // Overwriting a recorded state would silently rewrite every version
  // downstream of it.
  EXPECT_THROW(store.putOp(one, op), std::invalid_argument);
}

TEST(StoreTest, anOperationMustAgreeWithWhereItIsFiled) {
  Store store;
  Op op;
  op.kind   = OpKind::Insert;
  op.parent = MicroversionId::parse("7");
  EXPECT_THROW(store.putOp(MicroversionId::parse("1"), op),
               std::invalid_argument);
}

TEST(StoreTest, stateZeroIsNotProducedByAnything) {
  Store store;
  Op op;
  EXPECT_THROW(store.putOp(MicroversionId{}, op), std::invalid_argument);
}

TEST(StoreTest, theOpSequenceForAVersionIsWhatRebuildsIt) {
  // OSMIC's third server function: given a version, the ops needed for it.
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "a");
  const auto two = store.insert(one, 1, "b");
  store.insert(one, 1, "z"); // a branch, which must not appear below

  EXPECT_EQ(names(store.opsFor(two)), (std::vector<std::string>{"1", "2"}));
}

// advance() is what keeps a keystroke from costing the whole document: the
// view already holds the pieces for the state being edited, so only the one
// operation between them has to be replayed. What it must never do is give a
// different answer than replaying from the null document would.

TEST(StoreTest, advancingOneStepAgreesWithRebuildingFromNothing) {
  Store store;
  const auto one   = store.insert(MicroversionId{}, 0, "hello");
  const auto two   = store.insert(one, 5, " world");
  const auto three = store.erase(two, 0, 1);

  auto carried = store.rebuild(one);
  EXPECT_TRUE(store.advance(carried, one, two));
  EXPECT_EQ(carried.pieces(), store.rebuild(two).pieces());
  EXPECT_TRUE(store.advance(carried, two, three));
  EXPECT_EQ(carried.pieces(), store.rebuild(three).pieces());
  EXPECT_EQ(carried.materialize(store), store.textOf(three));
}

TEST(StoreTest, advancingCountsABranchAsOneStep) {
  // Going back and typing is one operation past the state forked from, so it
  // gets the cheap path too -- not just typing on the end.
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "hello");
  store.insert(one, 5, " world");
  const auto branched = store.insert(one, 5, " there");
  ASSERT_EQ(branched.str(), "1a1");

  auto carried = store.rebuild(one);
  EXPECT_TRUE(store.advance(carried, one, branched));
  EXPECT_EQ(carried.materialize(store), "hello there");
}

TEST(StoreTest, advancingRefusesAnythingFurtherThanOneStep) {
  Store store;
  const auto one   = store.insert(MicroversionId{}, 0, "a");
  const auto two   = store.insert(one, 1, "b");
  const auto three = store.insert(two, 2, "c");

  auto carried      = store.rebuild(one);
  const auto before = carried.pieces();
  // Two steps on, so the caller has to rebuild -- and the document it handed
  // in must come back exactly as it went, not half-advanced.
  EXPECT_FALSE(store.advance(carried, one, three));
  EXPECT_EQ(carried.pieces(), before);
  // Backwards is not one step on either.
  EXPECT_FALSE(store.advance(carried, two, one));
  EXPECT_EQ(carried.pieces(), before);
  // Nor is a state nothing has recorded yet.
  EXPECT_FALSE(store.advance(carried, three, MicroversionId::parse("4")));
  EXPECT_EQ(carried.pieces(), before);
}

TEST(StoreTest, advancingCarriesForcedBreaksAndLinksLikeAReplayDoes) {
  // The op kinds that change no text still have to travel: a break op moves
  // the page, a link op moves nothing, and both must land the same way.
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "abcdef");
  const auto two = store.insertBreak(one, 3);
  Link link;
  link.left.push_back(PrimediaSpan{localScroll, 0, 3});
  const auto three = store.addLink(two, link);

  auto carried = store.rebuild(one);
  ASSERT_TRUE(store.advance(carried, one, two));
  EXPECT_THAT(carried.forcedBreaks(), testing::ElementsAre(3U));
  ASSERT_TRUE(store.advance(carried, two, three));
  EXPECT_EQ(carried.pieces(), store.rebuild(three).pieces());
  EXPECT_THAT(carried.forcedBreaks(), testing::ElementsAre(3U));
}

TEST(StoreTest, transclusionSharesOneCopyOfTheContent) {
  Store store;
  const auto source = store.insert(MicroversionId{}, 0, "the quick brown fox");
  const auto before = store.primedia().size();

  const auto quoting = store.insert(MicroversionId{}, 0, "see: ");
  const auto quoted  = store.transclude(quoting, 5, source, 4, 5);

  EXPECT_EQ(store.textOf(quoted), "see: quick");
  // Only "see: " was added to the spool. Quoting cost pointers, not bytes.
  EXPECT_EQ(store.primedia().size(), before + 5);
}

TEST(StoreTest, transcludedContentIsRecognisedInBothDocuments) {
  Store store;
  const auto source = store.insert(MicroversionId{}, 0, "the quick brown fox");
  const auto quoted = store.transclude(MicroversionId{}, 0, source, 4, 5);

  const auto sourceVersion = store.rebuild(source);
  const auto quotedVersion = store.rebuild(quoted);
  const auto shared        = quotedVersion.pieces().front();

  // The same content, found by address in the document it was taken from.
  EXPECT_THAT(sourceVersion.occurrencesOf(shared),
              testing::ElementsAre(xudu::Extent{4, 9}));
  EXPECT_EQ(store.primedia().read(shared), "quick");
}

TEST(StoreTest, aTransclusionSurvivesEditingAroundIt) {
  Store store;
  const auto source = store.insert(MicroversionId{}, 0, "the quick brown fox");
  const auto quoted = store.transclude(MicroversionId{}, 0, source, 4, 5);
  const auto shared = store.rebuild(quoted).pieces().front();

  // Edit the original before the quoted passage. Its address does not move,
  // so the connection holds -- which a link naming an offset could not do.
  const auto edited = store.insert(source, 0, "I saw ");
  EXPECT_EQ(store.textOf(edited), "I saw the quick brown fox");
  EXPECT_THAT(store.rebuild(edited).occurrencesOf(shared),
              testing::ElementsAre(xudu::Extent{10, 15}));
}

TEST(StoreTest, linksAttachToContentRatherThanToPositions) {
  Store store;
  const auto source = store.insert(MicroversionId{}, 0, "the quick brown fox");
  const auto span   = store.rebuild(source).spansFor(4, 5).front();

  Link link;
  link.type  = LinkType::Comment;
  link.owner = "a reader";
  link.left.push_back(span);
  store.addLink(source, link);

  const auto found = store.linksTouching(span);
  ASSERT_EQ(found.size(), 1U);
  EXPECT_EQ(found.front()->owner, "a reader");
  EXPECT_EQ(found.front()->type, LinkType::Comment);
}

TEST(StoreTest, aLinkIsPresentOnEveryDocumentQuotingTheContent) {
  // Nelson's criterion: "Link to any portion is present on all
  // manifestations."
  Store store;
  const auto source = store.insert(MicroversionId{}, 0, "the quick brown fox");
  const auto span   = store.rebuild(source).spansFor(4, 5).front();

  Link link;
  link.left.push_back(span);
  store.addLink(source, link);

  const auto quoted      = store.transclude(MicroversionId{}, 0, source, 4, 5);
  const auto quotedPiece = store.rebuild(quoted).pieces().front();

  EXPECT_THAT(store.linksTouching(quotedPiece), testing::SizeIs(1));
}

TEST(StoreTest, aLinkChangesNoText) {
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "text");
  Link link;
  link.left.push_back(PrimediaSpan{localScroll, 0, 4});
  const auto two = store.addLink(one, link);

  EXPECT_EQ(store.textOf(two), "text");
  // Recorded as an operation all the same, so that a reader can go back to
  // before it was made.
  EXPECT_EQ(two.str(), "2");
}

TEST(StoreTest, theHypertimeMapReportsEveryFuture) {
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "x");
  store.insert(one, 1, "a");
  store.insert(one, 1, "b");
  store.insert(one, 1, "c");

  EXPECT_EQ(names(store.children(one)),
            (std::vector<std::string>{"2", "1a1", "1b1"}));
  EXPECT_THAT(store.children(MicroversionId::parse("2")), testing::IsEmpty());
}

struct StoreRoundTripTest : testing::Test {
  std::filesystem::path dir;

  void SetUp() override {
    dir =
        std::filesystem::temp_directory_path() /
        ("xudu-test-" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
         "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(dir);
  }
  void TearDown() override { std::filesystem::remove_all(dir); }
};

TEST_F(StoreRoundTripTest, aStoreSurvivesBeingWrittenAndReadBack) {
  MicroversionId branched;
  MicroversionId quoted;
  {
    Store store;
    const auto one = store.insert(MicroversionId{}, 0, "hello");
    const auto two = store.insert(one, 5, " world");
    branched       = store.insert(one, 5, " there");
    quoted         = store.transclude(two, 0, one, 0, 5);
    store.erase(quoted, 0, 1);

    Link link;
    link.type  = LinkType::Quotation;
    link.owner = "someone";
    link.left.push_back(PrimediaSpan{localScroll, 0, 5});
    link.right.push_back(PrimediaSpan{localScroll, 5, 6});
    store.addLink(two, link);
    store.save(dir.string());
  }

  Store reloaded;
  reloaded.load(dir.string());

  EXPECT_EQ(reloaded.textOf(MicroversionId::parse("1")), "hello");
  EXPECT_EQ(reloaded.textOf(MicroversionId::parse("2")), "hello world");
  EXPECT_EQ(reloaded.textOf(branched), "hello there");
  EXPECT_EQ(reloaded.textOf(quoted), "hellohello world");

  ASSERT_EQ(reloaded.links().size(), 1U);
  const auto &link = reloaded.links().begin()->second;
  EXPECT_EQ(link.type, LinkType::Quotation);
  EXPECT_EQ(link.owner, "someone");
  EXPECT_EQ(link.left.front(), (PrimediaSpan{localScroll, 0, 5}));
  EXPECT_EQ(link.right.front(), (PrimediaSpan{localScroll, 5, 6}));
}

TEST_F(StoreRoundTripTest, aForcedBreakSurvivesTheCompactBinaryFormat) {
  MicroversionId broken;
  {
    Store store;
    const auto one = store.insert(MicroversionId{}, 0, "abcdef");
    broken         = store.insertBreak(one, 3);
    store.save(dir.string());
  }

  Store reloaded;
  reloaded.load(dir.string());

  EXPECT_EQ(reloaded.textOf(broken), "abcdef");
  EXPECT_THAT(reloaded.rebuild(broken).forcedBreaks(),
              testing::ElementsAre(3U));
}

TEST_F(StoreRoundTripTest, aStoreThatIsNotThereOpensEmpty) {
  Store store;
  store.load((dir / "never-written").string());
  EXPECT_EQ(store.opCount(), 0U);
  EXPECT_TRUE(store.latest().isZero());
}

TEST_F(StoreRoundTripTest, editingContinuesAfterAReload) {
  {
    Store store;
    store.insert(MicroversionId{}, 0, "one");
    store.save(dir.string());
  }
  Store store;
  store.load(dir.string());
  const auto next = store.insert(store.latest(), 3, " two");
  EXPECT_EQ(next.str(), "2");
  EXPECT_EQ(store.textOf(next), "one two");
}

TEST_F(StoreRoundTripTest, savingTwiceOnlyAppendsWhatIsNewToThePrimediaSpool) {
  // The primedia spool only grows, so a save to the directory the last one
  // went to appends the new bytes rather than writing the whole document out
  // again. A round trip alone would not catch a duplicated spool -- stale
  // bytes past an address already handed out do not change what that address
  // reads as -- so this checks the file's size directly.
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "one");
  store.save(dir.string());
  store.save(dir.string());

  EXPECT_EQ(std::filesystem::file_size(dir / "primedia.spool"), 3U);

  Store reloaded;
  reloaded.load(dir.string());
  EXPECT_EQ(reloaded.textOf(one), "one");

  const auto two = store.insert(one, 3, " two");
  store.save(dir.string());
  EXPECT_EQ(std::filesystem::file_size(dir / "primedia.spool"), 7U);

  Store again;
  again.load(dir.string());
  EXPECT_EQ(again.textOf(two), "one two");
  EXPECT_EQ(again.primedia().bytes(), "one two");

  // A save to a directory this store has not written to before knows nothing
  // about what is already there, so it writes the whole spool out.
  const auto elsewhere = dir / "elsewhere";
  std::filesystem::create_directories(elsewhere);
  store.save(elsewhere.string());
  EXPECT_EQ(std::filesystem::file_size(elsewhere / "primedia.spool"), 7U);
}

TEST_F(StoreRoundTripTest, aQuotationIntoASecondDocumentSurvivesSaving) {
  // What ctrl-t does: quote the selection into a second document. That second
  // document starts from the null document, so the state it produces is a
  // branch off the root -- a1 -- and writing a store containing one used to
  // produce an ops spool that could not be read back:
  //
  //   microversion "a1": expected a number at offset 0
  //
  // The quotation is the point of the program, so this is the whole store
  // becoming unreadable, not a corner of it.
  std::filesystem::create_directories(dir);

  MicroversionId quoted;
  {
    Store store;
    const auto one = store.insert(MicroversionId{}, 0, "Hello there");
    quoted         = store.transclude(MicroversionId{}, 0, one, 0, 5);
    ASSERT_EQ(quoted.str(), "a1");
    ASSERT_EQ(store.textOf(quoted), "Hello");
    store.save(dir.string());
  }

  Store reloaded;
  reloaded.load(dir.string());
  EXPECT_EQ(reloaded.opCount(), 2U);
  // The quotation still points at the same content, which is what a virtual
  // copy has to mean after a round trip.
  EXPECT_EQ(reloaded.textOf(quoted), "Hello");
  EXPECT_EQ(reloaded.textOf(MicroversionId::parse("1")), "Hello there");
}

} // namespace

TEST_F(StoreRoundTripTest, osmicTextFormatCanBeGeneratedOnDemand) {
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "Hello");
  const auto two = store.insert(one, 5, " World");
  const auto del = store.erase(two, 5, 6);

  const std::string text = store.exportOsmicText();
  EXPECT_FALSE(text.empty());
  EXPECT_NE(text.find("1 insert 0 0 0 0 5 0 0 0 0 0"), std::string::npos);
  EXPECT_NE(text.find("2 insert 5 0 0 5 6 0 0 0 0 0"), std::string::npos);
  EXPECT_NE(text.find("3 delete 5 6 0 0 0 0 0 0 0 0"), std::string::npos);
}

TEST_F(StoreRoundTripTest, saveOsmicTextSurvivesBeingLoadedBack) {
  std::filesystem::create_directories(dir);
  {
    Store store;
    const auto one = store.insert(MicroversionId{}, 0, "legacy osmic");
    store.insert(one, 12, " test");
    store.saveOsmicText(dir.string());
  }

  Store reloaded;
  reloaded.load(dir.string());
  EXPECT_EQ(reloaded.opCount(), 2U);
  EXPECT_EQ(reloaded.textOf(MicroversionId::parse("2")), "legacy osmic test");
}
