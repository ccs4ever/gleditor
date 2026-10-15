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
#include <gleditor/renderer.hpp>
#include <gleditor/text_source.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <iterator>

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
  const auto projection = glm::ortho(-200.F, 200.F, -100.F, 100.F, -10.F, 10.F);
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

TEST_F(ReaderPackageUiTest,
       ClosedNativeEndpointRefusesEntryAndSavedVisitWithoutChangingReading) {
  auto perma = std::make_shared<UserPermascroll>();
  Session session((root / "workspace").string(), perma);
  auto &store       = session.store();
  const auto text   = store.insert({}, 0, "origin destination");
  const auto pieces = store.rebuild(text);
  Link link{.left = pieces.spansFor(0, 6), .right = pieces.spansFor(7, 11)};
  const auto head = store.addLink(text, link);
  store.repointCurrentVersion(head);
  session.viewOpened(head);
  const LinkKey key{.authority = store.documentId(),
                    .id        = store.links().begin()->first};
  LinkContext navigation(session);
  std::optional<Extent> focused;
  navigation.setFocusDocument([&](std::size_t view, Extent range) {
    EXPECT_EQ(view, 0U);
    focused = range;
  });
  navigation.setCaretQuery([] {
    return std::optional{LinkContext::CaretPosition{.view = 0, .offset = 0}};
  });
  ASSERT_TRUE(navigation.execute(nav::SelectLink{.key = key}));
  ASSERT_TRUE(navigation.execute(nav::SelectOccurrence{
      .key = key, .side = LinkSide::Right, .member = 0, .occurrence = 0}));
  ASSERT_TRUE(navigation.execute(nav::Enter{}));
  EXPECT_EQ(focused, (Extent{7, 18}));
  const auto destination = navigation.currentVisit();
  ASSERT_TRUE(destination);
  navigation.recordSourceArrival(DocumentSite{
      .store = store.documentId(), .version = head, .range = {0, 6}});
  const auto current = navigation.currentVisit();
  ASSERT_NE(current, destination);
  const auto visits      = navigation.walkEntries().size();
  const auto activityOps = session.activityForNavigation()->opCount();
  const auto operations  = store.opCount();
  const auto selected    = *navigation.selection();
  session.views().clear();

  for (const NavigationCommand &command :
       {NavigationCommand{nav::Enter{}},
        NavigationCommand{nav::EnterAt{
            .key = key, .side = LinkSide::Right, .member = 0, .occurrence = 0}},
        NavigationCommand{nav::EnterVisit{.visit = *destination}}}) {
    const auto result = navigation.execute(command);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), NavigationError::MemberNotInView);
    EXPECT_FALSE(navigation.entryStatus().empty());
    EXPECT_EQ(navigation.currentVisit(), current);
    EXPECT_EQ(navigation.walkEntries().size(), visits);
    EXPECT_EQ(session.activityForNavigation()->opCount(), activityOps);
    ASSERT_TRUE(navigation.selection());
    EXPECT_EQ(navigation.selection()->key, selected.key);
    EXPECT_EQ(navigation.selection()->left, selected.left);
    EXPECT_EQ(navigation.selection()->right, selected.right);
    EXPECT_EQ(navigation.selection()->active, selected.active);
    EXPECT_EQ(focused, (Extent{7, 18}));
  }
  ASSERT_TRUE(navigation.execute(nav::CancelEntry{}));
  EXPECT_TRUE(navigation.entryStatus().empty());
  EXPECT_EQ(navigation.currentVisit(), current);
  EXPECT_EQ(session.activityForNavigation()->opCount(), activityOps);
  EXPECT_EQ(store.opCount(), operations);
}

TEST_F(ReaderPackageUiTest,
       NewSourceArrivalCancelsDeferredEntryWithoutLosingTheLink) {
  auto perma = std::make_shared<UserPermascroll>();
  Session session((root / "workspace").string(), perma);
  auto &store       = session.store();
  const auto text   = store.insert({}, 0, "origin destination");
  const auto pieces = store.rebuild(text);
  Link link{.left = pieces.spansFor(0, 6), .right = pieces.spansFor(7, 11)};
  const auto head = store.addLink(text, link);
  session.viewOpened(head);
  const LinkKey key{.authority = store.documentId(),
                    .id        = store.links().begin()->first};
  WorldRecordingDevice device;
  RenderState state{&device};
  auto app      = std::make_shared<AppState>();
  auto renderer = Renderer::create(app, render::Backend::OpenGL);
  auto doc      = Doc::create(
      renderer, &device, glm::mat4{1},
      gleditor::MemoryTextSource("origin destination", "deferred-entry"));
  state.docs.push_back(doc);
  LinkContext navigation(session);
  LinkContext::CaretPosition caret{.view = 0, .offset = 0};
  navigation.setCaretQuery([&] { return std::optional{caret}; });
  std::optional<Extent> focused;
  navigation.setDocumentEntryHandler(
      [&](std::size_t view, Extent range, RenderState &) {
        EXPECT_EQ(view, 0U);
        focused = range;
      });
  navigation.advanceFrame(state);
  ASSERT_TRUE(navigation.execute(nav::SelectLink{.key = key}));
  ASSERT_TRUE(navigation.execute(nav::SelectOccurrence{
      .key = key, .side = LinkSide::Right, .member = 0, .occurrence = 0}));
  const auto selected    = *navigation.selection();
  const auto operations  = store.opCount();
  const auto visits      = navigation.walkEntries().size();
  const auto activityOps = session.activityForNavigation()->opCount();
  ASSERT_FALSE(doc->anchorFor(7));
  ASSERT_TRUE(navigation.execute(nav::Enter{}));
  ASSERT_TRUE(navigation.entryPending());
  EXPECT_FALSE(focused);
  EXPECT_EQ(navigation.walkEntries().size(), visits);
  EXPECT_EQ(session.activityForNavigation()->opCount(), activityOps);

  ASSERT_TRUE(navigation.execute(nav::CancelEntry{}));
  EXPECT_FALSE(navigation.entryPending());
  ASSERT_TRUE(navigation.selection());
  EXPECT_EQ(navigation.selection()->left, selected.left);
  EXPECT_EQ(navigation.selection()->right, selected.right);
  EXPECT_EQ(navigation.walkEntries().size(), visits);
  EXPECT_EQ(session.activityForNavigation()->opCount(), activityOps);
  ASSERT_TRUE(navigation.execute(nav::Enter{}));
  ASSERT_TRUE(navigation.entryPending());

  // Swing Back to an already open quotation source changes reading focus
  // without changing the session's view epoch.
  const auto views = session.generation();
  caret.selection  = Extent{0, 6};
  navigation.recordSourceArrival(DocumentSite{
      .store = store.documentId(), .version = head, .range = {0, 6}});
  EXPECT_EQ(session.generation(), views);
  EXPECT_FALSE(navigation.entryPending());
  const auto arrived    = navigation.currentVisit();
  const auto arrivals   = navigation.walkEntries().size();
  const auto arrivedOps = session.activityForNavigation()->opCount();
  EXPECT_EQ(arrivals, visits + 1);

  doc->makePages();
  std::ignore = doc->buildPendingPages(state);
  ASSERT_TRUE(doc->anchorFor(7));
  ASSERT_TRUE(doc->anchorFor(17));
  navigation.advanceFrame(state);
  navigation.advanceFrame(state);
  EXPECT_FALSE(focused);
  EXPECT_EQ(navigation.currentVisit(), arrived);
  EXPECT_EQ(navigation.walkEntries().size(), arrivals);
  EXPECT_EQ(session.activityForNavigation()->opCount(), arrivedOps);
  ASSERT_TRUE(navigation.selection());
  EXPECT_EQ(navigation.selection()->key, selected.key);
  EXPECT_EQ(navigation.selection()->left, selected.left);
  EXPECT_EQ(navigation.selection()->right, selected.right);
  EXPECT_EQ(navigation.selection()->active, selected.active);
  EXPECT_EQ(navigation.selection()->origin, selected.origin);
  EXPECT_EQ(navigation.selection()->originCheckpoint,
            selected.originCheckpoint);
  EXPECT_EQ(store.opCount(), operations);

  ASSERT_TRUE(navigation.execute(nav::Enter{}));
  EXPECT_EQ(focused, (Extent{7, 18}));
  EXPECT_EQ(navigation.walkEntries().size(), arrivals + 1);
  EXPECT_EQ(store.opCount(), operations);
}

TEST_F(ReaderPackageUiTest,
       SavedCellEntryRestoresHistoricalVersionAndExactContentRange) {
  auto perma = std::make_shared<UserPermascroll>();
  Session session((root / "workspace").string(), perma);
  auto &store           = session.store();
  const auto historical = store.makeCell({}, "old endpoint text");
  const auto cell       = store.cellRefOf(historical);
  const auto latest     = store.spliceCell(historical, cell, 0, 3, "new");
  store.repointCurrentVersion(latest);
  const auto bound = store.rebuildManifold(latest);
  LinkContext navigation(session);
  navigation.setManifold(&bound, 0, latest);
  const CellSite target{.store   = store.documentId(),
                        .version = historical,
                        .cell    = cell,
                        .range   = {4, 12}};
  navigation.recordSourceArrival(target);
  const auto saved = navigation.currentVisit();
  ASSERT_TRUE(saved);
  navigation.recordSourceArrival(CellSite{.store   = store.documentId(),
                                          .version = latest,
                                          .cell    = cell,
                                          .range   = {0, 3}});
  WorldRecordingDevice device;
  RenderState state{&device};
  navigation.advanceFrame(state);
  std::optional<CellSite> entered;
  navigation.setCellEntryHandler(
      [&](RenderState &received, std::size_t index, const CellSite &site) {
        EXPECT_EQ(&received, &state);
        EXPECT_EQ(index, 0U);
        entered = site;
      });
  navigation.setFocusCell([](zigzag::CellRef, Extent) {
    ADD_FAILURE() << "Historical entry must use the exact-version callback";
  });
  const auto operations  = store.opCount();
  const auto activityOps = session.activityForNavigation()->opCount();
  const auto visits      = navigation.walkEntries().size();
  ASSERT_TRUE(navigation.execute(nav::EnterVisit{.visit = *saved}));
  EXPECT_EQ(entered, target);
  EXPECT_EQ(navigation.currentVisit(), saved);
  EXPECT_EQ(navigation.walkEntries().size(), visits);
  EXPECT_GT(session.activityForNavigation()->opCount(), activityOps);
  const auto checkpointOps = session.activityForNavigation()->opCount();
  ASSERT_TRUE(navigation.execute(nav::EnterVisit{.visit = *saved}));
  EXPECT_EQ(session.activityForNavigation()->opCount(), checkpointOps);
  EXPECT_EQ(navigation.walkEntries().size(), visits);
  EXPECT_EQ(store.opCount(), operations);
  EXPECT_EQ(bound.textOf(cell, store), "new endpoint text");
}

TEST_F(ReaderPackageUiTest,
       WalksPreviewAndMetadataPreserveCurrentVisitAndHistoricalExcerpt) {
  auto perma = std::make_shared<UserPermascroll>();
  Session session((root / "workspace").string(), perma);
  auto &store           = session.store();
  const auto historical = store.insert({}, 0, "old passage");
  const auto latest     = store.insert(historical, 0, "new prefix ");
  store.repointCurrentVersion(latest);
  session.viewOpened(latest);
  LinkContext navigation(session);
  navigation.recordSourceArrival(DocumentSite{
      .store = store.documentId(), .version = historical, .range = {4, 11}});
  const auto saved = navigation.currentVisit();
  ASSERT_TRUE(saved);
  navigation.recordSourceArrival(DocumentSite{
      .store = store.documentId(), .version = latest, .range = {0, 3}});
  const auto current     = navigation.currentVisit();
  const auto stamp       = navigation.readingStamp();
  const auto visits      = navigation.walkEntries().size();
  const auto activityOps = session.activityForNavigation()->opCount();
  const auto operations  = store.opCount();
  EXPECT_EQ(navigation.previewVisit(*saved), "a closed version: passage");
  EXPECT_EQ(navigation.previewVisit(VisitId{999}), "Unavailable visit");
  EXPECT_EQ(navigation.readingStamp(), stamp);
  EXPECT_EQ(navigation.currentVisit(), current);
  EXPECT_EQ(session.activityForNavigation()->opCount(), activityOps);
  ASSERT_TRUE(navigation.referenceVisit(*saved));
  const auto referencedOps = session.activityForNavigation()->opCount();
  ASSERT_TRUE(navigation.referenceVisit(*saved));
  EXPECT_EQ(session.activityForNavigation()->opCount(), referencedOps);
  const std::string note = "Reader café\nsecond line";
  ASSERT_TRUE(navigation.annotateVisit(*saved, note));
  const auto entries    = navigation.walkEntries();
  const auto savedEntry = std::ranges::find_if(
      entries, [&](const auto &entry) { return entry.visit.id == *saved; });
  ASSERT_NE(savedEntry, entries.end());
  EXPECT_TRUE(savedEntry->referenced);
  EXPECT_EQ(savedEntry->annotations, std::vector<std::string>{note});
  EXPECT_EQ(navigation.readingStamp(), stamp);
  EXPECT_EQ(navigation.currentVisit(), current);
  EXPECT_EQ(entries.size(), visits);
  EXPECT_EQ(store.opCount(), operations);
}
} // namespace

TEST_F(ReaderPackageUiTest, NativeFormattingIsNotAReaderNavigationCandidate) {
  auto perma = std::make_shared<UserPermascroll>();
  Session session((root / "workspace").string(), perma);
  const auto secondary = session.createNewStore((root / "secondary").string());
  auto &store          = session.store(secondary);
  const auto text      = store.insert({}, 0, "left right");
  const auto pieces    = store.rebuild(text);
  const Link comment{.left  = pieces.spansFor(0, 4),
                     .right = pieces.spansFor(5, 5)};
  auto format          = comment;
  format.type          = LinkType::Format;
  const auto formatted = store.addLink(text, format);
  const auto head      = store.addLink(formatted, comment);
  store.repointCurrentVersion(head);
  session.viewOpened(head, secondary);
  const auto commentId =
      std::ranges::find_if(store.links(), [](const auto &entry) {
        return entry.second.type == LinkType::Comment;
      });
  ASSERT_NE(commentId, store.links().end());
  // Loading configuration must not introduce its metadata links into a
  // reader's candidate cycle; opening that store deliberately may do so.
  std::ignore = session.systemStoreIndex(SystemDocKind::Keymap);
  std::ignore = session.systemStoreIndex(SystemDocKind::Settings);
  LinkContext navigation(session);
  navigation.setCandidates({});
  ASSERT_TRUE(navigation.execute(nav::StepLink{.delta = 1}));
  ASSERT_TRUE(navigation.selection());
  EXPECT_EQ(navigation.selection()->key,
            (LinkKey{store.documentId(), commentId->first}));
  const auto selected = navigation.selection()->key;
  ASSERT_TRUE(navigation.execute(nav::StepLink{.delta = 1}));
  EXPECT_EQ(navigation.selection()->key, selected);
  const auto presented = navigation.presentationLinks(-1);
  ASSERT_EQ(presented.size(), 1U);
  EXPECT_EQ(presented.front().link.type, LinkType::Comment);
}

TEST_F(ReaderPackageUiTest,
       CaretMovementAndLinkExplorationDoNotCreateCompletedVisits) {
  auto perma = std::make_shared<UserPermascroll>();
  Session session((root / "workspace").string(), perma);
  auto &store       = session.store();
  const auto text   = store.insert({}, 0, "origin destination");
  const auto pieces = store.rebuild(text);
  const auto head   = store.addLink(text, Link{.left  = pieces.spansFor(0, 6),
                                               .right = pieces.spansFor(7, 11)});
  store.repointCurrentVersion(head);
  session.viewOpened(head);
  const LinkKey key{store.documentId(), store.links().begin()->first};
  LinkContext navigation(session);
  std::uint32_t caret{};
  navigation.setCaretQuery([&] {
    return std::optional{
        LinkContext::CaretPosition{.view = 0, .offset = caret}};
  });
  navigation.setFocusDocument([&](std::size_t view, Extent range) {
    EXPECT_EQ(view, 0U);
    caret = range.start;
  });
  WorldRecordingDevice device;
  RenderState state{&device};
  navigation.advanceFrame(state);
  const auto opened = navigation.currentVisit();
  ASSERT_TRUE(opened);
  ASSERT_EQ(navigation.walkEntries().size(), 1U);
  const auto activityOps = session.activityForNavigation()->opCount();
  const auto documentOps = store.opCount();
  caret                  = 3;
  navigation.advanceFrame(state);
  ASSERT_TRUE(navigation.execute(nav::SelectLink{.key = key}));
  const OccurrenceSite checkpoint =
      DocumentSite{store.documentId(), head, {3, 3}};
  EXPECT_EQ(navigation.originSite(), checkpoint);
  caret = 5;
  ASSERT_TRUE(navigation.execute(nav::SelectLink{.key = key}));
  EXPECT_EQ(navigation.originSite(), checkpoint);
  ASSERT_TRUE(navigation.execute(nav::SelectOccurrence{
      .key = key, .side = LinkSide::Right, .member = 0, .occurrence = 0}));
  ASSERT_TRUE(navigation.execute(nav::Cross{}));
  ASSERT_TRUE(navigation.execute(nav::Cross{}));
  // The source navigation callback must not record eye movement either.
  navigation.recordReadingOrigin();
  EXPECT_EQ(navigation.walkEntries().size(), 1U);
  EXPECT_EQ(session.activityForNavigation()->opCount(), activityOps);
  EXPECT_EQ(navigation.currentVisit(), opened);
  ASSERT_TRUE(navigation.execute(nav::Enter{}));
  EXPECT_EQ(caret, 7U);
  ASSERT_EQ(navigation.walkEntries().size(), 2U);
  const auto entered = navigation.currentVisit();
  ASSERT_TRUE(entered);
  ASSERT_TRUE(navigation.execute(nav::ActivityBack{}));
  EXPECT_EQ(caret, 0U);
  ASSERT_TRUE(navigation.execute(nav::ActivityForward{*entered}));
  ASSERT_TRUE(navigation.execute(nav::ReturnToOrigin{}));
  EXPECT_EQ(caret, 3U);
  EXPECT_EQ(navigation.currentVisit(), opened);
  EXPECT_EQ(navigation.walkEntries().size(), 2U);
  EXPECT_EQ(store.opCount(), documentOps);
}

TEST_F(ReaderPackageUiTest, UnsupportedActivityPayloadsRemainUntouched) {
  auto perma           = std::make_shared<UserPermascroll>();
  const auto directory = activityDirectory();
  const auto fileBytes = [](const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
  };
  for (const bool savedSelection : {false, true}) {
    std::filesystem::remove_all(directory);
    Store stale(perma);
    auto at         = stale.sliceGenesis({});
    const auto rank = stale.makeDimension(
        at, savedSelection ? "d.places" : "d.activity-visits");
    at = stale.makeCell(
        rank.version,
        savedSelection
            ? "place"
            : "visit2 0 doc 00000000000000000000000000000000 0 0 0 0 0 0");
    const auto record = stale.cellRefOf(at);
    at = stale.setLink(at, stale.homeCell(), rank.dim, zigzag::DimVector::POS,
                       record);
    if (savedSelection) {
      const auto selection = stale.makeDimension(at, "d.selection");
      at                   = stale.makeCell(
          selection.version,
          "selection2 00000000000000000000000000000000 1 0 -1 -1 -1 -1 0 0");
      at = stale.setLink(at, record, selection.dim, zigzag::DimVector::POS,
                         stale.cellRefOf(at));
    }
    stale.save(directory.string());
    const auto nodes  = fileBytes(directory / "ops.nodes");
    const auto tables = fileBytes(directory / "store.tables");
    Session session((root / "workspace").string(), perma);
    EXPECT_EQ(session.activityForNavigation(), nullptr);
    EXPECT_FALSE(session.lastPlace());
    LinkContext navigation(session);
    EXPECT_NO_THROW(navigation.recordSourceArrival(
        DocumentSite{session.store().documentId(), {}, {0, 0}}));
    EXPECT_NO_THROW(session.rememberPlace({}));
    EXPECT_EQ(fileBytes(directory / "ops.nodes"), nodes);
    EXPECT_EQ(fileBytes(directory / "store.tables"), tables);
  }
}

TEST_F(ReaderPackageUiTest, RestoredClosedPlaceNeverEntersAnotherOpenVersion) {
  auto perma = std::make_shared<UserPermascroll>();
  Session session((root / "workspace").string(), perma);
  auto &store       = session.store();
  const auto text   = store.insert({}, 0, "origin destination");
  const auto pieces = store.rebuild(text);
  const auto head   = store.addLink(text, Link{.left  = pieces.spansFor(0, 6),
                                               .right = pieces.spansFor(7, 11)});
  const auto wrong  = store.insert(head, 0, "shift ");
  session.viewOpened(head);
  LinkContext original(session);
  const LinkKey key{store.documentId(), store.links().begin()->first};
  ASSERT_TRUE(
      original.execute(nav::SelectOccurrence{key, LinkSide::Right, 0, 0}));
  const auto saved = selectionContext(*original.selection());
  ASSERT_TRUE(saved.rightPin);
  session.viewClosed(0);
  session.viewOpened(wrong);
  const auto operations = store.opCount();
  LinkContext resumed(session);
  std::optional<Extent> focus;
  resumed.setFocusDocument([&](std::size_t view, Extent range) {
    EXPECT_EQ(view, 0U);
    focus = range;
  });
  resumed.restoreSelection(saved);
  ASSERT_TRUE(resumed.selection());
  EXPECT_EQ(selectionContext(*resumed.selection()).rightPin, saved.rightPin);
  const auto visits      = resumed.walkEntries().size();
  const auto activityOps = session.activityForNavigation()->opCount();
  EXPECT_FALSE(resumed.execute(nav::Enter{}));
  EXPECT_FALSE(focus);
  EXPECT_FALSE(resumed.entryStatus().empty());
  EXPECT_EQ(resumed.walkEntries().size(), visits);
  EXPECT_EQ(session.activityForNavigation()->opCount(), activityOps);
  session.viewClosed(0);
  session.viewOpened(head);
  ASSERT_TRUE(resumed.execute(nav::Enter{}));
  EXPECT_EQ(focus, (Extent{7, 18}));
  EXPECT_EQ(resumed.walkEntries().size(), visits + 1);
  EXPECT_EQ(store.opCount(), operations);
}
