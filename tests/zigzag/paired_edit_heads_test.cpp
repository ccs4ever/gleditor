#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include "common/ui/slice/zigzag_visualizer.hpp"
#include "common/ui/xanadoc/session.hpp"

namespace {

TEST(PairedEditHeadsTest, InterleavedTextAndCellsKeepOneAncestry) {
  auto perma = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Session session("", perma);
  auto &store     = session.store();
  const auto base = store.sliceGenesis({});
  session.viewOpened(base);
  session.viewOpened(base);
  zigzag::ZigzagVisualizer slice("Sans 12");
  slice.bindXuduStore(store, base);
  std::vector<std::pair<xanadu::MicroversionId, xanadu::MicroversionId>>
      commits;
  session.setAuthoredCommitHandler([&](const std::uint32_t doc,
                                       const std::size_t index,
                                       const xanadu::MicroversionId &parent,
                                       const xanadu::MicroversionId &head) {
    ASSERT_EQ(doc, 0U);
    ASSERT_EQ(index, 0U);
    EXPECT_TRUE(slice.advanceEditingHead(parent, head));
    commits.emplace_back(parent, head);
  });
  slice.setAuthoredCommitHandler([&](const xanadu::MicroversionId &parent,
                                     const xanadu::MicroversionId &head) {
    EXPECT_TRUE(session.advanceViewAfterCommit(0, parent, head));
    commits.emplace_back(parent, head);
  });

  const auto firstText = session.insertText(0, 0, "Document A");
  EXPECT_EQ(slice.sliceHead(), firstText);
  const auto cell      = slice.engine()->addCell("Cell A");
  const auto firstCell = slice.sliceHead();
  EXPECT_EQ(session.views()[0].version, firstCell);
  slice.focusCell(cell);
  slice.swapDimensions(0, 1);
  const auto axis       = slice.currentView().x_dimension;
  const auto secondText = session.insertText(0, 10, " and B");
  EXPECT_EQ(slice.sliceHead(), secondText);
  EXPECT_EQ(slice.focusCell(), cell);
  EXPECT_EQ(slice.currentView().x_dimension, axis);
  slice.engine()->updateCellText(cell, "Cell B");
  const auto finalHead = slice.sliceHead();
  EXPECT_EQ(session.views()[0].version, finalHead);
  EXPECT_EQ(store.textOf(finalHead), "Document A and B");
  EXPECT_EQ(slice.manifold().textOf(cell, store), "Cell B");
  EXPECT_EQ(session.views()[1].version, base);
  EXPECT_FALSE(session.advanceViewAfterCommit(1, secondText, finalHead));
  ASSERT_EQ(commits.size(), 4U);
  EXPECT_EQ(commits.front().first, base);
  for (std::size_t i = 0; i < commits.size(); ++i) {
    EXPECT_TRUE(commits[i].first.isAncestorOf(commits[i].second));
    if (i != 0) EXPECT_EQ(commits[i].first, commits[i - 1].second);
  }
  EXPECT_TRUE(firstText.isAncestorOf(finalHead));
  EXPECT_TRUE(firstCell.isAncestorOf(finalHead));

  const auto ops = store.opCount();
  session.viewClosed(1);
  session.viewOpened(firstText);
  slice.engine()->syncTo(firstText);
  EXPECT_EQ(store.opCount(), ops);
  EXPECT_EQ(commits.size(), 4U);
}

TEST(PairedEditHeadsTest, HistoricSliceIsNeverMovedByAnUnrelatedCommit) {
  auto perma = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Session session("", perma);
  auto &store         = session.store();
  const auto base     = store.sliceGenesis({});
  const auto selected = store.insert(base, 0, "Selected branch");
  const auto sibling  = store.insert(base, 0, "Other branch");
  session.viewOpened(selected);
  zigzag::ZigzagVisualizer slice("Sans 12");
  slice.bindXuduStore(store, selected);
  const auto ops = store.opCount();
  EXPECT_FALSE(slice.advanceEditingHead(base, sibling));
  EXPECT_FALSE(slice.advanceEditingHead(selected, sibling));
  EXPECT_FALSE(session.advanceViewAfterCommit(0, selected, sibling));
  EXPECT_EQ(slice.sliceHead(), selected);
  EXPECT_EQ(session.views()[0].version, selected);
  EXPECT_EQ(store.opCount(), ops);

  const auto descendant = store.insert(selected, 15, " edited");
  EXPECT_TRUE(slice.advanceEditingHead(selected, descendant));
  EXPECT_TRUE(session.advanceViewAfterCommit(0, selected, descendant));
  EXPECT_EQ(store.textOf(slice.sliceHead()), "Selected branch edited");
}

} // namespace
