/**
 * @file apps/common/ui/xanadoc/branch_convergence_overlay.cpp
 * @brief Implementation of Branch Convergence Overlay UI coordinating dual 3D
 *        document layout, comparative optical beams, and Convergence Action
 *        Palette.
 */
#include "branch_convergence_overlay.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/sha.h>

#include <gleditor/doc.hpp>
#include <gleditor/logging.hpp>
#include <gleditor/render_state.hpp>

#include "beams.hpp"
#include "common/ui/hypertime_graph.hpp"
#include "common/xanadu/session_container.hpp"
#include "session.hpp"
#include "views.hpp"

namespace xanadu {

namespace {
[[nodiscard]] std::string toHex(std::span<const std::uint8_t> data) {
  static constexpr char hexChars[] = "0123456789abcdef";
  std::string s;
  s.reserve(data.size() * 2);
  for (const auto b : data) {
    s.push_back(hexChars[(b >> 4) & 0x0F]);
    s.push_back(hexChars[b & 0x0F]);
  }
  return s;
}

[[nodiscard]] std::string computeSha256Hex(std::span<const std::uint8_t> data) {
  std::array<std::uint8_t, 32> hash{};
  SHA256(data.data(), data.size(), hash.data());
  return toHex(hash);
}
} // namespace

BranchConvergenceOverlay::BranchConvergenceOverlay()
    : BranchConvergenceOverlay(nullptr, {}) {}

BranchConvergenceOverlay::BranchConvergenceOverlay(Store *store,
                                                   std::string fontName)
    : store_(store), fontName_(std::move(fontName)) {
  gleditor::ui::Widget bannerWidget{
      .id    = kIdBannerPanel,
      .model = gleditor::ui::Panel{.title = "Concurrent Edit Detected"}};
  bannerOverlay_ = std::make_unique<gleditor::ui::ScreenOverlay>(bannerWidget);
  bannerOverlay_->setVisible(false);
  bannerOverlay_->setActionHandler(
      [this](const gleditor::ui::WidgetAction &action) {
        (void)dispatchAction(action.action);
      });

  gleditor::ui::Widget paletteWidget{
      .id    = kIdPalettePanel,
      .model = gleditor::ui::Panel{.title = "Convergence Action Palette"}};
  paletteOverlay_ =
      std::make_unique<gleditor::ui::ScreenOverlay>(paletteWidget);
  paletteOverlay_->setVisible(false);
  paletteOverlay_->setActionHandler(
      [this](const gleditor::ui::WidgetAction &action) {
        (void)dispatchAction(action.action);
      });
}

BranchConvergenceOverlay::~BranchConvergenceOverlay() {
  if (!packagePassphrase_.empty()) {
    OPENSSL_cleanse(packagePassphrase_.data(), packagePassphrase_.size());
  }
}

BranchConvergenceOverlay *
BranchConvergenceOverlay::setStore(Store *store) noexcept {
  const std::scoped_lock lock(guard_);
  store_ = store;
  return this;
}

BranchConvergenceOverlay *
BranchConvergenceOverlay::setViews(Views *views) noexcept {
  const std::scoped_lock lock(guard_);
  views_ = views;
  return this;
}

BranchConvergenceOverlay *BranchConvergenceOverlay::setHypertimeGraph(
    ui::HypertimeGraph *graph) noexcept {
  const std::scoped_lock lock(guard_);
  hypertimeGraph_ = graph;
  return this;
}

BranchConvergenceOverlay *
BranchConvergenceOverlay::setLinkBeams(LinkBeams *beams) noexcept {
  const std::scoped_lock lock(guard_);
  beams_ = beams;
  return this;
}

BranchConvergenceOverlay *
BranchConvergenceOverlay::setSession(Session *session) noexcept {
  const std::scoped_lock lock(guard_);
  session_ = session;
  return this;
}

BranchConvergenceOverlay *
BranchConvergenceOverlay::setRenderer(RendererRef renderer) noexcept {
  const std::scoped_lock lock(guard_);
  renderer_ = std::move(renderer);
  return this;
}

BranchConvergenceOverlay *
BranchConvergenceOverlay::setDeviceId(std::string_view deviceId) {
  const std::scoped_lock lock(guard_);
  deviceId_ = std::string(deviceId);
  return this;
}

BranchConvergenceOverlay *
BranchConvergenceOverlay::setActiveHead(const MicroversionId &activeHead) {
  const std::scoped_lock lock(guard_);
  activeHead_ = activeHead;
  return this;
}

BranchConvergenceOverlay *
BranchConvergenceOverlay::setPeerHead(const MicroversionId &peerHead) {
  const std::scoped_lock lock(guard_);
  peerHead_ = peerHead;
  return this;
}

BranchConvergenceOverlay *
BranchConvergenceOverlay::setBannerVisible(bool visible) noexcept {
  const std::scoped_lock lock(guard_);
  bannerVisible_ = visible;
  if (bannerOverlay_) {
    bannerOverlay_->setVisible(visible);
  }
  return this;
}

BranchConvergenceOverlay *
BranchConvergenceOverlay::setPaletteVisible(bool visible) noexcept {
  const std::scoped_lock lock(guard_);
  paletteVisible_ = visible;
  if (paletteOverlay_) {
    paletteOverlay_->setVisible(visible);
  }
  return this;
}

BranchConvergenceOverlay *
BranchConvergenceOverlay::setConvergenceActive(bool active) noexcept {
  const std::scoped_lock lock(guard_);
  convergenceActive_ = active;
  return this;
}

BranchConvergenceOverlay *
BranchConvergenceOverlay::setEncryptPackage(bool enabled) noexcept {
  const std::scoped_lock lock(guard_);
  encryptPackage_ = enabled;
  return this;
}

BranchConvergenceOverlay *BranchConvergenceOverlay::setPackagePassphrase(
    std::string passphrase) noexcept {
  const std::scoped_lock lock(guard_);
  if (!packagePassphrase_.empty()) {
    OPENSSL_cleanse(packagePassphrase_.data(), packagePassphrase_.size());
  }
  packagePassphrase_ = std::move(passphrase);
  return this;
}

BranchConvergenceOverlay *BranchConvergenceOverlay::detectConcurrentEdits(
    std::string_view deviceId, const MicroversionId &branchVersion,
    const MicroversionId &activeHead) {
  const std::scoped_lock lock(guard_);
  deviceId_      = std::string(deviceId);
  peerHead_      = branchVersion;
  activeHead_    = activeHead;
  bannerVisible_ = true;
  if (bannerOverlay_) {
    bannerOverlay_->setVisible(true);
  }
  ensureDiffUpToDate();
  return this;
}

std::string BranchConvergenceOverlay::bannerText() const {
  return std::format("Concurrent edit from device '{}' on branch {}", deviceId_,
                     peerHead_.str());
}

std::string BranchConvergenceOverlay::fullBannerNotification() const {
  return std::format("A concurrent edit from device '{}' on branch {} was "
                     "detected against active head {}. Click 'Review & "
                     "Converge' to inspect and reconcile.",
                     deviceId_, peerHead_.str(), activeHead_.str());
}

void BranchConvergenceOverlay::ensureDiffUpToDate() {
  if (!store_ || activeHead_.isZero() || peerHead_.isZero()) {
    diffResult_.reset();
    return;
  }
  diffResult_ = store_->diffVersions(activeHead_, peerHead_);
}

std::expected<void, std::string> BranchConvergenceOverlay::reviewAndConverge() {
  const std::scoped_lock lock(guard_);
  convergenceActive_ = true;
  bannerVisible_     = false;
  if (bannerOverlay_) {
    bannerOverlay_->setVisible(false);
  }
  paletteVisible_ = true;
  if (paletteOverlay_) {
    paletteOverlay_->setVisible(true);
  }
  ensureDiffUpToDate();
  return {};
}

std::expected<MicroversionId, std::string>
BranchConvergenceOverlay::adoptSelectedSpan(
    std::optional<PrimediaSpan> span, std::optional<std::uint32_t> targetPos) {
  const std::scoped_lock lock(guard_);
  if (!store_) {
    return std::unexpected("No store available for span adoption");
  }
  if (!span) {
    return activeHead_;
  }
  if (span->start > UINT32_MAX || span->length > UINT32_MAX) {
    return std::unexpected("Span start or length exceeds 32-bit limits");
  }
  const auto newHead =
      store_->transclude(activeHead_, targetPos.value_or(0), peerHead_,
                         static_cast<std::uint32_t>(span->start),
                         static_cast<std::uint32_t>(span->length));
  activeHead_ = newHead;
  ensureDiffUpToDate();
  return newHead;
}

std::expected<void, std::string>
BranchConvergenceOverlay::retainBothAsNamedEditions(
    std::string_view activeAlias, std::string_view peerAlias) {
  const std::scoped_lock lock(guard_);
  if (!store_) {
    return std::unexpected("No store available");
  }
  if (!activeAlias.empty() && !activeHead_.isZero()) {
    store_->setVersionAnnotation(
        activeHead_, VersionAnnotation{.alias = std::string(activeAlias)});
  }
  if (!peerAlias.empty() && !peerHead_.isZero()) {
    store_->setVersionAnnotation(
        peerHead_, VersionAnnotation{.alias = std::string(peerAlias)});
  }
  return {};
}

std::expected<MicroversionId, std::string>
BranchConvergenceOverlay::synthesizeNewEdition(std::string_view editionName) {
  const std::scoped_lock lock(guard_);
  if (!store_) {
    return std::unexpected("No store available for edition synthesis");
  }
  const auto newHead = store_->designateEdition(
      activeHead_, editionName.empty() ? "unified" : editionName, peerHead_);
  activeHead_        = newHead;
  convergenceActive_ = false;
  paletteVisible_    = false;
  if (paletteOverlay_) {
    paletteOverlay_->setVisible(false);
  }
  return newHead;
}

std::expected<void, std::string>
BranchConvergenceOverlay::dispatchAction(std::string_view action) {
  if (action == "review_and_converge") {
    return reviewAndConverge();
  }
  if (action == "dismiss_banner") {
    setBannerVisible(false);
    return {};
  }
  if (action == "adopt_selected_span") {
    auto res = adoptSelectedSpan();
    if (!res) {
      return std::unexpected(res.error());
    }
    return {};
  }
  if (action == "retain_editions") {
    return retainBothAsNamedEditions("main", "peer");
  }
  if (action == "synthesize_edition") {
    auto res = synthesizeNewEdition();
    if (!res) {
      return std::unexpected(res.error());
    }
    return {};
  }
  if (action == "dismiss_palette") {
    setPaletteVisible(false);
    setConvergenceActive(false);
    return {};
  }
  if (action == "toggle_encrypt") {
    setEncryptPackage(!isEncryptPackage());
    return {};
  }
  if (action == "export_package") {
    if (!store_) {
      return std::unexpected("No store configured for package export");
    }
    const auto &userScroll = store_->userPermascroll();
    const auto &config     = userScroll.config();
    const std::string effectiveDeviceId =
        (!deviceId_.empty() && deviceId_ != "main") ? deviceId_ : "peer";

    std::string masterFp = std::string(config.masterIdentity.view());
    if (masterFp.empty() || masterFp.size() != 64) {
      masterFp = computeSha256Hex(config.deviceKeys.publicKey.bytes);
    }

    auto certRes = createX509DelegationCertificate(
        config.deviceKeys.publicKey.bytes,
        std::span<const std::uint8_t, 32>(
            config.deviceKeys.secretKey.bytes.data(), 32),
        effectiveDeviceId);
    if (!certRes) {
      return std::unexpected(
          std::string(validationErrorToString(certRes.error())));
    }

    std::vector<CompactOpNode> ops;
    ops.reserve(store_->opCount());
    for (std::uint32_t i = 1; i <= store_->opCount(); ++i) {
      if (const auto *node = store_->getCompactOp(i)) {
        ops.push_back(*node);
      }
    }

    const std::string primediaStr = std::string(userScroll.bytes());

    SessionPackage pkg;
    pkg.setManifestVersion(1)
        ->setMasterFingerprint(masterFp)
        ->setDeviceId(effectiveDeviceId)
        ->setDeviceKey(toHex(config.deviceKeys.publicKey.bytes))
        ->setBaseVersion(!activeHead_.isZero() ? activeHead_.str() : "0")
        ->setHeadVersion(!peerHead_.isZero() ? peerHead_.str() : "0")
        ->setPrimediaOffset(0)
        ->setPrimediaLength(primediaStr.size())
        ->setTimestamp(static_cast<std::uint64_t>(std::time(nullptr)))
        ->setPrimediaSlice(std::string_view(primediaStr))
        ->setOpsNodes(ops)
        ->setDeviceCert(*certRes)
        ->setEncrypted(encryptPackage_)
        ->setPassphrase(packagePassphrase_);

    auto signRes = pkg.signWithDeviceKey(std::span<const std::uint8_t, 32>(
        config.deviceKeys.secretKey.bytes.data(), 32));
    if (!signRes) {
      return std::unexpected(
          std::string(validationErrorToString(signRes.error())));
    }

    const auto exportPath = std::filesystem::path("convergence_export.xuzzpkg");
    auto expRes           = exportPackage(pkg, exportPath);
    if (!expRes) {
      return std::unexpected(
          std::string(validationErrorToString(expRes.error())));
    }
    return {};
  }
  return std::unexpected(std::format("Unknown convergence action: {}", action));
}

void BranchConvergenceOverlay::registerVortexActions(
    zigzag::vortex::VortexHost &, Store *) {
  // Actions registered via system://keymap
}

BranchConvergenceOverlay *
BranchConvergenceOverlay::bindVortexHost(zigzag::vortex::VortexHost &vHost,
                                         Store *keymapStore) {
  registerVortexActions(vHost, keymapStore);
  return this;
}

std::shared_ptr<const gleditor::ui::WidgetScene>
BranchConvergenceOverlay::prepare(const gleditor::ui::UiMetrics &metrics,
                                  const gleditor::ui::Theme &theme) {
  const std::scoped_lock lock(guard_);
  std::shared_ptr<const gleditor::ui::WidgetScene> bScene;
  std::shared_ptr<const gleditor::ui::WidgetScene> pScene;
  if (bannerVisible_ && bannerOverlay_) {
    rebuildBannerModel(metrics);
    bScene = bannerOverlay_->prepare(metrics, theme);
  }
  if (paletteVisible_ && paletteOverlay_) {
    rebuildPaletteModel(metrics);
    pScene = paletteOverlay_->prepare(metrics, theme);
  }
  if (pScene) {
    return pScene;
  }
  return bScene;
}

std::shared_ptr<const gleditor::ui::WidgetScene>
BranchConvergenceOverlay::bannerScene() const {
  const std::scoped_lock lock(guard_);
  return bannerOverlay_ ? bannerOverlay_->snapshot() : nullptr;
}

std::shared_ptr<const gleditor::ui::WidgetScene>
BranchConvergenceOverlay::paletteScene() const {
  const std::scoped_lock lock(guard_);
  return paletteOverlay_ ? paletteOverlay_->snapshot() : nullptr;
}

void BranchConvergenceOverlay::deviceReady(
    render::RenderDevice &device, const render::PipelineDesc &pipeline) {
  const std::scoped_lock lock(guard_);
  if (bannerOverlay_) {
    bannerOverlay_->deviceReady(device, pipeline);
  }
  if (paletteOverlay_) {
    paletteOverlay_->deviceReady(device, pipeline);
  }
}

void BranchConvergenceOverlay::drawFrame(gleditor::FrameContext &ctx) {
  const std::scoped_lock lock(guard_);
  if (bannerVisible_ && bannerOverlay_) {
    bannerOverlay_->drawFrame(ctx);
  }
  if (paletteVisible_ && paletteOverlay_) {
    paletteOverlay_->drawFrame(ctx);
  }
}

bool BranchConvergenceOverlay::busy() const { return false; }

bool BranchConvergenceOverlay::picked(const render::PickingResult &pick,
                                      RenderState &state) {
  const std::scoped_lock lock(guard_);
  if (paletteVisible_ && paletteOverlay_ &&
      paletteOverlay_->picked(pick, state)) {
    return true;
  }
  if (bannerVisible_ && bannerOverlay_ && bannerOverlay_->picked(pick, state)) {
    return true;
  }
  return false;
}

void BranchConvergenceOverlay::decorate(const Doc &,
                                        std::vector<gleditor::SpanStyle> &) {
  // Spatial spans decorated when convergence view is active
}

void BranchConvergenceOverlay::describe(gleditor::a11y::Builder &into) {
  const std::scoped_lock lock(guard_);
  if (paletteVisible_ && paletteOverlay_) {
    paletteOverlay_->describe(into);
  }
  if (bannerVisible_ && bannerOverlay_) {
    bannerOverlay_->describe(into);
  }
}

std::uint64_t BranchConvergenceOverlay::accessibilityRevision() const {
  const std::scoped_lock lock(guard_);
  return revision_;
}

bool BranchConvergenceOverlay::performAction(std::uint64_t id,
                                             gleditor::a11y::Action action,
                                             std::string_view value) {
  const std::scoped_lock lock(guard_);
  if (paletteVisible_ && paletteOverlay_ &&
      paletteOverlay_->performAction(id, action, value)) {
    return true;
  }
  if (bannerVisible_ && bannerOverlay_ &&
      bannerOverlay_->performAction(id, action, value)) {
    return true;
  }
  return false;
}

void BranchConvergenceOverlay::rebuildBannerModel(
    const gleditor::ui::UiMetrics &) {
  if (!bannerOverlay_) return;
  gleditor::ui::Widget banner{
      .id    = kIdBannerPanel,
      .model = gleditor::ui::Panel{.title = "Concurrent Edit Detected"}};

  gleditor::ui::Widget label{
      .id    = kIdBannerLabel,
      .model = gleditor::ui::Label{
          .text = bannerText(), .purpose = gleditor::ui::TextPurpose::Label}};

  gleditor::ui::Widget reviewBtn{
      .id    = kIdBannerReviewBtn,
      .model = gleditor::ui::Button{
          .text            = "Review & Converge",
          .action          = "review_and_converge",
          .accessibleLabel = "Review and converge concurrent edits"}};

  gleditor::ui::Widget dismissBtn{
      .id    = kIdBannerDismissBtn,
      .model = gleditor::ui::Button{.text            = "Dismiss",
                                    .action          = "dismiss_banner",
                                    .accessibleLabel = "Dismiss notification"}};

  banner.children.push_back(std::move(label));
  banner.children.push_back(std::move(reviewBtn));
  banner.children.push_back(std::move(dismissBtn));

  bannerOverlay_->setModel(std::move(banner));
}

void BranchConvergenceOverlay::rebuildPaletteModel(
    const gleditor::ui::UiMetrics &) {
  if (!paletteOverlay_) return;
  gleditor::ui::Widget palette{
      .id    = kIdPalettePanel,
      .model = gleditor::ui::Panel{.title = "Convergence Action Palette"}};

  gleditor::ui::Widget title{.id    = kIdPaletteTitle,
                             .model = gleditor::ui::Label{
                                 .text    = "Structured Convergence Actions",
                                 .purpose = gleditor::ui::TextPurpose::Title}};

  gleditor::ui::Widget adoptBtn{
      .id    = kIdPaletteAdoptBtn,
      .model = gleditor::ui::Button{
          .text            = "Adopt Selected Span (Ctrl+Shift+A)",
          .action          = "adopt_selected_span",
          .accessibleLabel = "Adopt selected span into active head"}};

  gleditor::ui::Widget retainBtn{
      .id    = kIdPaletteRetainBtn,
      .model = gleditor::ui::Button{
          .text            = "Retain Both Editions (Ctrl+Shift+E)",
          .action          = "retain_editions",
          .accessibleLabel = "Retain both branches as named editions"}};

  gleditor::ui::Widget synthBtn{
      .id    = kIdPaletteSynthBtn,
      .model = gleditor::ui::Button{
          .text            = "Synthesize New Edition (Ctrl+Shift+S)",
          .action          = "synthesize_edition",
          .accessibleLabel = "Synthesize new unified edition"}};

  gleditor::ui::Widget encryptCheckbox{
      .id    = kIdPaletteEncryptCheckbox,
      .model = gleditor::ui::Button{
          .text            = encryptPackage_ ? "[x] Encrypt package (.xuzzpkg)"
                                             : "[ ] Encrypt package (.xuzzpkg)",
          .action          = "toggle_encrypt",
          .accessibleLabel = "Toggle package encryption"}};

  gleditor::ui::Widget exportBtn{
      .id = kIdPaletteExportPackageBtn,
      .model =
          gleditor::ui::Button{.text            = "Export Package (.xuzzpkg)",
                               .action          = "export_package",
                               .accessibleLabel = "Export session package"}};

  gleditor::ui::Widget dismissBtn{
      .id    = kIdPaletteDismiss,
      .model = gleditor::ui::Button{.text            = "Close",
                                    .action          = "dismiss_palette",
                                    .accessibleLabel = "Close palette"}};

  palette.children.push_back(std::move(title));
  palette.children.push_back(std::move(adoptBtn));
  palette.children.push_back(std::move(retainBtn));
  palette.children.push_back(std::move(synthBtn));
  palette.children.push_back(std::move(encryptCheckbox));
  palette.children.push_back(std::move(exportBtn));
  palette.children.push_back(std::move(dismissBtn));

  paletteOverlay_->setModel(std::move(palette));
}

} // namespace xanadu
