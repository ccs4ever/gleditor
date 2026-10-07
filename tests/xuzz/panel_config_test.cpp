#include <gtest/gtest.h>

#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"

TEST(PanelConfigTest, DefaultsAndLiveLimitsComeFromSystemUi) {
  xanadu::Store store;
  xanadu::initializeSystemStore(store, xanadu::SystemDocKind::UI);
  auto config = xanadu::UIConfig::fromStore(store);
  EXPECT_EQ(config.pouchPanel, xanadu::PouchPanelConfig{});
  EXPECT_EQ(config.storePanel, xanadu::StorePanelConfig{});
  auto head = store.primaryCurrentVersion();
  head = xanadu::setSetting(store, head, xanadu::settings::kPouchPanelWidthPx,
                            420.);
  head = xanadu::setSetting(store, head,
                            xanadu::settings::kPouchPanelMaxWidthShare, .75);
  head = xanadu::setSetting(store, head,
                            xanadu::settings::kPouchPanelMaxHeightShare, .8);
  head = xanadu::setSetting(store, head, xanadu::settings::kStorePanelWidthPx,
                            480.);
  head = xanadu::setSetting(store, head,
                            xanadu::settings::kStorePanelMaxWidthShare, 8.);
  head = xanadu::setSetting(store, head,
                            xanadu::settings::kStorePanelMaxHeightShare, .01);
  store.repointCurrentVersion(head);
  config = xanadu::UIConfig::fromStore(store);
  EXPECT_FLOAT_EQ(config.pouchPanel.widthPx, 420.F);
  EXPECT_FLOAT_EQ(config.pouchPanel.maxWidthShare, .75F);
  EXPECT_FLOAT_EQ(config.pouchPanel.maxHeightShare, .8F);
  EXPECT_FLOAT_EQ(config.storePanel.widthPx, 480.F);
  EXPECT_FLOAT_EQ(config.storePanel.maxWidthShare, 1.F);
  EXPECT_FLOAT_EQ(config.storePanel.maxHeightShare, .1F);
  head = xanadu::setSetting(store, head, xanadu::settings::kPouchPanelWidthPx,
                            -1.);
  store.repointCurrentVersion(head);
  EXPECT_FLOAT_EQ(xanadu::UIConfig::fromStore(store).pouchPanel.widthPx,
                  xanadu::PouchPanelConfig{}.widthPx);
}

TEST(PanelConfigTest, bigModalGeometryIsSeededAndValidatedInSystemUi) {
  xanadu::Store store;
  xanadu::initializeSystemStore(store, xanadu::SystemDocKind::UI);
  const auto defaults = xanadu::UIConfig{};
  auto config         = xanadu::UIConfig::fromStore(store);
  EXPECT_EQ(config.quotationModal, defaults.quotationModal);
  EXPECT_EQ(config.telescopeModal, defaults.telescopeModal);
  EXPECT_EQ(config.hypertimeModal, defaults.hypertimeModal);
  auto head = store.primaryCurrentVersion();
  head      = xanadu::setSetting(store, head,
                                 xanadu::settings::kQuotationModalWidthPx, 900.);
  head      = xanadu::setSetting(store, head,
                                 xanadu::settings::kTelescopeModalHeightPx, 700.);
  head      = xanadu::setSetting(store, head,
                                 xanadu::settings::kHypertimeModalMaxWidthShare, 8.);
  head      = xanadu::setSetting(
      store, head, xanadu::settings::kHypertimeModalMaxHeightShare, .01);
  head = xanadu::setSetting(store, head,
                            xanadu::settings::kQuotationModalHeightPx, -1.);
  store.repointCurrentVersion(head);
  config = xanadu::UIConfig::fromStore(store);
  EXPECT_FLOAT_EQ(config.quotationModal.widthPx, 900.F);
  EXPECT_FLOAT_EQ(config.quotationModal.heightPx,
                  defaults.quotationModal.heightPx);
  EXPECT_FLOAT_EQ(config.telescopeModal.heightPx, 700.F);
  EXPECT_FLOAT_EQ(config.hypertimeModal.maxWidthShare, 1.F);
  EXPECT_FLOAT_EQ(config.hypertimeModal.maxHeightShare, .1F);
}
