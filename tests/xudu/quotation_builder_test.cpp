/**
 * @file quotation_builder_test.cpp
 * @brief Unit tests for QuotationBuilder model and preview engine (§5.10).
 */
#include <gtest/gtest.h>

#include "common/xanadu/quotation_builder.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"

using namespace xanadu;

namespace {

struct TestStore {
  std::shared_ptr<UserPermascroll> scroll;
  std::unique_ptr<Store> store;
  MicroversionId head;
  std::string scrollKey;

  explicit TestStore(std::string key)
      : scroll(std::make_shared<UserPermascroll>()),
        store(std::make_unique<Store>(scroll)), scrollKey(std::move(key)) {
    head = store->sliceGenesis(MicroversionId{});
  }

  zigzag::DimRef makeDim(const std::string_view name) {
    const auto m = store->makeDimension(head, name);
    head         = m.version;
    return m.dim;
  }
};

TEST(QuotationBuilderTest, ConfigureForeignStoreAndRootCell) {
  TestStore alice("btpk:aaaa:alice");
  const auto dimVarsA = alice.makeDim("d.vars");

  auto atA         = alice.head;
  atA              = alice.store->makeCell(atA, "SettingName");
  const auto sName = alice.store->cellRefOf(atA);
  atA        = alice.store->setLink(atA, alice.store->homeCell(), dimVarsA,
                                    zigzag::DimVector::POS, sName);
  alice.head = atA;

  QuotationBuilder builder;
  builder.setForeignStore(alice.scrollKey, alice.store.get());

  EXPECT_EQ(builder.config().foreignScrollKey, alice.scrollKey);
  EXPECT_EQ(builder.config().pinnedVersion, alice.head);
  EXPECT_EQ(builder.config().rootRef.produces,
            alice.store->segmentedOps().idOf(alice.store->homeCell()));
  EXPECT_EQ(builder.config().rootLabel, "home");

  builder.setRootCell(sName);
  EXPECT_EQ(builder.config().rootRef.produces,
            alice.store->segmentedOps().idOf(sName));
  EXPECT_EQ(builder.config().rootLabel, "SettingName");
}

TEST(QuotationBuilderTest, PreviewRankMode) {
  TestStore alice("btpk:aaaa:alice");
  const auto dimR = alice.makeDim("d.items");

  auto atA      = alice.head;
  atA           = alice.store->makeCell(atA, "item1");
  const auto c1 = alice.store->cellRefOf(atA);
  atA           = alice.store->makeCell(atA, "item2");
  const auto c2 = alice.store->cellRefOf(atA);
  atA           = alice.store->makeCell(atA, "item3");
  const auto c3 = alice.store->cellRefOf(atA);

  atA        = alice.store->setLink(atA, c1, dimR, zigzag::DimVector::POS, c2);
  atA        = alice.store->setLink(atA, c2, dimR, zigzag::DimVector::POS, c3);
  alice.head = atA;

  QuotationBuilder builder;
  builder.setForeignStore(alice.scrollKey, alice.store.get());
  builder.setRootCell(c1);
  builder.setMode(Selector::Kind::Rank);

  const ExternOpRef dimRef{
      .scroll   = 1,
      .produces = alice.store->segmentedOps().idOf(dimR),
  };
  builder.setRankStep(dimRef, zigzag::DimVector::POS);

  builder.recomputePreview();

  const auto &prev = builder.preview();
  EXPECT_TRUE(prev.isValid);
  EXPECT_EQ(prev.state, QuotationState::Resolved);
  ASSERT_EQ(prev.cells.size(), 3U);

  EXPECT_EQ(prev.cells[0].foreignCell, c1);
  EXPECT_EQ(prev.cells[0].text, "item1");
  ASSERT_EQ(prev.cells[0].outboundEdges.size(), 1U);
  EXPECT_EQ(prev.cells[0].outboundEdges[0].first, "d.items");
  EXPECT_EQ(prev.cells[0].outboundEdges[0].second, c2);

  EXPECT_EQ(prev.cells[1].foreignCell, c2);
  EXPECT_EQ(prev.cells[1].text, "item2");

  EXPECT_EQ(prev.cells[2].foreignCell, c3);
  EXPECT_EQ(prev.cells[2].text, "item3");
  EXPECT_TRUE(prev.cells[2].outboundEdges.empty());
}

TEST(QuotationBuilderTest, PreviewClosureMode) {
  TestStore alice("btpk:aaaa:alice");
  const auto dimVars = alice.makeDim("d.vars");
  const auto dimVals = alice.makeDim("d.values");

  auto atA         = alice.head;
  atA              = alice.store->makeCell(atA, "tab_size");
  const auto sName = alice.store->cellRefOf(atA);
  atA              = alice.store->makeCell(atA, "4");
  const auto sVal  = alice.store->cellRefOf(atA);

  atA = alice.store->setLink(atA, sName, dimVals, zigzag::DimVector::POS, sVal);
  alice.head = atA;

  QuotationBuilder builder;
  builder.setForeignStore(alice.scrollKey, alice.store.get());
  builder.setRootCell(sName);
  builder.setMode(Selector::Kind::Closure);

  const ExternOpRef varsRef{
      .scroll   = 1,
      .produces = alice.store->segmentedOps().idOf(dimVars),
  };
  const ExternOpRef valsRef{
      .scroll   = 1,
      .produces = alice.store->segmentedOps().idOf(dimVals),
  };
  builder.setCarriedDimensions({varsRef, valsRef});

  builder.recomputePreview();

  const auto &prev = builder.preview();
  EXPECT_TRUE(prev.isValid);
  EXPECT_EQ(prev.state, QuotationState::Resolved);
  ASSERT_EQ(prev.cells.size(), 2U);

  EXPECT_EQ(prev.cells[0].foreignCell, sName);
  EXPECT_EQ(prev.cells[0].text, "tab_size");
  ASSERT_EQ(prev.cells[0].outboundEdges.size(), 1U);
  EXPECT_EQ(prev.cells[0].outboundEdges[0].first, "d.values");
  EXPECT_EQ(prev.cells[0].outboundEdges[0].second, sVal);

  EXPECT_EQ(prev.cells[1].foreignCell, sVal);
  EXPECT_EQ(prev.cells[1].text, "4");
}

TEST(QuotationBuilderTest, PreviewVqlQueryMode) {
  TestStore alice("btpk:aaaa:alice");
  const auto dimVars = alice.makeDim("d.vars");

  auto atA      = alice.head;
  atA           = alice.store->makeCell(atA, "alpha");
  const auto cA = alice.store->cellRefOf(atA);
  atA           = alice.store->makeCell(atA, "beta");
  const auto cB = alice.store->cellRefOf(atA);

  atA = alice.store->setLink(atA, alice.store->homeCell(), dimVars,
                             zigzag::DimVector::POS, cA);
  atA = alice.store->setLink(atA, cA, dimVars, zigzag::DimVector::POS, cB);
  alice.head = atA;

  QuotationBuilder builder;
  builder.setForeignStore(alice.scrollKey, alice.store.get());
  builder.setMode(Selector::Kind::Query);
  builder.setVqlQuery("##/d.vars");

  builder.recomputePreview();

  const auto &prev = builder.preview();
  EXPECT_TRUE(prev.isValid);
  EXPECT_EQ(prev.state, QuotationState::Resolved);
  EXPECT_GE(prev.cells.size(), 2U);
}

TEST(QuotationBuilderTest, PreviewNavigation) {
  TestStore alice("btpk:aaaa:alice");
  const auto dimR = alice.makeDim("d.items");

  auto atA      = alice.head;
  atA           = alice.store->makeCell(atA, "1");
  const auto c1 = alice.store->cellRefOf(atA);
  atA           = alice.store->makeCell(atA, "2");
  const auto c2 = alice.store->cellRefOf(atA);
  atA           = alice.store->makeCell(atA, "3");
  const auto c3 = alice.store->cellRefOf(atA);

  atA        = alice.store->setLink(atA, c1, dimR, zigzag::DimVector::POS, c2);
  atA        = alice.store->setLink(atA, c2, dimR, zigzag::DimVector::POS, c3);
  alice.head = atA;

  QuotationBuilder builder;
  builder.setForeignStore(alice.scrollKey, alice.store.get());
  builder.setRootCell(c1);
  builder.setMode(Selector::Kind::Rank);
  builder.setRankStep(ExternOpRef{
      .scroll = 1, .produces = alice.store->segmentedOps().idOf(dimR)});
  builder.recomputePreview();

  EXPECT_EQ(builder.preview().focusedIndex, 0U);

  builder.navigatePreview(1);
  EXPECT_EQ(builder.preview().focusedIndex, 1U);

  builder.navigatePreview(1);
  EXPECT_EQ(builder.preview().focusedIndex, 2U);

  // Wraps around
  builder.navigatePreview(1);
  EXPECT_EQ(builder.preview().focusedIndex, 0U);

  // Negative wraparound
  builder.navigatePreview(-1);
  EXPECT_EQ(builder.preview().focusedIndex, 2U);

  builder.focusPreviewCell(1);
  EXPECT_EQ(builder.preview().focusedIndex, 1U);
}

TEST(QuotationBuilderTest, CommitQuotationAppliesToLocalStore) {
  TestStore alice("btpk:aaaa:alice");
  const auto dimA = alice.makeDim("d.items");

  auto atA      = alice.head;
  atA           = alice.store->makeCell(atA, "target_cell");
  const auto cA = alice.store->cellRefOf(atA);
  alice.head    = atA;

  TestStore bob("btpk:bbbb:bob");
  bob.head       = bob.store->registerScroll(bob.head, alice.scrollKey);
  const auto sId = *bob.store->scrollRegistry().scrollIdForKey(alice.scrollKey);

  const auto dimLocal = bob.makeDim("d.my_quotes");

  QuotationBuilder builder;
  builder.setForeignStore(alice.scrollKey, alice.store.get());
  builder.setRootRef(
      ExternOpRef{
          .scroll   = sId,
          .produces = alice.store->segmentedOps().idOf(cA),
      },
      "target_cell");
  builder.setMode(Selector::Kind::Cell);
  builder.setQuotationLabel("AdoptedTarget");

  const auto q =
      builder.commit(*bob.store, bob.head, bob.store->homeCell(), dimLocal);
  bob.head = q.version;

  EXPECT_NE(q.quotationCell, zigzag::noCell);
  EXPECT_NE(q.placeholderCell, zigzag::noCell);
  EXPECT_NE(q.stateCell, zigzag::noCell);
  EXPECT_NE(q.selectorCell, zigzag::noCell);

  const auto bobFold = bob.store->rebuildManifold(bob.head);
  EXPECT_EQ(bobFold.textOf(q.quotationCell, *bob.store), "AdoptedTarget");
  EXPECT_EQ(
      bobFold.linked(bob.store->homeCell(), dimLocal, zigzag::DimVector::POS),
      q.quotationCell);
}

} // namespace
