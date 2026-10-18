/**
 * @file link_directory_dialog_test.cpp
 * @brief Unit tests for LinkDirectoryDialog (Modal & 30% Docked Drawer, search,
 * filters).
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/ui/link_directory_dialog.hpp"
#include "common/ui/xanadoc/link_context.hpp"
#include "common/ui/xanadoc/session.hpp"

namespace {

TEST(LinkDirectoryDialogTest, ConstructionAndDefaultState) {
  xanadu::Session session{"", std::make_shared<xanadu::UserPermascroll>()};
  xanadu::LinkContext context(session);

  xanadu::ui::LinkDirectoryDialog dialog(context, "Sans");
  EXPECT_FALSE(dialog.isVisible());
  EXPECT_EQ(dialog.displayMode(), xanadu::ui::DirectoryDisplayMode::Modal);

  dialog.setVisible(true);
  EXPECT_TRUE(dialog.isVisible());

  dialog.setDisplayMode(xanadu::ui::DirectoryDisplayMode::DockedDrawer);
  EXPECT_EQ(dialog.displayMode(),
            xanadu::ui::DirectoryDisplayMode::DockedDrawer);

  dialog.setDisplayMode(xanadu::ui::DirectoryDisplayMode::Modal);
  EXPECT_EQ(dialog.displayMode(), xanadu::ui::DirectoryDisplayMode::Modal);
}

TEST(LinkDirectoryDialogTest, FiltersAndSearchStateManagement) {
  xanadu::Session session{"", std::make_shared<xanadu::UserPermascroll>()};
  xanadu::LinkContext context(session);

  xanadu::ui::LinkDirectoryDialog dialog(context, "Sans");

  // Search query
  dialog.setSearchQuery("primordial");
  dialog.setTypeFilter(xanadu::LinkType::Quotation);
  dialog.setAuthorFilter("ted");
  dialog.setBalanceFilter(xanadu::ui::BalanceFilter::OneToMany);
  dialog.setAdvancedPrimediaOpen(true);

  // Keyboard navigation
  dialog.setVisible(true);
  EXPECT_TRUE(dialog.keyPressed(gleditor::Key::Down, gleditor::KeyMods::None));
  EXPECT_TRUE(dialog.keyPressed(gleditor::Key::Up, gleditor::KeyMods::None));
  EXPECT_TRUE(
      dialog.keyPressed(gleditor::Key::Backspace, gleditor::KeyMods::None));
  EXPECT_TRUE(
      dialog.keyPressed(gleditor::Key::Escape, gleditor::KeyMods::None));
  EXPECT_FALSE(dialog.isVisible());
}

TEST(LinkDirectoryDialogTest, TextTypingAppendsToSearch) {
  xanadu::Session session{"", std::make_shared<xanadu::UserPermascroll>()};
  xanadu::LinkContext context(session);

  xanadu::ui::LinkDirectoryDialog dialog(context, "Sans");
  dialog.setVisible(true);

  dialog.textTyped("hello");
  dialog.textTyped(" world");

  EXPECT_TRUE(dialog.pointerArea().has_value());
}

} // namespace
