/**
 * @file views.hpp
 * @brief Document presentation and views coordinator for Xudu.
 */
#ifndef XUDU_VIEWS_HPP
#define XUDU_VIEWS_HPP

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
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
#include "common/xanadu/framing.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/reading_place.hpp"
#include "common/xanadu/spool.hpp"
#include "common/xanadu/swarm_catalog.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"
#include "xudu/kinetic_tether_overlay.hpp"
#include "xudu/pouch_drawer.hpp"
#include "xudu/session.hpp"
#include "xudu/wireframe_hull.hpp"

namespace xudu {

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
    std::vector<xudu::PrimediaSpan> spans;
    std::vector<xudu::PrimediaSpan> right;
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
  float readableTextPx_{xudu::LayoutConfig{}.readableTextPx};
  float chromeTopPx_{};
  bool readingFramed_{false};
  std::weak_ptr<Doc> frameTarget_;
  std::weak_ptr<Doc> presentationAnchor_;
  std::function<bool()> pendingCamera_;
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

} // namespace xudu

#endif // XUDU_VIEWS_HPP
