#include "common/xanadu/bencode.hpp"
#include "common/xanadu/link_navigation.hpp"
#include "common/xanadu/reader_link_packages.hpp"
#include "common/xanadu/store.hpp"
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>

namespace {
using namespace xanadu;
class ReaderLinkPackagesTest : public testing::Test {
protected:
  MutableKeys keys = createMutableKeys();
  std::filesystem::path root =
      std::filesystem::temp_directory_path() / keys.publicKey.hex();
  Scroll scroll = Scroll::ofTorrentFile(
      makeTorrent(
          std::vector<TorrentContent>{
              {.path = "text", .data = "012345678901234567890123456789"}},
          "text")
          .hash,
      0, "text", 0, 30);
  std::string global = scrollKey(scroll);
  GlobalLink link{.owner = "Author",
                  .left  = {{global, 0, 3}, {global, 6, 3}},
                  .right = {{global, 10, 3}, {global, 16, 3}, {global, 24, 3}}};
  LinkPackage pkg = publishLinkPackage(keys, "curations:ideas", "Devin's links",
                                       1, 1, {link}, {{global, scroll}});
  InfoHash hash   = carrier(pkg);
  static InfoHash carrier(const LinkPackage &p) {
    return makeTorrent(
               std::vector<TorrentContent>{
                   {.path = "links.xanalinks", .data = encodeLinkPackage(p)}},
               "link-package")
        .hash;
  }
  ~ReaderLinkPackagesTest() override { std::filesystem::remove_all(root); }
};
TEST_F(ReaderLinkPackagesTest,
       ExactImmutableChoicesSurviveRestartWithoutEnablingUpdates) {
  const auto path = root / "preferences";
  ReaderLinkPackages reader(path);
  ASSERT_TRUE(reader.retain(pkg, hash));
  EXPECT_TRUE(reader.links().empty());
  ASSERT_TRUE(reader.setEnabled(hash, true));
  const auto key = ReaderLinkPackages::keyOf(hash, 0);
  ASSERT_EQ(reader.links(), std::vector<LinkKey>{key});
  const auto next =
      publishLinkPackage(keys, pkg.salt, pkg.title, 2, 2, {link}, pkg.scrolls);
  const auto nextHash = carrier(next);
  ASSERT_TRUE(reader.retain(next, nextHash));
  EXPECT_FALSE(reader.enabled(nextHash));
  EXPECT_NE(key, ReaderLinkPackages::keyOf(nextHash, 0));
  ReaderLinkPackages restarted(path);
  EXPECT_TRUE(restarted.enabled(hash));
  EXPECT_TRUE(restarted.containsAuthority(key.authority));
  EXPECT_EQ(restarted.find(key), nullptr);
  EXPECT_TRUE(restarted.links().empty()); // Missing payload is retained as a
                                          // preference, never trusted.
  ASSERT_TRUE(restarted.retain(pkg, hash));
  ASSERT_EQ(restarted.links(), std::vector<LinkKey>{key});
  ASSERT_TRUE(restarted.setEnabled(hash, false));
  EXPECT_EQ(restarted.find(key), nullptr);
  EXPECT_FALSE(ReaderLinkPackages(path).enabled(hash));
  EXPECT_FALSE(reader.retain(pkg, nextHash));
}
TEST_F(ReaderLinkPackagesTest,
       MalformedPreferencesArePreservedAndRefusedByVersion) {
  std::filesystem::create_directories(root);
  const auto file = root / "choices";
  const auto bytes =
      bencode::Value::dict({{"format", bencode::Value::integer(99)},
                            {"choices", bencode::Value::dict({})}})
          .encode();
  std::ofstream(file) << bytes;
  EXPECT_THROW(ReaderLinkPackages{file}, ReaderLinkPackagesUnreadable);
  std::ifstream input(file);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), bytes);
}
TEST_F(
    ReaderLinkPackagesTest,
    GlobalMatchingKeepsSlotsEndsetsRepeatedQuotesAndUnresolvedMembersDistinct) {
  auto perma = std::make_shared<UserPermascroll>();
  Store a(perma), b(perma);
  const auto slot = a.addScroll(scroll);
  auto other      = Scroll::ofTorrentFile(
      makeTorrent(std::vector<TorrentContent>{{.path = "text",
                                                    .data = "unrelated bytes"}},
                       "other")
          .hash,
      0, "text", 0, 15);
  b.addScroll(other);
  const auto remote = b.addScroll(scroll);
  Version textA, textB;
  textA.insertSpans(0,
                    {{slot, 0, 3}, {slot, 0, 3}, {slot, 6, 3}, {slot, 10, 3}});
  textB.insertSpans(0, {{1, 0, 3}, {remote, 0, 3}, {remote, 16, 3}});
  const auto oldA = a.opCount(), oldB = b.opCount();
  const auto oldScrollsA = a.scrolls().size(), oldScrollsB = b.scrolls().size();
  const std::vector<PackageDocumentView> docs{{a, {}, textA}, {b, {}, textB}};
  const auto resolved =
      resolvePackageLink(ReaderLinkPackages::keyOf(hash, 0), pkg, docs, {});
  ASSERT_EQ(resolved.left.size(), 2U);
  ASSERT_EQ(resolved.right.size(), 3U);
  ASSERT_EQ(resolved.left[0].occurrences.size(), 3U);
  EXPECT_EQ(std::get<DocumentSite>(resolved.left[0].occurrences[0].site).range,
            (Extent{0, 3}));
  EXPECT_EQ(std::get<DocumentSite>(resolved.left[0].occurrences[1].site).range,
            (Extent{3, 6}));
  EXPECT_EQ(std::get<DocumentSite>(resolved.left[0].occurrences[2].site).range,
            (Extent{3, 6}));
  EXPECT_TRUE(resolved.right[2].occurrences.empty());
  EXPECT_EQ(a.opCount(), oldA);
  EXPECT_EQ(b.opCount(), oldB);
  EXPECT_EQ(a.scrolls().size(), oldScrollsA);
  EXPECT_EQ(b.scrolls().size(), oldScrollsB);
  b.bindPublishedLocalScroll(remote);
  EXPECT_EQ(packageOccurrences(b, std::vector<PrimediaSpan>{{0, 0, 3}},
                               {global, 0, 3})
                .size(),
            1U);
}
TEST_F(ReaderLinkPackagesTest,
       PublicationFilterUsesBytesAndKeepsBothCompleteEndsets) {
  Publication pub;
  pub.pieces = {{global, 28, 2}};
  EXPECT_FALSE(packageReferences(pkg, pub));
  pub.pieces = {{global, 8, 1}};
  EXPECT_TRUE(packageReferences(pkg, pub));
  pub.pieces = {{"different", 0, 30}};
  EXPECT_FALSE(packageReferences(pkg, pub));
  EXPECT_EQ(pkg.links[0].right.size(), 3U);
}
TEST_F(ReaderLinkPackagesTest,
       NavigatorUsesTheSameIndependentCursorsAndRefusesUnavailableEntry) {
  auto perma = std::make_shared<UserPermascroll>();
  Store store(perma);
  const auto slot = store.addScroll(scroll);
  Version text;
  text.insertSpans(0, {{slot, 0, 30}});
  const auto key = ReaderLinkPackages::keyOf(hash, 0);
  const std::vector<PackageDocumentView> docs{{store, {}, text}};
  InMemoryActivityLog activity;
  LinkNavigator navigator(activity);
  auto selected = navigator.dispatch(nav::SelectLink{.key = key});
  ASSERT_TRUE(selected);
  ASSERT_TRUE(navigator.supply(selected->resolve->generation,
                               resolvePackageLink(key, pkg, docs, {})));
  ASSERT_TRUE(navigator.dispatch(
      nav::SelectMember{.key = key, .side = LinkSide::Left, .member = 1}));
  ASSERT_TRUE(navigator.dispatch(
      nav::SelectMember{.key = key, .side = LinkSide::Right, .member = 2}));
  EXPECT_EQ(navigator.selection()->cursor(LinkSide::Left).member, 1U);
  EXPECT_EQ(navigator.selection()->cursor(LinkSide::Right).member, 2U);
  ASSERT_TRUE(navigator.dispatch(nav::Enter{}));
  const auto current = navigator.currentVisit();
  ASSERT_TRUE(current);
  EXPECT_EQ(activity.find(*current)->link->key, key);
  ASSERT_TRUE(navigator.dispatch(nav::Dismiss{}));
  EXPECT_FALSE(navigator.selection());
  EXPECT_TRUE(activity.find(*current));
  selected = navigator.dispatch(nav::SelectLink{.key = key});
  ASSERT_TRUE(selected);
  ASSERT_TRUE(navigator.supply(selected->resolve->generation,
                               resolvePackageLink(key, pkg, {}, {})));
  EXPECT_FALSE(navigator.dispatch(nav::Enter{}));
  EXPECT_EQ(navigator.currentVisit(), current);
}
TEST_F(ReaderLinkPackagesTest,
       IndexedResolutionMeasuresLargeDiscontinuousEndsets) {
  const auto seal = makeTorrent(
      std::vector<TorrentContent>{
          {.path = "large", .data = std::string(100000, 'x')}},
      "large");
  const auto scroll = Scroll::ofTorrentFile(seal.hash, 0, "large", 0, 100000);
  const auto global = scrollKey(scroll);
  Store store(std::make_shared<UserPermascroll>());
  const auto slot = store.addScroll(scroll);
  std::vector<PrimediaSpan> pieces;
  for (std::uint64_t i = 0; i < 50000; ++i) pieces.push_back({slot, i * 2, 1});
  Version text;
  text.insertSpans(0, pieces);
  GlobalLink link;
  link.owner = "fanout";
  for (std::uint64_t i = 0; i < 1000; ++i) {
    link.left.push_back({global, i * 2, 1});
    link.right.push_back({global, 50000 + i * 2, 1});
  }
  const auto pkg = publishLinkPackage(keys, "curations:large", "Large", 1, 1,
                                      {link}, {{global, scroll}});
  const std::vector<PackageDocumentView> docs{{store, {}, text}};
  const auto began = std::chrono::steady_clock::now();
  const PackageOccurrenceIndex index(docs, {});
  const auto built = std::chrono::steady_clock::now();
  const auto result =
      index.resolve(ReaderLinkPackages::keyOf(carrier(pkg), 0), pkg);
  const auto ended = std::chrono::steady_clock::now();
  RecordProperty("pieces", 50000);
  RecordProperty("members", 2000);
  RecordProperty(
      "build_us",
      std::chrono::duration_cast<std::chrono::microseconds>(built - began)
          .count());
  RecordProperty(
      "resolve_us",
      std::chrono::duration_cast<std::chrono::microseconds>(ended - built)
          .count());
  ASSERT_EQ(result.left.size(), 1000U);
  ASSERT_EQ(result.right.size(), 1000U);
  for (const auto *side : {&result.left, &result.right})
    for (const auto &member : *side) ASSERT_EQ(member.occurrences.size(), 1U);
}
} // namespace
