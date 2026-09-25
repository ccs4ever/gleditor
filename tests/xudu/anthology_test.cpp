/**
 * @file anthology_test.cpp
 * @brief Unit tests for Section 5.9: Cross-store ranks: the anthology.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "common/xanadu/anthology.hpp"
#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/extern_ref.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/publication.hpp"
#include "common/xanadu/scroll.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

using namespace xanadu;

TEST(AnthologyTest, anAnthologyMixesLocalAndForeignMembersOnOneRank) {
  const auto perma = std::make_shared<UserPermascroll>();
  Store store(perma);
  auto at = store.sliceGenesis(MicroversionId{});

  const std::string foreignKey1 =
      "btpk:1111111111111111111111111111111111111111111111111111111111111111:"
      "doc1";
  const std::string foreignKey2 =
      "btpk:2222222222222222222222222222222222222222222222222222222222222222:"
      "doc2";

  at = store.registerScroll(at, foreignKey1);
  at = store.registerScroll(at, foreignKey2);

  const auto scrollId1 = *store.scrollRegistry().scrollIdForKey(foreignKey1);
  const auto scrollId2 = *store.scrollRegistry().scrollIdForKey(foreignKey2);

  const ExternOpRef memberRef1{
      .scroll   = scrollId1,
      .produces = MicroversionId::parse("1"),
  };
  const GlobalDocumentState pinnedState1{
      .scroll  = foreignKey1,
      .version = MicroversionId::parse("2"),
  };

  const ExternOpRef memberRef2{
      .scroll   = scrollId2,
      .produces = MicroversionId::parse("1"),
  };
  const GlobalDocumentState pinnedState2{
      .scroll  = foreignKey2,
      .version = MicroversionId::parse("3"),
  };

  const auto home = store.homeCell();

  // 1. Foreign member A
  const auto entryA = store.appendAnthologyEntry(
      at, home, memberRef1, pinnedState1, "Alice Chapter 3");
  at = entryA.version;

  // 2. Local member note
  at                   = store.makeCell(at, "Editor's Preface Note");
  const auto localCell = store.cellRefOf(at);
  at                   = store.appendAnthologyLocalMember(at, home, localCell);

  // 3. Foreign member B
  const auto entryB = store.appendAnthologyEntry(at, home, memberRef2,
                                                 pinnedState2, "Bob Section 2");
  at                = entryB.version;

  const auto manifold        = store.rebuildManifold(at);
  const auto dimAnthologyOpt = manifold.dimensionNamed("d.anthology", store);
  ASSERT_TRUE(dimAnthologyOpt.has_value());
  const auto dimAnthology = *dimAnthologyOpt;

  // Walk rank of three starting from entryA with walkRank
  std::vector<zigzag::CellRef> visited;
  zigzag::walkRank(manifold, entryA.entryCell, dimAnthology,
                   [&](const zigzag::CellRef c) { visited.push_back(c); });

  ASSERT_EQ(visited.size(), 3U);
  EXPECT_EQ(visited[0], entryA.entryCell);
  EXPECT_EQ(visited[1], localCell);
  EXPECT_EQ(visited[2], entryB.entryCell);

  // Inspect the three members with readAnthologyMember
  const auto m1 = readAnthologyMember(manifold, store, visited[0]);
  ASSERT_TRUE(m1.has_value());
  EXPECT_TRUE(m1->isForeign());
  EXPECT_EQ(m1->kind, AnthologyMemberKind::ForeignEntry);
  EXPECT_EQ(m1->placeholderCell, entryA.placeholderCell);
  EXPECT_EQ(m1->label, "Alice Chapter 3");
  ASSERT_TRUE(m1->pinnedState.has_value());
  EXPECT_EQ(m1->pinnedState->version, pinnedState1.version);
  EXPECT_EQ(m1->pinnedState->scroll, foreignKey1);

  const auto m2 = readAnthologyMember(manifold, store, visited[1]);
  ASSERT_TRUE(m2.has_value());
  EXPECT_TRUE(m2->isLocal());
  EXPECT_EQ(m2->kind, AnthologyMemberKind::Local);
  EXPECT_EQ(m2->cell, localCell);
  EXPECT_EQ(m2->label, "Editor's Preface Note");
  EXPECT_EQ(m2->placeholderCell, zigzag::noCell);

  const auto m3 = readAnthologyMember(manifold, store, visited[2]);
  ASSERT_TRUE(m3.has_value());
  EXPECT_TRUE(m3->isForeign());
  EXPECT_EQ(m3->kind, AnthologyMemberKind::ForeignEntry);
  EXPECT_EQ(m3->placeholderCell, entryB.placeholderCell);
  EXPECT_EQ(m3->label, "Bob Section 2");
  ASSERT_TRUE(m3->pinnedState.has_value());
  EXPECT_EQ(m3->pinnedState->version, pinnedState2.version);
  EXPECT_EQ(m3->pinnedState->scroll, foreignKey2);

  // Verify helper anthologyMembers from home
  const auto members = anthologyMembers(manifold, store, home);
  ASSERT_EQ(members.size(), 3U);
  EXPECT_EQ(members[0].cell, entryA.entryCell);
  EXPECT_EQ(members[1].cell, localCell);
  EXPECT_EQ(members[2].cell, entryB.entryCell);
}

TEST(AnthologyTest, anAnthologyMemberPinsItsState) {
  const auto perma = std::make_shared<UserPermascroll>();

  // Foreign store: create cell at state verA, then advance to state verB
  Store foreignStore(perma);
  auto atF = foreignStore.sliceGenesis(MicroversionId{});
  atF      = foreignStore.makeCell(atF, "Alice Chapter 3 Draft 1");
  const auto foreignCell = foreignStore.cellRefOf(atF);
  const auto verA        = atF;

  // Advance foreign store past that state
  atF =
      foreignStore.setCellText(atF, foreignCell, "Alice Chapter 3 Revision 2");
  const auto verB = atF;
  EXPECT_NE(verA, verB);
  EXPECT_TRUE(verA.isAncestorOf(verB));

  Scroll sealedAs;
  sealedAs.publisher    = PublicKey::fromHex(std::string(64, 'a'));
  sealedAs.salt         = "alice_novel";
  const auto foreignKey = scrollKey(sealedAs);

  // Local anthology store cites foreignCell pinned at verA
  Store localStore(perma);
  auto atL            = localStore.sliceGenesis(MicroversionId{});
  atL                 = localStore.registerScroll(atL, foreignKey);
  const auto scrollId = *localStore.scrollRegistry().scrollIdForKey(foreignKey);

  const ExternOpRef memberRef{.scroll = scrollId, .produces = verA};
  const GlobalDocumentState pinnedState{.scroll = foreignKey, .version = verA};

  const auto entry =
      localStore.appendAnthologyEntry(atL, localStore.homeCell(), memberRef,
                                      pinnedState, "Alice ch.3 (Edition 1)");
  atL = entry.version;

  const auto manifold = localStore.rebuildManifold(atL);
  const auto member =
      readAnthologyMember(manifold, localStore, entry.entryCell);
  ASSERT_TRUE(member.has_value());
  ASSERT_TRUE(member->pinnedState.has_value());

  // Assert the anthology still names the pinned state verA, not the latest verB
  EXPECT_EQ(member->pinnedState->version, verA);
  EXPECT_NE(member->pinnedState->version, foreignStore.latest());

  // Resolving against foreign store folded at verA yields content at verA
  const auto foreignFoldA = foreignStore.rebuildManifold(verA);
  const auto resolvedA    = resolveAnthologyMember(
      localStore, *member, foreignStore, sealedAs, &foreignFoldA);
  EXPECT_EQ(resolvedA.status, AnthologyDisplayStatus::Resolved);
  EXPECT_EQ(foreignFoldA.textOf(resolvedA.resolvedForeignCell, foreignStore),
            "Alice Chapter 3 Draft 1");
}

TEST(AnthologyTest, theSameCellMayAppearTwiceInAnAnthology) {
  const auto perma = std::make_shared<UserPermascroll>();

  Store localStore(perma);
  auto at = localStore.sliceGenesis(MicroversionId{});

  const std::string foreignKey =
      "btpk:3333333333333333333333333333333333333333333333333333333333333333:"
      "poem";
  at                  = localStore.registerScroll(at, foreignKey);
  const auto scrollId = *localStore.scrollRegistry().scrollIdForKey(foreignKey);

  const auto birthOp = MicroversionId::parse("1");
  const ExternOpRef memberRef{.scroll = scrollId, .produces = birthOp};

  const GlobalDocumentState state1{.scroll  = foreignKey,
                                   .version = MicroversionId::parse("2")};
  const GlobalDocumentState state2{.scroll  = foreignKey,
                                   .version = MicroversionId::parse("5")};

  const auto home = localStore.homeCell();

  // First occurrence pins state1
  const auto entry1 = localStore.appendAnthologyEntry(
      at, home, memberRef, state1, "Opening Stanza (Early draft)");
  at = entry1.version;

  // Middle local cell separator
  at                 = localStore.makeCell(at, "--- Comparative Analysis ---");
  const auto midCell = localStore.cellRefOf(at);
  at                 = localStore.appendAnthologyLocalMember(at, home, midCell);

  // Second occurrence of same foreign cell pins state2
  const auto entry2 = localStore.appendAnthologyEntry(
      at, home, memberRef, state2, "Opening Stanza (Final revision)");
  at = entry2.version;

  // Assert both entries cite the exact same interned placeholder
  EXPECT_EQ(entry1.placeholderCell, entry2.placeholderCell);
  // But are two distinct entry cells
  EXPECT_NE(entry1.entryCell, entry2.entryCell);

  const auto manifold = localStore.rebuildManifold(at);
  const auto dimAnthologyOpt =
      manifold.dimensionNamed("d.anthology", localStore);
  ASSERT_TRUE(dimAnthologyOpt.has_value());
  const auto dimAnthology = *dimAnthologyOpt;

  std::vector<zigzag::CellRef> visited;
  zigzag::walkRank(manifold, entry1.entryCell, dimAnthology,
                   [&](const zigzag::CellRef c) { visited.push_back(c); });

  ASSERT_EQ(visited.size(), 3U);
  EXPECT_EQ(visited[0], entry1.entryCell);
  EXPECT_EQ(visited[1], midCell);
  EXPECT_EQ(visited[2], entry2.entryCell);

  const auto m1 = readAnthologyMember(manifold, localStore, entry1.entryCell);
  const auto m2 = readAnthologyMember(manifold, localStore, entry2.entryCell);

  ASSERT_TRUE(m1.has_value());
  ASSERT_TRUE(m2.has_value());
  EXPECT_EQ(m1->placeholderCell, m2->placeholderCell);
  EXPECT_EQ(m1->pinnedState->version, state1.version);
  EXPECT_EQ(m2->pinnedState->version, state2.version);
}

TEST(AnthologyTest, anAnthologySurvivesAMemberGoingDark) {
  const auto perma = std::make_shared<UserPermascroll>();
  Store store(perma);
  auto at = store.sliceGenesis(MicroversionId{});

  const std::string darkKey =
      "btpk:0000000000000000000000000000000000000000000000000000000000000000:"
      "offline_store";
  at                  = store.registerScroll(at, darkKey);
  const auto scrollId = *store.scrollRegistry().scrollIdForKey(darkKey);

  const ExternOpRef memberRef{.scroll   = scrollId,
                              .produces = MicroversionId::parse("1")};
  const GlobalDocumentState pinnedState{.scroll  = darkKey,
                                        .version = MicroversionId::parse("1")};

  const auto entry = store.appendAnthologyEntry(at, store.homeCell(), memberRef,
                                                pinnedState, "Rare Manuscript");
  at               = entry.version;

  const auto manifold = store.rebuildManifold(at);

  // Requirements from §7:
  // rank still visits the entry, d.member placeholder remains inspectable,
  // refusedOps() == 0, unresolvedExternals() == 1
  EXPECT_EQ(manifold.refusedOps(), 0U);
  EXPECT_EQ(manifold.unresolvedExternals(), 1U);

  const auto dimAnthologyOpt = manifold.dimensionNamed("d.anthology", store);
  ASSERT_TRUE(dimAnthologyOpt.has_value());
  const auto firstCell =
      zigzag::step(manifold, store.homeCell(), *dimAnthologyOpt);
  ASSERT_TRUE(firstCell.has_value());
  EXPECT_EQ(*firstCell, entry.entryCell);

  const auto member = readAnthologyMember(manifold, store, entry.entryCell);
  ASSERT_TRUE(member.has_value());
  EXPECT_TRUE(member->isForeign());
  EXPECT_EQ(member->placeholderCell, entry.placeholderCell);
  EXPECT_TRUE(manifold.contains(entry.placeholderCell));
  EXPECT_EQ(manifold.valueKindOf(entry.placeholderCell), ValueKind::ExternRef);

  // Resolution against empty store returns Absent
  Store emptyForeign(perma);
  Scroll sealedEmpty;
  sealedEmpty.publisher = PublicKey::fromHex(std::string(64, '0'));
  sealedEmpty.salt      = "offline_store";

  const auto resolved =
      resolveAnthologyMember(store, *member, emptyForeign, sealedEmpty);
  EXPECT_EQ(resolved.status, AnthologyDisplayStatus::Absent);
  EXPECT_FALSE(resolved.status == AnthologyDisplayStatus::Resolved);
}

TEST(AnthologyTest, refreshingAMemberIsAnOperation) {
  const auto perma = std::make_shared<UserPermascroll>();
  Store store(perma);
  auto at = store.sliceGenesis(MicroversionId{});

  const std::string foreignKey =
      "btpk:4444444444444444444444444444444444444444444444444444444444444444:"
      "syllabus";
  at                  = store.registerScroll(at, foreignKey);
  const auto scrollId = *store.scrollRegistry().scrollIdForKey(foreignKey);

  const auto birthOp = MicroversionId::parse("1");
  const ExternOpRef memberRef{.scroll = scrollId, .produces = birthOp};

  const auto ver1 = MicroversionId::parse("2");
  const auto ver2 = MicroversionId::parse("4");

  const GlobalDocumentState state1{.scroll = foreignKey, .version = ver1};
  const GlobalDocumentState state2{.scroll = foreignKey, .version = ver2};

  const auto entry = store.appendAnthologyEntry(at, store.homeCell(), memberRef,
                                                state1, "Week 1 Reading");
  at               = entry.version;
  const auto vBeforeRefresh = at;

  const auto manifold1 = store.rebuildManifold(vBeforeRefresh);
  const auto history1  = manifold1.historyOf(entry.entryCell);
  ASSERT_FALSE(history1.empty());

  const auto m1 = readAnthologyMember(manifold1, store, entry.entryCell);
  ASSERT_TRUE(m1.has_value());
  ASSERT_TRUE(m1->pinnedState.has_value());
  EXPECT_EQ(m1->pinnedState->version, ver1);

  // Authorially refresh the member to state2
  at = store.refreshAnthologyEntry(at, entry.entryCell, state2);
  const auto vAfterRefresh = at;
  EXPECT_NE(vBeforeRefresh, vAfterRefresh);

  const auto manifold2 = store.rebuildManifold(vAfterRefresh);
  const auto history2  = manifold2.historyOf(entry.entryCell);

  // Assert R7 chain of entryCell shows both states
  EXPECT_GT(history2.size(), history1.size());

  const auto m2 = readAnthologyMember(manifold2, store, entry.entryCell);
  ASSERT_TRUE(m2.has_value());
  ASSERT_TRUE(m2->pinnedState.has_value());
  EXPECT_EQ(m2->pinnedState->version, ver2);

  // Past state vBeforeRefresh still returns ver1
  const auto pastManifold = store.rebuildManifold(vBeforeRefresh);
  const auto pastMember =
      readAnthologyMember(pastManifold, store, entry.entryCell);
  ASSERT_TRUE(pastMember.has_value());
  ASSERT_TRUE(pastMember->pinnedState.has_value());
  EXPECT_EQ(pastMember->pinnedState->version, ver1);
}

TEST(AnthologyTest, invalidAncestryThrows) {
  const auto perma = std::make_shared<UserPermascroll>();
  Store store(perma);
  auto at = store.sliceGenesis(MicroversionId{});

  const std::string foreignKey =
      "btpk:5555555555555555555555555555555555555555555555555555555555555555:"
      "doc";
  at                  = store.registerScroll(at, foreignKey);
  const auto scrollId = *store.scrollRegistry().scrollIdForKey(foreignKey);

  // Produces is 5, but pinned version is 2 (5 is NOT an ancestor of 2)
  const ExternOpRef memberRef{.scroll   = scrollId,
                              .produces = MicroversionId::parse("5")};
  const GlobalDocumentState pinnedState{.scroll  = foreignKey,
                                        .version = MicroversionId::parse("2")};

  EXPECT_THROW(
      store.appendAnthologyEntry(at, store.homeCell(), memberRef, pinnedState),
      std::invalid_argument);
}

TEST(AnthologyTest, arenaResolutionAttachesForeignProxyWithoutCopying) {
  const auto perma = std::make_shared<UserPermascroll>();

  Store foreignStore(perma);
  auto atF               = foreignStore.sliceGenesis(MicroversionId{});
  atF                    = foreignStore.makeCell(atF, "Foreign Cell Content");
  const auto foreignCell = foreignStore.cellRefOf(atF);
  const auto foreignBirthVersion = atF;

  Scroll sealedAs;
  sealedAs.publisher    = PublicKey::fromHex(std::string(64, 'f'));
  sealedAs.salt         = "foreign_arena_doc";
  const auto foreignKey = scrollKey(sealedAs);

  Store localStore(perma);
  auto atL            = localStore.sliceGenesis(MicroversionId{});
  atL                 = localStore.registerScroll(atL, foreignKey);
  const auto scrollId = *localStore.scrollRegistry().scrollIdForKey(foreignKey);

  const ExternOpRef memberRef{.scroll   = scrollId,
                              .produces = foreignBirthVersion};
  const GlobalDocumentState pinnedState{.scroll  = foreignKey,
                                        .version = foreignBirthVersion};

  const auto entry = localStore.appendAnthologyEntry(
      atL, localStore.homeCell(), memberRef, pinnedState, "Foreign Citation");
  atL = entry.version;

  const auto localManifold = localStore.rebuildManifold(atL);
  const auto member =
      readAnthologyMember(localManifold, localStore, entry.entryCell);
  ASSERT_TRUE(member.has_value());

  // Attach foreign space into ArenaManifold
  const auto foreignManifold =
      foreignStore.rebuildManifold(foreignBirthVersion);
  zigzag::ArenaManifold arena;
  const zigzag::Space foreignSpace{
      .manifold = &foreignManifold,
      .store    = &foreignStore,
      .sealedAs = &sealedAs,
      .reader   = &foreignStore,
      .label    = "ForeignSpace",
  };
  const auto spaceId = arena.attach(foreignSpace);

  const auto resolved =
      resolveAnthologyInArena(arena, localStore, *member, spaceId, sealedAs);

  EXPECT_EQ(resolved.status, AnthologyDisplayStatus::Resolved);
  ASSERT_NE(resolved.arenaProxy, zigzag::noCell);
  EXPECT_TRUE(arena.isProxy(resolved.arenaProxy));
  EXPECT_EQ(arena.textOf(resolved.arenaProxy), "Foreign Cell Content");
}

} // namespace
