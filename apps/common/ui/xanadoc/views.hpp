/**
 * @file views.hpp
 * @brief Document presentation and views coordinator for Xudu.
 */
#ifndef XUDU_VIEWS_HPP
#define XUDU_VIEWS_HPP

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include <gleditor/caret.hpp>
#include <gleditor/caret_motion.hpp>
#include <gleditor/doc_switcher.hpp>
#include <gleditor/form.hpp>
#include <gleditor/media_widget.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/state.hpp>

#include "common/ui/hypertime_graph.hpp"
#include "common/ui/view/page_dim.hpp"
#include "common/ui/xanadoc/kinetic_tether_overlay.hpp"
#include "common/ui/xanadoc/pouch_drawer.hpp"
#include "common/ui/xanadoc/session.hpp"
#include "common/ui/xanadoc/wireframe_hull.hpp"
#include "common/xanadu/framing.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/reading_place.hpp"
#include "common/xanadu/spool.hpp"
#include "common/xanadu/swarm_catalog.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu {

using HypertimeMap = xanadu::ui::HypertimeGraph;

struct OnionSkinPolicy {
  glm::vec3 offsetPerVersion{18.0F, 14.0F, -10.0F};
  float minimumOpacity{0.20F};
  float opacityStep{0.20F};
};

inline constexpr OnionSkinPolicy kDefaultOnionSkinPolicy{};

class Views : public gleditor::FrameContributor {
public:
  Views(Session &aSession, RendererRef aRenderer, HypertimeMap &aMap,
        ImageOverlay &aImages, gleditor::Form &aForm, AppStateRef aState,
        std::shared_ptr<gleditor::DocumentSwitcher> aSwitcher);
  ~Views() override;

  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  /// Whether the camera is still being placed: moved on the last frame it
  /// drew -- which that frame was drawn without -- or waiting for a page to be
  /// built to place it by. What lets a capture or a profiled run wait for a
  /// frame the camera has finished moving for.
  [[nodiscard]] bool busy() const override { return cameraSettling_; }

  void cancelReadingFrame() {
    frameTarget_.reset();
    readingFramed_ = true;
  }

  void setReadableTextPx(const float px) noexcept { readableTextPx_ = px; }
  void frameForReading(const gleditor::FrameContext &ctx);
  void placeCameraWhenReady(std::function<bool()> place) {
    pendingCamera_ = std::move(place);
  }
  void keepInView(const Doc &doc, std::uint32_t offset);
  void setComparisonCameraReady(std::function<bool()> ready) {
    comparisonCameraReady_ = std::move(ready);
  }
  void frameNewestComparison();

  [[nodiscard]] std::optional<Doc::Anchor>
  widgetRectFor(const Doc &doc, std::uint32_t docOffset) const;

  void showOnly(const MicroversionId &version, std::size_t storeIndex = 0);
  void showAlongside(const MicroversionId &version, float depthZ = 0.0F,
                     std::size_t storeIndex     = 0,
                     std::uint32_t focusedBirth = 0);

  void syncMediaWidgets(RenderState &rState);

  [[nodiscard]] std::optional<glm::mat4> presentationTransform() const;

  struct Where {
    std::uint32_t doc{};
    std::uint32_t start{};
    std::uint32_t end{};
    bool hasRange{};
  };

  template <typename Fun> void withCaret(Fun fun) {
    renderer->runWithState([this, fun](RenderState &rState) {
      auto *const caret = renderer->editCaret();
      if (nullptr == caret || !caret->active() ||
          caret->documentIndex() >= rState.docs.size()) {
        return;
      }
      session.flushUncommitted(caret->documentIndex());
      Where where{.doc      = caret->documentIndex(),
                  .start    = caret->byteOffset(),
                  .end      = caret->byteOffset(),
                  .hasRange = caret->hasSelection()};
      if (where.hasRange) {
        where.start = caret->selectionStart();
        where.end   = caret->selectionEnd();
      }
      fun(rState, where, caret);
    });
  }

  void swingBackToSpan(const PouchItem &item);
  void focusSpan(zigzag::CellRef cell, const PrimediaSpan &span);
  void focusSpan(std::size_t docIndex, std::uint32_t charStart,
                 std::uint32_t charEnd);
  void focusContent(std::vector<PrimediaSpan> content);

  void back();
  void forward();
  void scrubHistory(bool backward);

  void deleteSelection();
  void deleteForward();
  void moveCaret(gleditor::CaretMotion motion, bool extend);

  void transcludeSelection();
  void linkSelection();
  void cancelLink();

  void addCellToPendingLink(std::span<const PrimediaSpan> content);
  void finishCellLink();

  void publishCurrent(const std::string &salt);
  void publicationStatus();
  void publishAnswers(const MicroversionId &version, std::uint32_t which,
                      std::size_t storeIdx,
                      const std::vector<gleditor::Form::Field> &answers);

  void saveCurrent();
  void preserveAnswers(std::size_t storeIdx,
                       const std::vector<gleditor::Form::Field> &answers);

  void closeDocument(std::uint32_t docIndex);
  void closeActive();
  void activateNewest();
  void activateDocument(RenderState &rState, std::uint32_t index);
  std::size_t newDocument();

  [[nodiscard]] xanadu::ReadingPlace currentPlace() const;
  void restorePlace(const xanadu::ReadingPlace &place,
                    std::vector<std::optional<std::uint32_t>> opened,
                    std::vector<std::uint32_t> lengths);
  void keepFinalPlace() { finalPlace_ = currentPlace(); }
  [[nodiscard]] xanadu::ReadingPlace finalPlace() const {
    return finalPlace_ ? *finalPlace_ : currentPlace();
  }

  /// While @p on, the pages keep the share of their own opacity that
  /// view.page.dim says (setPageDimShare), so a slice drawn over them is the
  /// brightest thing in the frame. Any thread.
  void dimPages(const bool on) noexcept {
    pagesDimmed_.store(on ? PageDimming::Alone : PageDimming::Off);
  }
  /// The slice shares the frame with the pages, which are read too: they keep
  /// the larger share view.page.dimUnified says (setPageDimUnifiedShare), so
  /// the slice's labels no longer stand on a white page (M2 gate 2, D5).
  void dimPagesBeside() noexcept { pagesDimmed_.store(PageDimming::Beside); }
  void setPageDimShare(const float share) noexcept {
    pageDimShare_.store(share);
  }
  void setPageDimUnifiedShare(const float share) noexcept {
    pageDimUnifiedShare_.store(share);
  }
  /// How far above the top edge of the page a slice that is not placed beside
  /// it keeps its focus row, in logical pixels (view.page.clearancePx). Any
  /// thread.
  void setPlacementClearancePx(const float px) noexcept {
    clearancePx_.store(px);
  }
  /// Where a slice presented on its own, not beside a page, is placed: the
  /// world's origin, lifted by the clearance so the page's top edge does not
  /// cut through the row the reader is on. The page's own scale turns the
  /// logical clearance into world units; with no page there is nothing to
  /// clear.
  [[nodiscard]] std::optional<glm::mat4> soloPlacement() const;

private:
  [[nodiscard]] glm::vec3 cameraPosition() const;

public:
  void anchorPresentation(std::weak_ptr<Doc> doc) {
    presentationAnchor_ = std::move(doc);
  }

  void insertSpanAtCaret(const PrimediaSpan &span);
  void transcludeSpansAtCaret(std::vector<PrimediaSpan> spans);
  void insertSpanAt(RenderState &rState, std::uint32_t doc, std::uint32_t at,
                    const PrimediaSpan &span);

  void spawnTranscludedDocument(const TetherPayload &payload,
                                float screenX = 0.0F, float screenY = 0.0F);
  void summonPublication(const PublicationEntry &entry);
  void publicationDownloadStatus(const std::string &id);
  void publishIndependentLinks();
  void linksAndResponses(const std::string &query = {});
  void linkPackageStatus(const std::string &id,
                         std::optional<Publication> source = {});
  void inspectIndependentLinkKeys(const std::string &id, std::size_t link,
                                  std::size_t left, std::size_t right);
  void reviewIndependentLinks(const std::string &id, std::size_t selected = 0);
  std::function<void(const LinkKey &)> selectIndependentLink;
  std::function<void()> packageVisibilityChanged;
  std::optional<Publication> packageFilterSource;
  std::map<std::string, Publication> packageQuerySources;
  void publicationUpdates(const std::string &subscription = {},
                          std::int64_t sequence           = -1);
  void discoverPublications(const std::string &query = {});
  void publicationDiscoveryStatus(const std::string &id);
  void setPublicationCatalog(SwarmCatalog *catalog) {
    publicationCatalog_ = catalog;
  }

  void insertPageBreak(std::uint32_t docIndex, std::uint32_t charOffset);
  void insertPageBreakAtCaret();

  void exportOsmic();
  void importFile(const std::string &filePath);

  void setWireframeOverlay(WireframeHullOverlay *overlay) noexcept;

  void unlockTranscopyright(std::size_t storeIdx, const PrimediaSpan &span);
  void unlockTranscopyrightAtCaret();

  void openDocumentPalette();
  void openDocumentFromPath(const std::string &chosen);

  void selectDoc(std::uint32_t index);
  void nextDoc();
  void prevDoc();
  void printHistory();

  [[nodiscard]] bool onionSkinMode() const noexcept { return onionSkinMode_; }
  void setOnionSkin(bool enabled);
  void toggleOnionSkin();
  void arrangeOnionSkin(RenderState &rState);
  void arrangeAlongside(RenderState &rState);
  void cycleOnionSkin(int delta);

  [[nodiscard]] Session &sessionRef() noexcept { return session; }
  [[nodiscard]] const Session &sessionRef() const noexcept { return session; }

private:
  struct Pending {
    std::uint32_t doc{};
    std::uint32_t start{};
    std::uint32_t end{};
    std::vector<xanadu::PrimediaSpan> spans;
    std::vector<xanadu::PrimediaSpan> right;
    std::size_t rightCells{};
  };

  std::chrono::steady_clock::time_point nextPublicationNotice_{};
  bool publicationNoticeError_{};
  Session &session;
  RendererRef renderer;
  HypertimeMap &map;
  ImageOverlay &images;
  gleditor::Form &form;
  AppStateRef state;
  std::shared_ptr<gleditor::DocumentSwitcher> switcher;
  std::weak_ptr<Doc> primaryDocument_;
  float readableTextPx_{xanadu::LayoutConfig{}.readableTextPx};
  float chromeTopPx_{};
  bool readingFramed_{false};
  std::weak_ptr<Doc> frameTarget_;
  std::weak_ptr<Doc> presentationAnchor_;
  enum class PageDimming : std::uint8_t { Off, Alone, Beside };
  std::atomic<PageDimming> pagesDimmed_{PageDimming::Off};
  std::atomic<float> pageDimShare_{1.0F};
  std::atomic<float> pageDimUnifiedShare_{1.0F};
  std::atomic<float> clearancePx_{0.0F};
  xanadu::view::PageDim pageDim_;
  std::function<bool()> pendingCamera_;
  bool cameraSettling_{};
  /// The camera is to be framed on a page that is not built yet.
  bool framingWaiting_{};
  int cameraSettlingFrames_{};
  std::function<bool()> comparisonCameraReady_;
  std::optional<xanadu::ReadingPlace> finalPlace_;
  std::optional<Pending> pending;
  std::vector<std::shared_ptr<gleditor::MediaWidget>> mediaWidgets;
  bool onionSkinMode_{false};
  std::size_t activeOnionIdx_{0};

  render::RenderDevice *device_{nullptr};
  render::PipelineDesc documentDesc_;
  WireframeHullOverlay *wireframeOverlay_{nullptr};
  SwarmCatalog *publicationCatalog_{};
};

} // namespace xanadu

#endif // XUDU_VIEWS_HPP
