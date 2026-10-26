#include "common/ui/xanadoc/session.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace {

using namespace xanadu;

constexpr std::string_view svg =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"12\">"
    "<rect width=\"24\" height=\"12\" fill=\"black\"/></svg>";

class SessionMediaCacheTest : public testing::Test {
protected:
  std::shared_ptr<UserPermascroll> perma = std::make_shared<UserPermascroll>();
  Session session{"", perma};
};

TEST_F(SessionMediaCacheTest, RepeatedPlainQueriesStayEmptyAcrossViewChanges) {
  auto &store        = session.store();
  const auto version = store.insert({}, 0, "Plain text with no media.\n");
  const auto ops     = store.opCount();
  const auto bytes   = perma->size();
  for (int repeat = 0; repeat < 100; ++repeat)
    EXPECT_TRUE(session.mediaSpansFor(version).empty());
  const auto generation = session.generation();
  session.viewOpened(version);
  EXPECT_GT(session.generation(), generation);
  for (int repeat = 0; repeat < 100; ++repeat)
    EXPECT_TRUE(session.mediaSpansFor(version).empty());
  session.clearViews();
  EXPECT_TRUE(session.mediaSpansFor(version).empty());
  EXPECT_EQ(store.opCount(), ops);
  EXPECT_EQ(perma->size(), bytes);
}

TEST_F(SessionMediaCacheTest,
       ExplicitRootFocusSurvivesAlongsideAnActiveXanadoc) {
  auto &store = session.store();
  const auto rootText =
      store.insert(MicroversionId{}, 0, "root bytes", MicroversionId{});
  const auto birthVersion  = store.makeXanadoc(rootText, "active document");
  const auto activeVersion = store.insert(birthVersion, 0, "active bytes");
  const auto birth         = store.activeXanadocOnBranch(activeVersion);
  ASSERT_TRUE(birth);

  session.viewOpened(activeVersion);
  ASSERT_TRUE(session.focusTargetForView(0));
  ASSERT_FALSE(session.focusTargetForView(0)->isRootXanadoc());
  EXPECT_EQ(session.sourceFor(activeVersion)->text(), "active bytes");
  EXPECT_EQ(
      session.sourceFor(activeVersion, 0, XanadocSelection::root())->text(),
      "root bytes");

  const auto operationsBeforeFocus = store.opCount();
  session.setFocusTarget(0, FocusTarget::rootXanadoc());
  ASSERT_TRUE(session.focusTargetForView(0));
  EXPECT_TRUE(session.focusTargetForView(0)->isRootXanadoc());
  EXPECT_EQ(session.views()[0].pieces.materialize(store), "root bytes");
  EXPECT_EQ(store.opCount(), operationsBeforeFocus);

  const auto edited = session.insertText(0, 10, " edited");
  ASSERT_NE(edited, MicroversionId{});
  ASSERT_TRUE(store.getOp(edited));
  EXPECT_EQ(store.getOp(edited)->context, MicroversionId{});
  EXPECT_EQ(session.views()[0].pieces.materialize(store), "root bytes edited");
  EXPECT_EQ(store.textOf(edited, XanadocSelection::birth(*birth)),
            "active bytes");
  const auto rootDiff =
      store.diffVersions(activeVersion, edited, XanadocSelection::root());
  ASSERT_EQ(rootDiff.versions.size(), 2U);
  EXPECT_EQ(rootDiff.versions[0].text, "root bytes");
  EXPECT_EQ(rootDiff.versions[1].text, "root bytes edited");
  ASSERT_TRUE(session.focusTargetForView(0));
  EXPECT_TRUE(session.focusTargetForView(0)->isRootXanadoc());

  const auto operationsBeforeAutomatic = store.opCount();
  session.setFocusTarget(0, std::nullopt);
  ASSERT_TRUE(session.focusTargetForView(0));
  EXPECT_FALSE(session.focusTargetForView(0)->isRootXanadoc());
  EXPECT_EQ(session.views()[0].pieces.materialize(store), "active bytes");
  EXPECT_EQ(store.opCount(), operationsBeforeAutomatic);
}

TEST_F(SessionMediaCacheTest, LocalMediaOffsetsStaySpecificToEachVersion) {
  auto &store              = session.store();
  const std::string prefix = "Introduction\n";
  const auto text          = store.insert({}, 0, prefix);
  const auto media         = store.insertMedia(
      text, static_cast<std::uint32_t>(prefix.size()), svg, "image/svg+xml");
  const auto first = session.mediaSpansFor(media.version);
  ASSERT_EQ(first.size(), 1U);
  EXPECT_EQ(first[0].docOffset, prefix.size());
  EXPECT_EQ(first[0].span, media.span);
  EXPECT_EQ(first[0].containerOffset, 0U);
  EXPECT_EQ(first[0].containerLength, svg.size());
  EXPECT_EQ(first[0].mime, "image/svg+xml");
  EXPECT_TRUE(first[0].isImage);
  EXPECT_FALSE(first[0].isAnimation);
  const auto source = session.sourceFor(media.version);
  ASSERT_EQ(source->layoutBoxes().size(), 1U);
  EXPECT_EQ(source->layoutBoxes()[0].anchor, first[0].docOffset);

  // Append directly to Store: Session's epoch does not change. Both the
  // historical version and new version must retain their own offsets.
  const auto generation      = session.generation();
  const std::string inserted = "Earlier ";
  const auto edited          = store.insert(media.version, 0, inserted);
  EXPECT_EQ(session.generation(), generation);
  const auto second = session.mediaSpansFor(edited);
  ASSERT_EQ(second.size(), 1U);
  EXPECT_EQ(second[0].docOffset, prefix.size() + inserted.size());
  EXPECT_EQ(second[0].span, first[0].span);
  const auto historical = session.mediaSpansFor(media.version);
  ASSERT_EQ(historical.size(), 1U);
  EXPECT_EQ(historical[0].docOffset, prefix.size());
  EXPECT_TRUE(session.mediaSpansFor(text).empty());
}

TEST_F(SessionMediaCacheTest, ReturnedMediaIsAnOwningSnapshot) {
  auto &store = session.store();
  // Unsegmented local media exercises cache admission, rather than the
  // conservative bypass used for authored container metadata.
  const auto version = store.insert({}, 0, svg);
  auto snapshot      = session.mediaSpansFor(version);
  ASSERT_EQ(snapshot.size(), 1U);
  const auto originalLabel = snapshot[0].label;
  const auto originalSpan  = snapshot[0].span;
  snapshot[0].label        = "Caller annotation";
  snapshot[0].docOffset    = 900;
  const auto queried       = session.mediaSpansFor(version);
  ASSERT_EQ(queried.size(), 1U);
  EXPECT_EQ(queried[0].label, originalLabel);
  EXPECT_EQ(queried[0].docOffset, 0U);

  session.viewOpened(version);
  const auto later = store.insert(version, 0, "New heading\n");
  const auto moved = session.mediaSpansFor(later);
  ASSERT_EQ(moved.size(), 1U);
  EXPECT_EQ(moved[0].docOffset, std::string_view("New heading\n").size());
  EXPECT_EQ(snapshot[0].label, "Caller annotation");
  EXPECT_EQ(snapshot[0].docOffset, 900U);
  EXPECT_EQ(snapshot[0].span, originalSpan);
}

TEST_F(SessionMediaCacheTest, SameVersionNamesInDifferentStoresStayIsolated) {
  const auto plain = session.store().insert({}, 0, "Text only");
  auto other       = std::make_unique<Store>(perma);
  const auto media = other->insert({}, 0, svg);
  ASSERT_EQ(plain, media);
  const auto otherIndex = session.addStore(std::move(other), "");
  for (int repeat = 0; repeat < 10; ++repeat) {
    EXPECT_TRUE(session.mediaSpansFor(plain, 0).empty());
    const auto spans = session.mediaSpansFor(media, otherIndex);
    ASSERT_EQ(spans.size(), 1U);
    EXPECT_TRUE(spans[0].isImage);
    EXPECT_EQ(spans[0].docOffset, 0U);
  }
}

TEST_F(SessionMediaCacheTest, MissingExternalBytesBecomeMediaWithoutAnEdit) {
  auto &store = session.store();
  Scroll remote;
  remote.publisher = createMutableKeys().publicKey;
  remote.salt      = "live-media-test";
  const PrimediaSpan span{
      .scroll = store.addScroll(remote), .start = 0, .length = svg.size()};
  const auto version    = store.insertSpan({}, 0, span);
  const auto ops        = store.opCount();
  const auto generation = session.generation();
  EXPECT_TRUE(store.read(span).empty());
  EXPECT_TRUE(session.mediaSpansFor(version).empty());
  ASSERT_TRUE(remote.publisher);
  store.setExternalLiveBytes(
      "btpk:" + remote.publisher->hex() + ":" + remote.salt, 0, svg);
  ASSERT_EQ(store.read(span), svg);
  EXPECT_EQ(store.opCount(), ops);
  EXPECT_EQ(session.generation(), generation);
  const auto available = session.mediaSpansFor(version);
  ASSERT_EQ(available.size(), 1U);
  EXPECT_TRUE(available[0].isImage);
  EXPECT_EQ(available[0].span, span);
  EXPECT_EQ(available[0].docOffset, 0U);
  EXPECT_EQ(available[0].containerLength, svg.size());
}

TEST_F(SessionMediaCacheTest,
       NewContainerMetadataInvalidatesPlainClassification) {
  auto &store        = session.store();
  const auto version = store.insert({}, 0, "authored media descriptor");
  const auto spans   = store.rebuild(version).pieces();
  ASSERT_EQ(spans.size(), 1U);
  const auto span = spans.front();
  EXPECT_TRUE(session.mediaSpansFor(version).empty());
  const auto ops        = store.opCount();
  const auto generation = session.generation();
  // Classification honours the author's container MIME. No decode is
  // requested here; this regression tests metadata appearing at the same
  // immutable address after an earlier unsegmented classification.
  store.addSegment(
      localScroll,
      {.at = span.start, .length = span.length, .mimeType = "image/png"});
  EXPECT_EQ(store.opCount(), ops);
  EXPECT_EQ(session.generation(), generation);
  const auto annotated = session.mediaSpansFor(version);
  ASSERT_EQ(annotated.size(), 1U);
  EXPECT_TRUE(annotated[0].isImage);
  EXPECT_EQ(annotated[0].mime, "image/png");
  EXPECT_EQ(annotated[0].span, span);
}

TEST_F(SessionMediaCacheTest, BootstrapLocalBytesAreNotCachedAsMissing) {
  auto &store = session.store();
  std::string available;
  std::size_t reads = 0;
  store.setBootstrapPermascroll("bootstrap-test", [&](const PrimediaSpan &) {
    ++reads;
    return ResolveResult{.status = available.empty()
                                       ? ResolutionStatus::MissingPieces
                                       : ResolutionStatus::VerifiedBytes,
                         .text   = available};
  });
  const PrimediaSpan span{
      .scroll = localScroll, .start = 0, .length = svg.size()};
  const auto version    = store.insertSpan({}, 0, span);
  const auto ops        = store.opCount();
  const auto generation = session.generation();
  ASSERT_EQ(perma->size(), 0U);
  EXPECT_TRUE(session.mediaSpansFor(version).empty());
  available        = svg;
  const auto fresh = session.mediaSpansFor(version);
  ASSERT_EQ(fresh.size(), 1U);
  EXPECT_TRUE(fresh[0].isImage);
  EXPECT_EQ(store.opCount(), ops);
  EXPECT_EQ(session.generation(), generation);
  EXPECT_EQ(perma->size(), 0U);
  const auto readsBeforeClear = reads;
  store.setBootstrapPermascroll(std::nullopt);
  EXPECT_EQ(store.read(span), "");
  EXPECT_EQ(reads, readsBeforeClear);
  EXPECT_FALSE(store.bootstrapPermascrollKey());
  EXPECT_THROW(store.setBootstrapPermascroll(std::string{}, {}),
               std::invalid_argument);
}

} // namespace
