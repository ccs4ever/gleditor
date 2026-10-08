#include "../lib/mocks/world_device.hpp"
#include "common/ui/xanadoc/beams.hpp"
#include "common/ui/xanadoc/link_context.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <gleditor/beams.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text_source.hpp>
#include <gtest/gtest.h>

namespace {
using namespace xanadu;
class ReaderPackageUiTest : public testing::Test {
protected:
  MutableKeys keys           = createMutableKeys();
  std::filesystem::path root = std::filesystem::temp_directory_path() /
                               ("reader-package-ui-" + keys.publicKey.hex());
  std::map<std::string, std::optional<std::string>> environment;
  void SetUp() override {
    for (const auto *name : {"XDG_CONFIG_HOME", "XDG_DATA_HOME"}) {
      const auto *old   = std::getenv(name);
      environment[name] = old ? std::optional<std::string>{old} : std::nullopt;
      setenv(name, (root / name).c_str(), 1);
    }
  }
  void TearDown() override {
    for (const auto &[name, old] : environment)
      if (old)
        setenv(name.c_str(), old->c_str(), 1);
      else
        unsetenv(name.c_str());
    std::filesystem::remove_all(root);
  }
};
TEST_F(
    ReaderPackageUiTest,
    EnabledPackageUsesSharedNavigationAndAccessibleIdentityThenDismissesWithoutMutation) {
  auto perma = std::make_shared<UserPermascroll>();
  Session session((root / "workspace").string(), perma);
  auto &store        = session.store();
  const auto version = store.insert({}, 0, "012345678901234567890123456789");
  const auto torrent = makeTorrent(
      std::vector<TorrentContent>{
          {.path = "text", .data = "012345678901234567890123456789"}},
      "text");
  const auto scroll = Scroll::ofTorrentFile(torrent.hash, 0, "text", 0, 30);
  const auto global = scrollKey(scroll);
  store.bindPublishedLocalScroll(store.addScroll(scroll));
  session.views().push_back(
      {.version = version, .pieces = store.rebuild(version)});
  GlobalLink authored;
  authored.owner = "Author";
  authored.left  = {{global, 0, 3}, {global, 6, 3}};
  authored.right = {{global, 10, 3}, {global, 16, 3}, {global, 24, 3}};
  const auto pkg = publishLinkPackage(keys, "curations:ideas", "Devin's links",
                                      1, 1, {authored}, {{global, scroll}});
  const auto hash =
      makeTorrent(std::vector<TorrentContent>{{.path = "links.xanalinks",
                                               .data = encodeLinkPackage(pkg)}},
                  "link-package")
          .hash;
  LinkPackageStatus status{
      .phase = LinkPackagePhase::Ready, .package = pkg, .hash = hash};
  const auto oldOps = store.opCount(), oldScrolls = store.scrolls().size();
  const auto oldGeneration = session.generation();
  ASSERT_TRUE(session.setLinkPackageEnabled(status, true));
  EXPECT_GT(session.generation(), oldGeneration);
  const auto key      = ReaderLinkPackages::keyOf(hash, 0);
  const auto renderId = session.packageRenderId(key);
  EXPECT_GT(renderId, static_cast<std::uint64_t>(zigzag::noCell));
  LinkContext navigation(session);
  std::optional<Extent> focused;
  navigation.setFocusDocument([&](std::size_t view, Extent range) {
    EXPECT_EQ(view, 0U);
    focused = range;
  });
  WorldRecordingDevice device;
  RenderState state{&device};
  state.docs.push_back(Doc::create(
      {}, &device, glm::mat4{1},
      gleditor::MemoryTextSource({}, "012345678901234567890123456789")));
  LinkBeams beams(session, nullptr);
  beams.setLinkContext(&navigation);
  beams.deviceReady(device, {});
  ch::Timeline timeline;
  glm::mat4 projection{1};
  gleditor::FrameContext frame{.state          = state,
                               .viewProjection = projection,
                               .screenWidth    = 800,
                               .screenHeight   = 600,
                               .timeline       = timeline};
  beams.drawFrame(frame);
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 27);
  beams.describe(builder);
  EXPECT_TRUE(
      beams.performAction(renderId + 1, gleditor::a11y::Action::Click, {}));
  beams.drawFrame(frame);
  ASSERT_TRUE(navigation.selection());
  EXPECT_EQ(navigation.selection()->key, key);
  EXPECT_FALSE(focused);
  ASSERT_TRUE(navigation.execute(
      nav::SelectMember{.key = key, .side = LinkSide::Left, .member = 1}));
  ASSERT_TRUE(navigation.execute(
      nav::SelectMember{.key = key, .side = LinkSide::Right, .member = 2}));
  ASSERT_TRUE(navigation.execute(nav::Enter{}));
  EXPECT_EQ(focused, (Extent{24, 27}));
  const auto activityOps = session.activityForNavigation()->opCount();
  const auto view        = session.views().back();
  session.views().clear();
  EXPECT_FALSE(navigation.execute(nav::Enter{}));
  EXPECT_EQ(session.activityForNavigation()->opCount(), activityOps);
  EXPECT_EQ(focused, (Extent{24, 27}));
  session.views().push_back(view);
  // Queue the accessible action while enabled, then disable before delivery.
  ASSERT_TRUE(
      beams.performAction(renderId + 1, gleditor::a11y::Action::Click, {}));
  ASSERT_TRUE(session.setLinkPackageEnabled(status, false));
  navigation.packageVisibilityChanged();
  beams.drawFrame(frame);
  EXPECT_FALSE(navigation.selection());
  EXPECT_EQ(session.activityForNavigation()->opCount(), activityOps);
  EXPECT_THROW(session.addStore(std::make_unique<Store>(perma, key.authority),
                                (root / "collision").string()),
               std::invalid_argument);
  EXPECT_EQ(focused, (Extent{24, 27}));
  EXPECT_EQ(store.opCount(), oldOps);
  EXPECT_EQ(store.scrolls().size(), oldScrolls);
  EXPECT_TRUE(store.links().empty());
  EXPECT_FALSE(session.readerLinkPackages().enabled(hash));
  EXPECT_FALSE(navigation.resolveForPresentation(key));
}
TEST_F(ReaderPackageUiTest, RestoringPreferencesRefusesNativeAuthorityOverlap) {
  auto perma         = std::make_shared<UserPermascroll>();
  const auto torrent = makeTorrent(
      std::vector<TorrentContent>{{.path = "text", .data = "abc"}}, "text");
  const auto scroll = Scroll::ofTorrentFile(torrent.hash, 0, "text", 0, 3);
  const auto global = scrollKey(scroll);
  GlobalLink link{.left = {{global, 0, 1}}, .right = {{global, 1, 1}}};
  const auto pkg = publishLinkPackage(keys, "layers:collision", "Collision", 1,
                                      1, {link}, {{global, scroll}});
  const auto hash =
      makeTorrent(std::vector<TorrentContent>{{.path = "links.xanalinks",
                                               .data = encodeLinkPackage(pkg)}},
                  "link-package")
          .hash;
  {
    Session original((root / "workspace").string(), perma);
    original.linkPackageExchange().submit(pkg, false);
    ASSERT_TRUE(original.setLinkPackageEnabled(
        {.phase = LinkPackagePhase::Ready, .package = pkg, .hash = hash},
        true));
  }
  const auto preferences =
      xanadocsDirectory().parent_path() / "package-visibility";
  std::ifstream before(preferences);
  const std::string retained{std::istreambuf_iterator<char>(before), {}};
  Store collision(perma, ReaderLinkPackages::keyOf(hash, 0).authority);
  collision.save((root / "collision").string());
  Session restored((root / "collision").string(), perma);
  EXPECT_THROW(restored.readerLinkPackages(), ReaderLinkPackagesUnreadable);
  std::ifstream after(preferences);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(after), {}), retained);
}
TEST_F(ReaderPackageUiTest,
       MissingRepresentativeDoesNotHideVisibleLinkMembers) {
  auto perma = std::make_shared<UserPermascroll>();
  Session session((root / "workspace").string(), perma);
  auto &store       = session.store();
  const auto leftA  = store.makeCell({}, "left A");
  const auto leftB  = store.makeCell(leftA, "left B");
  const auto rightA = store.makeCell(leftB, "right A");
  const auto rightB = store.makeCell(rightA, "right B");
  const auto before = store.rebuildManifold(rightB);
  const auto a = store.cellRefOf(leftA), b = store.cellRefOf(leftB);
  const auto missing = store.cellRefOf(rightA),
             visible = store.cellRefOf(rightB);
  Link link;
  link.left       = {before.contentOf(a).front(), before.contentOf(b).front()};
  link.right      = {before.contentOf(missing).front(),
                     before.contentOf(visible).front()};
  const auto head = store.addLink(rightB, link);
  store.repointCurrentVersion(head);
  const auto manifold   = store.rebuildManifold(head);
  const auto operations = store.opCount();
  WorldRecordingDevice device;
  std::vector<gleditor::Beams::Row> rows;
  ON_CALL(device, drawGlyphs)
      .WillByDefault([&](const render::DrawUniforms &,
                         render::BufferHandle buffer, std::size_t offset,
                         std::uint32_t count) {
        const auto &bytes = device.buffers.at(buffer.id);
        ASSERT_LE(offset + count * sizeof(gleditor::Beams::Row), bytes.size());
        for (std::uint32_t i = 0; i < count; ++i) {
          gleditor::Beams::Row row;
          std::memcpy(&row, bytes.data() + offset + i * sizeof(row),
                      sizeof(row));
          rows.push_back(row);
        }
      });
  RenderState state{&device};
  LinkBeams beams(session, nullptr);
  beams.setManifoldViews({&manifold});
  beams.setCellRadius(-1);
  beams.setCellAnchorResolver([&](zigzag::CellRef cell)
                                  -> std::optional<CellAnchor> {
    if (cell == a)
      return CellAnchor{.position = {-100, -20, 0}, .width = 20, .height = 14};
    if (cell == b)
      return CellAnchor{.position = {-100, 20, 0}, .width = 20, .height = 14};
    if (cell == visible)
      return CellAnchor{.position = {100, 0, 0}, .width = 20, .height = 14};
    return std::nullopt;
  });
  beams.deviceReady(device, {});
  ch::Timeline timeline;
  glm::mat4 projection{1};
  gleditor::FrameContext frame{.state          = state,
                               .viewProjection = projection,
                               .screenWidth    = 800,
                               .screenHeight   = 600,
                               .timeline       = timeline};
  beams.drawFrame(frame);
  ASSERT_FALSE(rows.empty());
  const auto touches = [&](float x, float y) {
    return std::ranges::any_of(rows, [&](const auto &row) {
      return (std::abs(row.from[0] - x) < .01F &&
              std::abs(row.from[1] - y) < 10.F) ||
             (std::abs(row.to[0] - x) < .01F && std::abs(row.to[1] - y) < 10.F);
    });
  };
  EXPECT_TRUE(touches(-90, -20));
  EXPECT_TRUE(touches(-90, 20));
  EXPECT_TRUE(touches(90, 0));
  EXPECT_EQ(store.opCount(), operations);
}
} // namespace
