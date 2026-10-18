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
  ASSERT_TRUE(reader.cacheVerified(pkg, hash));
  EXPECT_TRUE(reader.links().empty());
  EXPECT_FALSE(reader.setEnabled(hash, true));
  ASSERT_TRUE(reader.setRetained(hash, true));
  EXPECT_FALSE(reader.enabled(hash));
  ASSERT_TRUE(reader.setEnabled(hash, true));
  const auto key = ReaderLinkPackages::keyOf(hash, 0);
  ASSERT_EQ(reader.links(), std::vector<LinkKey>{key});
  const auto next =
      publishLinkPackage(keys, pkg.salt, pkg.title, 2, 2, {link}, pkg.scrolls);
  const auto nextHash = carrier(next);
  ASSERT_TRUE(reader.cacheVerified(next, nextHash));
  EXPECT_FALSE(reader.enabled(nextHash));
  EXPECT_NE(key, ReaderLinkPackages::keyOf(nextHash, 0));
  ReaderLinkPackages restarted(path);
  EXPECT_TRUE(restarted.enabled(hash));
  EXPECT_TRUE(restarted.containsAuthority(key.authority));
  EXPECT_EQ(restarted.find(key), nullptr);
  EXPECT_TRUE(restarted.links().empty()); // Missing payload is retained as a
                                          // preference, never trusted.
  ASSERT_TRUE(restarted.cacheVerified(pkg, hash));
  ASSERT_EQ(restarted.links(), std::vector<LinkKey>{key});
  ASSERT_TRUE(restarted.setEnabled(hash, false));
  EXPECT_EQ(restarted.find(key), nullptr);
  ASSERT_NE(restarted.verifiedPackage(key), nullptr);
  EXPECT_EQ(restarted.verifiedPackage(key)->links, pkg.links);
  EXPECT_FALSE(restarted.enabled(key));
  ASSERT_TRUE(restarted.setEnabled(key, true));
  EXPECT_TRUE(restarted.enabled(key));
  EXPECT_EQ(restarted.find(key),
            &restarted.verifiedPackage(key)->links.front());
  ASSERT_TRUE(restarted.setEnabled(key, false));
  EXPECT_FALSE(ReaderLinkPackages(path).enabled(hash));
  EXPECT_FALSE(reader.cacheVerified(pkg, nextHash));
}
TEST_F(ReaderLinkPackagesTest,
       RetentionEnablementAndEmphasisAreIndependentImmutableChoices) {
  const auto path = root / "independent";
  ReaderLinkPackages reader(path);
  ASSERT_TRUE(reader.cacheVerified(pkg, hash));
  EXPECT_TRUE(reader.preferences().empty());
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_FALSE(reader.setEnabled(hash, true));
  EXPECT_FALSE(reader.setEmphasis(hash, PackageEmphasis::Quiet));
  ASSERT_TRUE(reader.setRetained(hash, true));
  ASSERT_TRUE(reader.setEmphasis(hash, PackageEmphasis::Quiet));
  EXPECT_EQ(reader.preference(hash),
            (ReaderPackagePreference{true, false, PackageEmphasis::Quiet}));
  ASSERT_TRUE(reader.setEnabled(hash, true));
  const auto key = ReaderLinkPackages::keyOf(hash, 0);
  EXPECT_EQ(reader.emphasis(key), PackageEmphasis::Quiet);
  EXPECT_EQ(reader.links(), std::vector<LinkKey>{key});
  const auto payload = encodeLinkPackage(*reader.verifiedPackage(key));
  const auto next =
      publishLinkPackage(keys, pkg.salt, pkg.title, 2, 2, {link}, pkg.scrolls);
  const auto nextHash = carrier(next);
  ASSERT_TRUE(reader.cacheVerified(next, nextHash));
  EXPECT_EQ(reader.preference(nextHash), ReaderPackagePreference{});
  EXPECT_FALSE(reader.preferences().contains(nextHash));
  ASSERT_TRUE(reader.setEnabled(hash, false));
  EXPECT_EQ(reader.preference(hash),
            (ReaderPackagePreference{true, false, PackageEmphasis::Quiet}));
  ASSERT_TRUE(reader.setRetained(hash, false));
  EXPECT_EQ(reader.preference(hash),
            (ReaderPackagePreference{false, false, PackageEmphasis::Quiet}));
  EXPECT_EQ(encodeLinkPackage(*reader.verifiedPackage(key)), payload);
  EXPECT_FALSE(reader.setEnabled(hash, true));
  ReaderLinkPackages reopened(path);
  EXPECT_EQ(reopened.preference(hash), reader.preference(hash));
  EXPECT_TRUE(reopened.containsAuthority(key.authority));
  EXPECT_EQ(reopened.verifiedPackage(key), nullptr);
  EXPECT_FALSE(reopened.setRetained(hash, true));
  EXPECT_FALSE(reopened.setEnabled(hash, true));
  ASSERT_TRUE(reopened.cacheVerified(pkg, hash));
  ASSERT_TRUE(reopened.setRetained(hash, true));
  EXPECT_EQ(reopened.preference(hash),
            (ReaderPackagePreference{true, false, PackageEmphasis::Quiet}));
  ASSERT_TRUE(reopened.setEnabled(hash, true));
  ASSERT_TRUE(reopened.setEmphasis(hash, PackageEmphasis::Normal));
  EXPECT_TRUE(reopened.enabled(hash));
  EXPECT_EQ(encodeLinkPackage(*reopened.verifiedPackage(key)), payload);
}
TEST_F(ReaderLinkPackagesTest,
       PreviousBooleanFormatAndImpossibleChoicesArePreserved) {
  std::filesystem::create_directories(root);
  const auto path = root / "invalid";
  using V         = bencode::Value;
  for (const auto &bytes :
       {V::dict({{"format", V::integer(1)},
                 {"choices", V::dict({{hash.hex(), V::integer(1)}})}})
            .encode(),
        V::dict({{"format", V::integer(2)},
                 {"choices",
                  V::dict({{hash.hex(),
                            V::dict({{"retained", V::integer(0)},
                                     {"enabled", V::integer(1)},
                                     {"emphasis", V::string("quiet")}})}})}})
            .encode(),
        V::dict({{"format", V::integer(2)},
                 {"choices",
                  V::dict({{hash.hex(),
                            V::dict({{"retained", V::integer(1)},
                                     {"enabled", V::integer(0)},
                                     {"emphasis", V::string("loud")}})}})}})
            .encode()}) {
    std::ofstream(path, std::ios::binary) << bytes;
    EXPECT_THROW(ReaderLinkPackages{path}, ReaderLinkPackagesUnreadable);
    std::ifstream in(path, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}), bytes);
  }
}
TEST_F(ReaderLinkPackagesTest,
       ExplicitEditionSwitchPreservesBothChoicesAndUnrelatedLayers) {
  const auto path = root / "switch";
  ReaderLinkPackages reader(path);
  const auto next =
      publishLinkPackage(keys, pkg.salt, pkg.title, 2, 2, {link}, pkg.scrolls);
  const auto nextHash  = carrier(next);
  const auto unrelated = publishLinkPackage(keys, "curations:other", pkg.title,
                                            1, 1, {link}, pkg.scrolls);
  const auto otherHash = carrier(unrelated);
  for (const auto &[package, hash] :
       std::vector<std::pair<LinkPackage, InfoHash>>{
           {pkg, this->hash}, {next, nextHash}, {unrelated, otherHash}})
    ASSERT_TRUE(reader.cacheVerified(package, hash));
  ASSERT_TRUE(reader.setRetained(hash, true));
  ASSERT_TRUE(reader.setEnabled(hash, true));
  ASSERT_TRUE(reader.setEmphasis(hash, PackageEmphasis::Quiet));
  ASSERT_TRUE(reader.setRetained(otherHash, true));
  ASSERT_TRUE(reader.setEnabled(otherHash, true));
  const auto oldChoice   = reader.preference(hash),
             otherChoice = reader.preference(otherHash);
  EXPECT_EQ(reader.editions(nextHash), std::vector<InfoHash>{hash});
  EXPECT_FALSE(reader.switchEnabled(hash, nextHash));
  EXPECT_EQ(reader.preference(hash), oldChoice);
  ASSERT_TRUE(reader.setRetained(nextHash, true));
  const auto nextChoice = reader.preference(nextHash);
  ASSERT_TRUE(reader.switchEnabled(hash, nextHash));
  EXPECT_EQ(reader.preference(hash),
            (ReaderPackagePreference{true, false, PackageEmphasis::Quiet}));
  EXPECT_EQ(reader.preference(nextHash),
            (ReaderPackagePreference{true, true, PackageEmphasis::Normal}));
  EXPECT_EQ(reader.preference(otherHash), otherChoice);
  EXPECT_EQ(reader.verifiedPackage(ReaderLinkPackages::keyOf(hash, 0))->links,
            pkg.links);
  ReaderLinkPackages reopened(path);
  EXPECT_EQ(reopened.preference(hash), reader.preference(hash));
  EXPECT_EQ(reopened.preference(nextHash), reader.preference(nextHash));
  EXPECT_FALSE(reopened.switchEnabled(nextHash, hash));
  ASSERT_TRUE(reader.switchEnabled(nextHash, hash));
  EXPECT_EQ(reader.preference(hash), oldChoice);
  EXPECT_EQ(reader.preference(nextHash), nextChoice);
  EXPECT_EQ(reader.preference(otherHash), otherChoice);
  EXPECT_FALSE(reader.switchEnabled(hash, otherHash));
  EXPECT_FALSE(reader.switchEnabled(hash, hash));
  const auto conflict = publishLinkPackage(keys, pkg.salt, "Conflicting title",
                                           1, 1, {link}, pkg.scrolls);
  const auto conflictHash = carrier(conflict);
  ASSERT_TRUE(reader.cacheVerified(conflict, conflictHash));
  ASSERT_TRUE(reader.setRetained(conflictHash, true));
  EXPECT_FALSE(reader.switchEnabled(hash, conflictHash));
  EXPECT_EQ(reader.preference(hash), oldChoice);
}
TEST_F(ReaderLinkPackagesTest,
       FailedEditionSwitchLeavesBothDurableChoicesUntouched) {
  const auto path = root / "atomic";
  ReaderLinkPackages reader(path);
  const auto next =
      publishLinkPackage(keys, pkg.salt, pkg.title, 2, 2, {link}, pkg.scrolls);
  const auto nextHash = carrier(next);
  ASSERT_TRUE(reader.cacheVerified(pkg, hash));
  ASSERT_TRUE(reader.cacheVerified(next, nextHash));
  ASSERT_TRUE(reader.setRetained(hash, true));
  ASSERT_TRUE(reader.setRetained(nextHash, true));
  ASSERT_TRUE(reader.setEnabled(hash, true));
  const auto oldChoice  = reader.preference(hash),
             nextChoice = reader.preference(nextHash);
  std::ifstream before(path, std::ios::binary);
  const std::string original{std::istreambuf_iterator<char>(before), {}};
  const auto untouched = root / "untouched";
  std::ofstream(untouched) << "unchanged";
  std::filesystem::create_symlink(untouched, path.string() + ".partial");
  EXPECT_FALSE(reader.switchEnabled(hash, nextHash));
  EXPECT_EQ(reader.preference(hash), oldChoice);
  EXPECT_EQ(reader.preference(nextHash), nextChoice);
  std::ifstream after(path, std::ios::binary);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(after), {}), original);
  std::ifstream sentinel(untouched);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(sentinel), {}),
            "unchanged");
}
TEST_F(ReaderLinkPackagesTest, NativeLinksResolveAcrossIndependentScrollSlots) {
  auto perma = std::make_shared<UserPermascroll>();
  Store owner(perma), quoted(perma);
  const auto slot      = owner.addScroll(scroll);
  const auto unrelated = Scroll::ofTorrentFile(
      makeTorrent(
          std::vector<TorrentContent>{{.path = "other", .data = "unrelated"}},
          "other")
          .hash,
      0, "other", 0, 9);
  const auto other    = quoted.addScroll(unrelated);
  const auto imported = quoted.addScroll(scroll);
  Link native;
  native.owner = "native author";
  native.left  = {{slot, 0, 3}, {slot, 6, 3}};
  native.right = {{slot, 10, 3}};
  owner.addLink({}, native);
  ASSERT_EQ(owner.links().size(), 1);
  const auto id = owner.links().begin()->first;
  Version original, quotation;
  original.insertSpans(0, {{slot, 0, 3}, {slot, 6, 3}, {slot, 10, 3}});
  quotation.insertSpans(0, {{other, 0, 3}, {imported, 0, 3}, {imported, 0, 3}});
  const std::vector<PackageDocumentView> docs{{owner, {}, original},
                                              {quoted, {}, quotation}};
  const auto count       = owner.opCount();
  const auto deployments = quoted.scrolls().size();
  const auto resolved    = PackageOccurrenceIndex(docs, {}).resolve(owner, id);
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->key, (LinkKey{owner.documentId(), id}));
  EXPECT_EQ(resolved->link.owner, native.owner);
  ASSERT_EQ(resolved->left.size(), 2);
  EXPECT_EQ(resolved->left[0].span, native.left[0]);
  EXPECT_EQ(resolved->left[0].occurrences.size(), 3);
  EXPECT_EQ(resolved->left[1].occurrences.size(), 1);
  ASSERT_EQ(resolved->right.size(), 1);
  EXPECT_EQ(resolved->right[0].occurrences.size(), 1);
  EXPECT_EQ(owner.opCount(), count);
  EXPECT_EQ(quoted.scrolls().size(), deployments);
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
