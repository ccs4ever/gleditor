/**
 * @file tests/zigzag/branch_convergence_overlay_test.cpp
 * @brief Unit tests for BranchConvergenceOverlay UI coordinating branch
 *        detection, side-by-side inspection, convergence actions, and package
 * export.
 */
#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../lib/mocks/device.hpp"
#include <gleditor/a11y/tree.hpp>
#include <gleditor/render_state.hpp>

#include "common/ui/xanadoc/branch_convergence_overlay.hpp"
#include "common/xanadu/session_container.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace xanadu {
namespace {

namespace fs = std::filesystem;

class BranchConvergenceOverlayTest : public ::testing::Test {
protected:
  void SetUp() override {
    testDir_ = fs::current_path() / "build" / "test_convergence_overlay";
    fs::remove_all(testDir_);
    fs::create_directories(testDir_);

    UserPermascroll::Config cfg;
    cfg.storageDir   = testDir_ / "permascroll";
    cfg.deviceId     = "laptop_alpha";
    userPermascroll_ = std::make_shared<UserPermascroll>(std::move(cfg));

    store_ = std::make_unique<Store>(userPermascroll_);

    // Base document
    v0_ = store_->insert(MicroversionId{}, 0,
                         "Initial shared thesis paragraph.\n");

    // Branch A
    vA_ = store_->insert(v0_, 32, "Branch A additions on architecture.\n");

    // Branch B (concurrent edit from device B)
    vB_ = store_->insert(v0_, 32, "Branch B additions on cryptography.\n");

    store_->setCurrentVersions({vA_, vB_});
  }

  void TearDown() override {
    fs::remove_all(testDir_);
    fs::remove("convergence_export.xuzzpkg");
  }

  fs::path testDir_;
  std::shared_ptr<UserPermascroll> userPermascroll_;
  std::unique_ptr<Store> store_;
  MicroversionId v0_;
  MicroversionId vA_;
  MicroversionId vB_;
};

TEST_F(BranchConvergenceOverlayTest, AutomaticDetectionOfMultipleHeads) {
  BranchConvergenceOverlay overlay(store_.get());

  EXPECT_FALSE(overlay.isBannerVisible());
  EXPECT_FALSE(overlay.isPaletteVisible());
  EXPECT_FALSE(overlay.isConvergenceActive());

  // Detect concurrent edits automatically using store versions
  overlay.detectConcurrentEdits();

  EXPECT_TRUE(overlay.isBannerVisible());
  EXPECT_FALSE(overlay.isPaletteVisible());
  EXPECT_FALSE(overlay.isConvergenceActive());

  const auto text = overlay.bannerText();
  EXPECT_THAT(text, ::testing::HasSubstr("Concurrent edit"));

  const auto notification = overlay.fullBannerNotification();
  EXPECT_THAT(notification, ::testing::HasSubstr("Review & Converge"));
  EXPECT_TRUE(overlay.diffResult().has_value());
}

TEST_F(BranchConvergenceOverlayTest, ReviewAndConvergeOpensPalette) {
  BranchConvergenceOverlay overlay(store_.get());
  overlay.detectConcurrentEdits();
  ASSERT_TRUE(overlay.isBannerVisible());

  auto res = overlay.reviewAndConverge();
  ASSERT_TRUE(res.has_value());

  EXPECT_FALSE(overlay.isBannerVisible());
  EXPECT_TRUE(overlay.isPaletteVisible());
  EXPECT_TRUE(overlay.isConvergenceActive());
}

TEST_F(BranchConvergenceOverlayTest, AdoptSelectedSpanUpdatesActiveHead) {
  BranchConvergenceOverlay overlay(store_.get());
  overlay.detectConcurrentEdits("device_beta", vB_, vA_);
  ASSERT_TRUE(overlay.reviewAndConverge().has_value());

  const auto initialActive = overlay.activeHead();
  PrimediaSpan spanToAdopt{.scroll = 0, .start = 32, .length = 20};

  auto adoptRes = overlay.adoptSelectedSpan(spanToAdopt, 0);
  ASSERT_TRUE(adoptRes.has_value());
  EXPECT_NE(*adoptRes, initialActive);
  EXPECT_EQ(overlay.activeHead(), *adoptRes);
}

TEST_F(BranchConvergenceOverlayTest, RetainBothAsNamedEditionsAttachesAliases) {
  BranchConvergenceOverlay overlay(store_.get());
  overlay.detectConcurrentEdits("device_beta", vB_, vA_);

  auto retainRes =
      overlay.retainBothAsNamedEditions("edition_active", "edition_peer");
  ASSERT_TRUE(retainRes.has_value());

  const auto annotActive = store_->versionAnnotation(vA_);
  ASSERT_TRUE(annotActive.has_value());
  EXPECT_EQ(annotActive->alias, "edition_active");

  const auto annotPeer = store_->versionAnnotation(vB_);
  ASSERT_TRUE(annotPeer.has_value());
  EXPECT_EQ(annotPeer->alias, "edition_peer");
}

TEST_F(BranchConvergenceOverlayTest, SynthesizeNewEditionClosesPalette) {
  BranchConvergenceOverlay overlay(store_.get());
  overlay.detectConcurrentEdits("device_beta", vB_, vA_);
  ASSERT_TRUE(overlay.reviewAndConverge().has_value());
  ASSERT_TRUE(overlay.isPaletteVisible());

  auto synthRes = overlay.synthesizeNewEdition("unified_master");
  ASSERT_TRUE(synthRes.has_value());

  EXPECT_FALSE(overlay.isPaletteVisible());
  EXPECT_FALSE(overlay.isConvergenceActive());

  const auto manifold = store_->rebuildManifold(overlay.activeHead());
  const auto ed       = manifold.editionNamed("unified_master");
  ASSERT_TRUE(ed.has_value());
}

TEST_F(BranchConvergenceOverlayTest, DispatchActionCoordinatesFlowAndToggles) {
  BranchConvergenceOverlay overlay(store_.get());
  overlay.detectConcurrentEdits("device_beta", vB_, vA_);

  // Toggle encrypt
  EXPECT_FALSE(overlay.isEncryptPackage());
  auto actRes = overlay.dispatchAction("toggle_encrypt");
  EXPECT_TRUE(actRes.has_value());
  EXPECT_TRUE(overlay.isEncryptPackage());

  // Review and converge
  actRes = overlay.dispatchAction("review_and_converge");
  EXPECT_TRUE(actRes.has_value());
  EXPECT_TRUE(overlay.isPaletteVisible());

  // Dismiss palette
  actRes = overlay.dispatchAction("dismiss_palette");
  EXPECT_TRUE(actRes.has_value());
  EXPECT_FALSE(overlay.isPaletteVisible());
  EXPECT_FALSE(overlay.isConvergenceActive());
}

TEST_F(BranchConvergenceOverlayTest,
       ExportPackageFromOverlayWritesValidPackage) {
  BranchConvergenceOverlay overlay(store_.get());
  overlay.detectConcurrentEdits("device_beta", vB_, vA_);
  overlay.setPackagePassphrase("Secr3tToken!");
  overlay.setEncryptPackage(true);

  auto expRes = overlay.dispatchAction("export_package");
  ASSERT_TRUE(expRes.has_value());

  const auto exportPath = fs::path("convergence_export.xuzzpkg");
  ASSERT_TRUE(fs::exists(exportPath));
  EXPECT_GT(fs::file_size(exportPath), 0U);

  // Read back and verify package
  auto impRes = importPackage(exportPath, std::nullopt, true, std::nullopt,
                              "Secr3tToken!");
  ASSERT_TRUE(impRes.has_value());
  EXPECT_TRUE(impRes->isFatPackage());
  EXPECT_TRUE(impRes->encrypted);
}

TEST_F(BranchConvergenceOverlayTest, AccessibilityTreeExposure) {
  BranchConvergenceOverlay overlay(store_.get());
  overlay.detectConcurrentEdits("device_beta", vB_, vA_);

  gleditor::ui::UiMetrics metrics{
      .fontScale = 1.0F, .screenWidth = 1280, .screenHeight = 800};
  gleditor::ui::Theme theme;
  const auto scene = overlay.prepare(metrics, theme);
  ASSERT_NE(scene, nullptr);

  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 100);
  overlay.describe(builder);

  EXPECT_GE(overlay.accessibilityRevision(), 1U);

  // Banner buttons can be clicked via a11y performAction
  bool handled = overlay.performAction(
      builder.id(BranchConvergenceOverlay::kIdBannerDismissBtn),
      gleditor::a11y::Action::Click, {});
  EXPECT_TRUE(handled);
  EXPECT_FALSE(overlay.isBannerVisible());
}

} // namespace
} // namespace xanadu
