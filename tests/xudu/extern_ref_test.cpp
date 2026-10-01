/**
 * @file extern_ref_test.cpp
 * @brief Unit tests for Section 5.5: Persistent references to cells in other
 * stores.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/extern_ref.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/publication.hpp"
#include "common/xanadu/scroll.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

namespace fs = std::filesystem;
using namespace xanadu;

fs::path scratch(const std::string &name) {
  const auto dir = fs::temp_directory_path() / ("xudu_extern_" + name);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

TEST(ExternRefTest, anExternPlaceholderSurvivesAReload) {
  const auto dir   = scratch("survives_reload");
  const auto perma = std::make_shared<UserPermascroll>();
  zigzag::CellRef placeholderCell{zigzag::noCell};
  const std::string globalKey =
      "btpk:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef:"
      "doc1";
  const auto targetProduces = MicroversionId::parse("2a3");

  {
    Store store(perma);
    auto at                = store.sliceGenesis(MicroversionId{});
    at                     = store.registerScroll(at, globalKey);
    const auto scrollIdOpt = store.scrollRegistry().scrollIdForKey(globalKey);
    ASSERT_TRUE(scrollIdOpt.has_value());
    const auto scrollId = *scrollIdOpt;

    const ExternOpRef targetRef{.scroll = scrollId, .produces = targetProduces};
    at                 = store.makeExternRef(at, targetRef);
    const auto optCell = store.placeholderForExtern(targetRef);
    ASSERT_TRUE(optCell.has_value());
    placeholderCell = *optCell;

    store.save(dir.string());
  }

  Store reloaded(perma);
  reloaded.load(dir.string());

  const auto manifold = reloaded.rebuildManifold(reloaded.latest());
  ASSERT_TRUE(manifold.contains(placeholderCell));
  EXPECT_EQ(manifold.valueKindOf(placeholderCell), ValueKind::ExternRef);
  EXPECT_EQ(manifold.textOf(placeholderCell, reloaded), targetProduces.str());

  const auto targetOpt = reloaded.externTarget(placeholderCell);
  ASSERT_TRUE(targetOpt.has_value());
  EXPECT_EQ(targetOpt->produces, targetProduces);
  const auto *const scrollRec =
      reloaded.scrollRegistry().recordForId(targetOpt->scroll);
  ASSERT_NE(nullptr, scrollRec);
  EXPECT_EQ(scrollRec->globalKey, globalKey);

  const auto indexedPlaceholder =
      reloaded.scrollRegistry().placeholderForExtern(*targetOpt);
  ASSERT_TRUE(indexedPlaceholder.has_value());
  EXPECT_EQ(*indexedPlaceholder, placeholderCell);
}

TEST(ExternRefTest, anUnresolvableExternIsNotCorruption) {
  const auto perma = std::make_shared<UserPermascroll>();
  Store store(perma);
  auto at = store.sliceGenesis(MicroversionId{});
  const std::string globalKey =
      "btpk:unknown000000000000000000000000000000000000000000000000000000000:"
      "missing";
  at                  = store.registerScroll(at, globalKey);
  const auto scrollId = *store.scrollRegistry().scrollIdForKey(globalKey);

  const ExternOpRef targetRef{.scroll   = scrollId,
                              .produces = MicroversionId::parse("5")};
  at                 = store.makeExternRef(at, targetRef);
  const auto optCell = store.placeholderForExtern(targetRef);
  ASSERT_TRUE(optCell.has_value());
  const auto placeholder = *optCell;

  const auto manifold = store.rebuildManifold(at);
  EXPECT_EQ(manifold.refusedOps(), 0U);
  EXPECT_EQ(manifold.unresolvedExternals(), 1U);
  ASSERT_EQ(manifold.externalCells().size(), 1U);
  EXPECT_EQ(manifold.externalCells().front(), placeholder);
}

TEST(ExternRefTest, aRankDeadEndsAtAPlaceholderRatherThanVanishing) {
  const auto perma = std::make_shared<UserPermascroll>();
  Store store(perma);
  auto at            = store.sliceGenesis(MicroversionId{});
  const auto dimTest = store.makeDimension(at, "d.test").dim;
  at                 = store.structureHead();

  at = store.registerScroll(
      at,
      "btpk:foreign1111111111111111111111111111111111111111111111111111111111"
      "11:doc");
  const auto scrollId = *store.scrollRegistry().scrollIdForKey(
      "btpk:foreign1111111111111111111111111111111111111111111111111111111111"
      "11:doc");

  at                    = store.makeCell(at, "Local Start Cell");
  const auto localStart = store.cellRefOf(at);

  const ExternOpRef targetRef{.scroll   = scrollId,
                              .produces = MicroversionId::parse("3b2")};
  at                 = store.makeExternRef(at, targetRef);
  const auto optCell = store.placeholderForExtern(targetRef);
  ASSERT_TRUE(optCell.has_value());
  const auto placeholder = *optCell;

  at = store.setLink(at, localStart, dimTest, zigzag::DimVector::POS,
                     placeholder);

  const auto manifold = store.rebuildManifold(at);
  EXPECT_EQ(manifold.linked(localStart, dimTest, zigzag::DimVector::POS),
            placeholder);
  EXPECT_EQ(manifold.linked(placeholder, dimTest, zigzag::DimVector::NEG),
            localStart);
  EXPECT_EQ(manifold.textOf(placeholder, store), "3b2");
}

TEST(ExternRefTest, aPlaceholderIsNotEphemeral) {
  const auto perma = std::make_shared<UserPermascroll>();
  Store store(perma);
  auto at            = store.sliceGenesis(MicroversionId{});
  const auto dimTest = store.makeDimension(at, "d.test").dim;
  at                 = store.structureHead();

  at = store.registerScroll(
      at,
      "btpk:foreign2222222222222222222222222222222222222222222222222222222222"
      "22:doc");
  const auto scrollId = *store.scrollRegistry().scrollIdForKey(
      "btpk:foreign2222222222222222222222222222222222222222222222222222222222"
      "22:doc");

  const ExternOpRef targetRef{.scroll   = scrollId,
                              .produces = MicroversionId::parse("1")};
  at                 = store.makeExternRef(at, targetRef);
  const auto optCell = store.placeholderForExtern(targetRef);
  ASSERT_TRUE(optCell.has_value());
  const auto placeholder = *optCell;
  EXPECT_FALSE(zigzag::isEphemeral(placeholder));

  at                 = store.makeCell(at, "Subject Cell");
  const auto subject = store.cellRefOf(at);

  EXPECT_NO_THROW({
    at = store.setLink(at, subject, dimTest, zigzag::DimVector::POS,
                       placeholder);
  });
  const auto manifold = store.rebuildManifold(at);
  EXPECT_EQ(manifold.refusedOps(), 0U);

  const auto fakeEphemeral = placeholder | zigzag::ephemeralBit;
  EXPECT_THROW(
      {
        store.setLink(at, subject, dimTest, zigzag::DimVector::POS,
                      fakeEphemeral);
      },
      std::invalid_argument);
}

TEST(ExternRefTest, anExternRefSurvivesTheForeignStoreGainingABranch) {
  const auto perma = std::make_shared<UserPermascroll>();

  Store foreignStore(perma);
  const auto atF         = foreignStore.sliceGenesis(MicroversionId{});
  const auto baseVersion = atF;

  // Branch 1: add some ops
  const auto b1_1 = foreignStore.insert(baseVersion, 0, "branch 1 text");
  static_cast<void>(foreignStore.insert(b1_1, 0, "more branch 1 text"));

  // Branch 2: make the cell on branch 2
  const auto b2_1 = foreignStore.makeCell(baseVersion, "Foreign Cell Content");
  const auto foreignBirthVersion = b2_1;
  const auto foreignCellHere     = foreignStore.cellRefOf(foreignBirthVersion);

  Scroll sealedAs;
  sealedAs.publisher    = PublicKey::fromHex(std::string(64, 'a'));
  sealedAs.salt         = "test_salt";
  const auto foreignKey = scrollKey(sealedAs);

  const auto history = historyFromSeal(sealableOps(foreignStore), sealedAs, {});
  ASSERT_NE(history, nullptr);

  const auto foreignCellThere =
      history->segmentedOps().indexOf(foreignBirthVersion);
  ASSERT_NE(foreignCellHere, 0U);
  ASSERT_NE(foreignCellThere, 0U);
  EXPECT_NE(foreignCellHere, foreignCellThere)
      << "this test proves nothing unless the index moved";

  Store localStore(perma);
  auto atL            = localStore.sliceGenesis(MicroversionId{});
  atL                 = localStore.registerScroll(atL, foreignKey);
  const auto scrollId = *localStore.scrollRegistry().scrollIdForKey(foreignKey);

  const ExternOpRef targetRef{.scroll   = scrollId,
                              .produces = foreignBirthVersion};
  atL                = localStore.makeExternRef(atL, targetRef);
  const auto optCell = localStore.placeholderForExtern(targetRef);
  ASSERT_TRUE(optCell.has_value());
  const auto placeholder = *optCell;

  const auto historyFold = history->rebuildManifold(foreignBirthVersion);
  const auto resolution  = resolveExternCell(localStore, placeholder, *history,
                                             sealedAs, &historyFold);

  EXPECT_EQ(resolution.status, ExternResolutionStatus::Resolved);
  EXPECT_TRUE(resolution.isResolved());
  EXPECT_EQ(resolution.cell, foreignCellThere);
  EXPECT_NE(resolution.cell, foreignCellHere);
}

TEST(ExternRefTest, anExternRefIsRefusedForTheWrongDocument) {
  const auto perma = std::make_shared<UserPermascroll>();

  Store foreignStore(perma);
  auto atF                       = foreignStore.sliceGenesis(MicroversionId{});
  atF                            = foreignStore.makeCell(atF, "Foreign Cell");
  const auto foreignBirthVersion = atF;

  Scroll sealedAsDocA;
  sealedAsDocA.publisher = PublicKey::fromHex(std::string(64, 'a'));
  sealedAsDocA.salt      = "doc_A";

  Scroll sealedAsDocB;
  sealedAsDocB.publisher = PublicKey::fromHex(std::string(64, 'b'));
  sealedAsDocB.salt      = "doc_B";

  Store localStore(perma);
  auto atL = localStore.sliceGenesis(MicroversionId{});
  atL      = localStore.registerScroll(atL, scrollKey(sealedAsDocA));
  const auto scrollId =
      *localStore.scrollRegistry().scrollIdForKey(scrollKey(sealedAsDocA));

  const ExternOpRef targetRef{.scroll   = scrollId,
                              .produces = foreignBirthVersion};
  atL                = localStore.makeExternRef(atL, targetRef);
  const auto optCell = localStore.placeholderForExtern(targetRef);
  ASSERT_TRUE(optCell.has_value());
  const auto placeholder = *optCell;

  // Attempt resolving against foreign store sealed as doc B instead of doc A
  const auto resolution =
      resolveExternCell(localStore, placeholder, foreignStore, sealedAsDocB);

  EXPECT_EQ(resolution.status, ExternResolutionStatus::Absent);
  EXPECT_FALSE(resolution.isResolved());
}

TEST(ExternRefTest, aResolvedExternCellMustNameItsBirthOperation) {
  const auto perma = std::make_shared<UserPermascroll>();

  Store foreignStore(perma);
  auto atF        = foreignStore.sliceGenesis(MicroversionId{});
  atF             = foreignStore.makeCell(atF, "Initial Text");
  const auto cell = foreignStore.cellRefOf(atF);

  // Perform a SetValue operation on the foreign cell
  atF = foreignStore.setCellText(atF, cell, "Updated Text");
  const auto nonBirthVersion = atF;

  Scroll sealedAs;
  sealedAs.publisher    = PublicKey::fromHex(std::string(64, 'c'));
  sealedAs.salt         = "salt_c";
  const auto foreignKey = scrollKey(sealedAs);

  Store localStore(perma);
  auto atL            = localStore.sliceGenesis(MicroversionId{});
  atL                 = localStore.registerScroll(atL, foreignKey);
  const auto scrollId = *localStore.scrollRegistry().scrollIdForKey(foreignKey);

  // Reference the SetValue operation instead of MakeCell
  const ExternOpRef targetRef{.scroll = scrollId, .produces = nonBirthVersion};
  atL                = localStore.makeExternRef(atL, targetRef);
  const auto optCell = localStore.placeholderForExtern(targetRef);
  ASSERT_TRUE(optCell.has_value());
  const auto placeholder = *optCell;

  const auto resolution =
      resolveExternCell(localStore, placeholder, foreignStore, sealedAs);

  EXPECT_EQ(resolution.status, ExternResolutionStatus::Unintelligible);
  EXPECT_FALSE(resolution.isResolved());
}

TEST(ExternRefTest, twoReferencesToOneForeignStoreShareAScrollCell) {
  const auto perma = std::make_shared<UserPermascroll>();
  Store store(perma);
  auto at = store.sliceGenesis(MicroversionId{});
  const std::string foreignKey =
      "btpk:foreign3333333333333333333333333333333333333333333333333333333333"
      "33:shared";
  at                  = store.registerScroll(at, foreignKey);
  const auto scrollId = *store.scrollRegistry().scrollIdForKey(foreignKey);

  const ExternOpRef targetRef1{.scroll   = scrollId,
                               .produces = MicroversionId::parse("1")};
  at               = store.makeExternRef(at, targetRef1);
  const auto optP1 = store.placeholderForExtern(targetRef1);
  ASSERT_TRUE(optP1.has_value());
  const auto p1 = *optP1;

  const ExternOpRef targetRef2{.scroll   = scrollId,
                               .produces = MicroversionId::parse("2")};
  at               = store.makeExternRef(at, targetRef2);
  const auto optP2 = store.placeholderForExtern(targetRef2);
  ASSERT_TRUE(optP2.has_value());
  const auto p2 = *optP2;

  EXPECT_NE(p1, p2);

  // Exactly one scroll record in registry
  EXPECT_EQ(store.scrollRegistry().scrolls.size(), 1U);

  const auto manifold      = store.rebuildManifold(at);
  const auto dimScrollRefs = manifold.dimensionNamed("d.scroll-refs", store);
  ASSERT_TRUE(dimScrollRefs.has_value());

  const auto scrollCell = store.scrollRegistry().scrolls.front().cell;
  const auto visitedRefs =
      zigzag::rankAfter(manifold, scrollCell, *dimScrollRefs) |
      std::ranges::to<std::vector>();
  EXPECT_THAT(visitedRefs, testing::ElementsAre(p1, p2));

  // Minting the same reference again reuses p1
  const auto headBefore = at;
  at                    = store.makeExternRef(
      at,
      ExternOpRef{.scroll = scrollId, .produces = MicroversionId::parse("1")});
  EXPECT_EQ(at, headBefore);
}

TEST(ExternRefTest, manyPlaceholdersDoNotDisplaceTheScrollRegistryRank) {
  const auto perma = std::make_shared<UserPermascroll>();
  Store store(perma);
  auto at = store.sliceGenesis(MicroversionId{});

  const std::string keyA = "btpk:" + std::string(64, '1') + ":docA";
  const std::string keyB = "btpk:" + std::string(64, '2') + ":docB";
  const std::string keyC = "btpk:" + std::string(64, '3') + ":docC";

  at = store.registerScroll(at, keyA);
  at = store.registerScroll(at, keyB);
  at = store.registerScroll(at, keyC);

  const auto scrollIdB = *store.scrollRegistry().scrollIdForKey(keyB);

  // Add 5 placeholders to scroll B
  for (int i = 1; i <= 5; ++i) {
    at = store.makeExternRef(
        at, ExternOpRef{.scroll   = scrollIdB,
                        .produces = MicroversionId::parse(std::to_string(i))});
  }

  const auto manifold   = store.rebuildManifold(at);
  const auto dimScrolls = manifold.dimensionNamed("d.scrolls", store);
  ASSERT_TRUE(dimScrolls.has_value());

  const auto visitedScrolls =
      zigzag::rankAfter(manifold, manifold.home(), *dimScrolls) |
      std::ranges::to<std::vector>();

  ASSERT_EQ(visitedScrolls.size(), 3U);
  EXPECT_EQ(manifold.textOf(visitedScrolls[0], store), keyA);
  EXPECT_EQ(manifold.textOf(visitedScrolls[1], store), keyB);
  EXPECT_EQ(manifold.textOf(visitedScrolls[2], store), keyC);
}

TEST(ExternRefTest, globalDocumentStateCodecRoundTrip) {
  const GlobalDocumentState state1{
      .scroll  = "btpk:" + std::string(64, 'a') + ":test",
      .version = MicroversionId::parse("2a4b12"),
  };

  const auto encoded1 = writeGlobalDocumentState(state1);
  const auto decoded1 = readGlobalDocumentState(encoded1);
  ASSERT_TRUE(decoded1.has_value());
  EXPECT_EQ(*decoded1, state1);

  // Test with state 0
  const GlobalDocumentState stateZero{
      .scroll  = "local_scroll",
      .version = MicroversionId{},
  };
  const auto encodedZero = writeGlobalDocumentState(stateZero);
  const auto decodedZero = readGlobalDocumentState(encodedZero);
  ASSERT_TRUE(decodedZero.has_value());
  EXPECT_EQ(*decodedZero, stateZero);

  // Corrupted version byte
  auto corrupted = encoded1;
  corrupted[0]   = '\x99';
  EXPECT_FALSE(readGlobalDocumentState(corrupted).has_value());

  // Truncated data
  EXPECT_FALSE(readGlobalDocumentState(encoded1.substr(0, encoded1.size() - 2))
                   .has_value());

  // Trailing garbage
  auto trailingGarbage = encoded1;
  trailingGarbage.push_back('x');
  EXPECT_FALSE(readGlobalDocumentState(trailingGarbage).has_value());

  // Empty string
  EXPECT_FALSE(readGlobalDocumentState("").has_value());
}

} // namespace
