#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>

#include "common/xanadu/store.hpp"
#include "common/xanadu/store_activity_log.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

TEST(StoreActivityLogTest, BranchesAndSelectedVisitSurviveReopening) {
  const auto base =
      std::filesystem::temp_directory_path() /
      ("xuzz-activity-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::remove_all(base);
  xanadu::UserPermascroll::Config config;
  config.storageDir    = base / "permascroll";
  auto scroll          = std::make_shared<xanadu::UserPermascroll>(config);
  const auto directory = base / "activity";
  xanadu::DocumentId document;
  xanadu::VisitId origin, first, second;
  {
    xanadu::Store store(scroll);
    xanadu::StoreActivityLog log(&store, directory);
    origin = log.append({.target = xanadu::DocumentSite{document, {}, {4, 4}}});
    first  = log.append({.parent  = origin,
                         .target  = xanadu::CellSite{document, {}, 7, {2, 5}},
                         .arrival = xanadu::Arrival::EnteredEndpoint,
                         .link    = xanadu::LinkVisitContext{
                                .key    = {document, 12},
                                .active = xanadu::LinkSide::Right,
                                .left   = {.member = 0, .occurrence = 0},
                                .right  = {.member = 1, .occurrence = 0},
                                .origin = origin,
                                .originCheckpoint =
                                xanadu::DocumentSite{document, {}, {19, 19}}}});
    log.select(origin);
    second = log.append({.parent = origin,
                         .target = xanadu::CellSite{document, {}, 21, {0, 3}}});
    log.select(origin);
  }
  {
    xanadu::Store reopened(scroll);
    reopened.load(directory.string());
    xanadu::StoreActivityLog log(&reopened, directory);
    ASSERT_EQ(log.current(), origin);
    EXPECT_EQ(log.children(origin),
              (std::vector<xanadu::VisitId>{first, second}));
    ASSERT_TRUE(log.find(first));
    EXPECT_EQ(log.find(first)->link->right.member, 1U);
    EXPECT_EQ(log.find(first)->link->originCheckpoint,
              (std::optional<xanadu::OccurrenceSite>{
                  xanadu::DocumentSite{document, {}, {19, 19}}}));
    xanadu::LinkNavigator restored(log);
    ASSERT_TRUE(restored.dispatch(xanadu::nav::EnterVisit{first}));
    const auto returned = restored.dispatch(xanadu::nav::ReturnToOrigin{});
    ASSERT_TRUE(returned && returned->focus);
    EXPECT_EQ(
        *returned->focus,
        (xanadu::OccurrenceSite{xanadu::DocumentSite{document, {}, {19, 19}}}));
    EXPECT_EQ(log.current(), origin);
    EXPECT_EQ(log.walkEntries().size(), 3U);
    EXPECT_EQ(std::get<xanadu::CellSite>(log.find(second)->target).cell, 21U);
    log.select(second);
  }
  {
    xanadu::Store reopened(scroll);
    reopened.load(directory.string());
    xanadu::StoreActivityLog log(&reopened, directory);
    EXPECT_EQ(log.current(), second);
  }
  scroll.reset();
  std::filesystem::remove_all(base);
}

namespace {
class ActivityMetadataTest : public testing::Test {
protected:
  void SetUp() override {
    base = std::filesystem::temp_directory_path() /
           ("xuzz-walks-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    xanadu::UserPermascroll::Config config;
    config.storageDir = base / "permascroll";
    scroll            = std::make_shared<xanadu::UserPermascroll>(config);
    store             = std::make_unique<xanadu::Store>(scroll);
    log               = std::make_unique<xanadu::StoreActivityLog>(store.get(),
                                                                   base / "activity");
    root  = log->append({.target = xanadu::DocumentSite{document, {}, {0, 4}}});
    first = log->append(
        {.parent = root, .target = xanadu::CellSite{document, {}, 8, {0, 3}}});
    log->select(root);
    second = log->append(
        {.parent = root, .target = xanadu::CellSite{document, {}, 92, {0, 3}}});
    log->select(root);
  }
  void TearDown() override {
    log.reset();
    store.reset();
    scroll.reset();
    std::filesystem::remove_all(base);
  }
  void inject(std::string_view dimension, std::string_view text) {
    const auto made = store->makeDimension(store->latest(), dimension);
    auto at         = store->makeCell(made.version, text);
    const auto cell = store->cellRefOf(at);
    std::ignore     = store->setLink(at, store->homeCell(), made.dim,
                                     zigzag::DimVector::POS, cell);
  }
  std::filesystem::path base;
  std::shared_ptr<xanadu::UserPermascroll> scroll;
  std::unique_ptr<xanadu::Store> store;
  std::unique_ptr<xanadu::StoreActivityLog> log;
  xanadu::DocumentId document;
  xanadu::VisitId root, first, second;
};
} // namespace

TEST_F(ActivityMetadataTest,
       ReferencesAndUnicodeAnnotationsPersistWithoutVisits) {
  const std::string note =
      "Café branch\n\"quoted\" \\ note" + std::string{"\0tail", 5};
  ASSERT_TRUE(log->reference(first));
  ASSERT_TRUE(log->annotate(second, note));
  EXPECT_EQ(log->current(), root);
  EXPECT_EQ(log->walkEntries().size(), 3U);
  const auto beforeRepeat = store->latest();
  ASSERT_TRUE(log->reference(first));
  EXPECT_EQ(store->latest(), beforeRepeat);
  log.reset();
  store.reset();
  store = std::make_unique<xanadu::Store>(scroll);
  store->load((base / "activity").string());
  log = std::make_unique<xanadu::StoreActivityLog>(store.get(),
                                                   base / "activity");
  EXPECT_EQ(log->current(), root);
  const auto entries = log->walkEntries();
  ASSERT_EQ(entries.size(), 3U);
  EXPECT_TRUE(entries[1].referenced);
  EXPECT_FALSE(entries[2].referenced);
  EXPECT_EQ(entries[2].annotations, (std::vector<std::string>{note}));
  EXPECT_EQ(log->children(root), (std::vector{xanadu::VisitId{first}, second}));
}

TEST_F(ActivityMetadataTest, InvalidTargetsAndEmptyNotesAppendNothing) {
  const auto before       = store->latest();
  const auto badReference = log->reference({999});
  ASSERT_FALSE(badReference);
  EXPECT_EQ(badReference.error(), xanadu::ActivityError::UnknownVisit);
  EXPECT_FALSE(log->annotate({999}, "unknown"));
  const auto empty = log->annotate(first, "");
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error(), xanadu::ActivityError::EmptyAnnotation);
  EXPECT_EQ(store->latest(), before);
  EXPECT_EQ(log->current(), root);
  EXPECT_EQ(log->walkEntries().size(), 3U);
}

TEST_F(ActivityMetadataTest, MalformedReferenceIsRefusedByTypedError) {
  inject("d.activity-references", "reference1 1 trailing");
  EXPECT_THROW((xanadu::StoreActivityLog{store.get(), base / "activity"}),
               xanadu::ActivityRecordUnreadable);
}
TEST_F(ActivityMetadataTest, TruncatedAnnotationIsRefusedByTypedError) {
  inject("d.activity-annotations", "annotation1 1 30\nshort");
  EXPECT_THROW((xanadu::StoreActivityLog{store.get(), base / "activity"}),
               xanadu::ActivityRecordUnreadable);
}
TEST_F(ActivityMetadataTest, PersistedUnknownReferenceIsRefusedByTypedError) {
  inject("d.activity-references", "reference1 999");
  EXPECT_THROW((xanadu::StoreActivityLog{store.get(), base / "activity"}),
               xanadu::ActivityRecordUnreadable);
}

TEST_F(ActivityMetadataTest, PreviousVisitPayloadIsRefusedWithoutRewriting) {
  const auto rank = store->rebuildManifold(store->latest())
                        .dimensionNamed("d.activity-visits", *store);
  ASSERT_TRUE(rank);
  auto at           = store->makeCell(store->latest(), "visit1 0 doc");
  std::ignore       = store->setLink(at, store->homeCell(), *rank,
                                     zigzag::DimVector::POS, store->cellRefOf(at));
  const auto before = store->opCount();
  EXPECT_THROW((xanadu::StoreActivityLog{store.get(), base / "activity"}),
               xanadu::ActivityRecordUnreadable);
  EXPECT_EQ(store->opCount(), before);
}

TEST(ActivitySelectionRecordTest, OldAndTruncatedCheckpointsAreRefused) {
  EXPECT_THROW(
      xanadu::decodeLinkContext(
          "selection1 00000000000000000000000000000000 1 0 -1 -1 -1 -1 1"),
      xanadu::ActivityRecordUnreadable);
  EXPECT_THROW(
      xanadu::decodeLinkContext("selection2 00000000000000000000000000000000 1 "
                                "0 -1 -1 -1 -1 1 1 doc"),
      xanadu::ActivityRecordUnreadable);
}

TEST(ActivitySelectionRecordTest,
     NegativeAndOverflowingCheckpointNumbersAreRefused) {
  const std::string authority = "00000000000000000000000000000000";
  const auto selection        = "selection2 " + authority + " ";
  const auto context          = selection + "1 0 -1 -1 -1 -1 1 1 ";
  const auto doc              = context + "doc " + authority + " 0 ";
  const auto cell             = context + "cell " + authority + " 0 ";
  for (const auto &malformed : std::vector<std::string>{
           selection + "-1 0 -1 -1 -1 -1 1 0",
           selection + "4294967296 0 -1 -1 -1 -1 1 0",
           selection + "1 0 -1 -1 -1 -1 -1 0",
           selection + "1 0 -1 -1 -1 -1 18446744073709551616 0",
           selection + "1 0 -1 -1 -1 -1 1 -1", doc + "-1 -1 0", doc + "0 -1 0",
           doc + "4294967296 4294967296 0", cell + "0 0 -1",
           cell + "0 0 4294967296"}) {
    SCOPED_TRACE(malformed);
    EXPECT_THROW(xanadu::decodeLinkContext(malformed),
                 xanadu::ActivityRecordUnreadable);
  }
  // Every representable offset remains accepted; source availability is
  // checked by the host rather than by the private activity codec.
  const auto boundary =
      xanadu::decodeLinkContext(doc + "4294967295 4294967295 0");
  ASSERT_TRUE(boundary.originCheckpoint);
  EXPECT_EQ(std::get<xanadu::DocumentSite>(*boundary.originCheckpoint).range,
            (xanadu::Extent{4294967295U, 4294967295U}));
}

TEST_F(ActivityMetadataTest, NegativeVisitParentAndAddressesAreRefused) {
  const auto rank = store->rebuildManifold(store->latest())
                        .dimensionNamed("d.activity-visits", *store);
  ASSERT_TRUE(rank);
  for (const auto *record :
       {"visit2 -1 doc 00000000000000000000000000000000 0 0 0 0 0 0",
        "visit2 0 doc 00000000000000000000000000000000 0 -1 -1 0 0 0"}) {
    auto at           = store->makeCell(store->latest(), record);
    std::ignore       = store->setLink(at, store->homeCell(), *rank,
                                       zigzag::DimVector::POS, store->cellRefOf(at));
    const auto before = store->opCount();
    EXPECT_THROW((xanadu::StoreActivityLog{store.get(), base / "activity"}),
                 xanadu::ActivityRecordUnreadable);
    EXPECT_EQ(store->opCount(), before);
  }
}
