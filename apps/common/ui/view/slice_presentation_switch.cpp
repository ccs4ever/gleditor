/**
 * @file slice_presentation_switch.cpp
 * @brief Several slice presentations behind one seam, one of them shown.
 */
#include "slice_presentation_switch.hpp"

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <utility>

#include <gleditor/logging.hpp>

#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"

namespace xanadu::view {

namespace {

/// A switch moves the revisions it forwards by this much, far above anything
/// one presentation's counters reach, so a switch is always a change even to
/// a reader of a presentation whose own count is lower.
constexpr unsigned kSwitchShift = 40U;

} // namespace

/// The renderer's three roles, forwarded to the presentation shown. Device
/// setup goes to every one, since any of them may be shown later and a
/// contributor added after the render thread starts is never set up.
class SlicePresentationSwitch::Roles final : public gleditor::FrameContributor,
                                             public gleditor::PickObserver,
                                             public gleditor::a11y::Source {
public:
  explicit Roles(SlicePresentationSwitch &owner) : owner_(owner) {}

  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override {
    for (const auto &entry : owner_.entries_) {
      entry.presentation->frameContributor()->deviceReady(device,
                                                          documentPipeline);
    }
  }
  void drawScene(gleditor::FrameContext &ctx) override {
    owner_.shown().frameContributor()->drawScene(ctx);
  }
  void drawFrame(gleditor::FrameContext &ctx) override {
    owner_.shown().frameContributor()->drawFrame(ctx);
  }
  [[nodiscard]] bool busy() const override {
    return owner_.shown().frameContributor()->busy();
  }
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override {
    return owner_.shown().pickObserver()->picked(pick, state);
  }
  void describe(gleditor::a11y::Builder &into) override {
    owner_.shown().accessibilitySource()->describe(into);
  }
  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    return (owner_.switches_ << kSwitchShift) +
           owner_.shown().accessibilitySource()->accessibilityRevision();
  }
  bool performAction(const std::uint64_t nodeId,
                     const gleditor::a11y::Action action,
                     const std::string_view value) override {
    return owner_.shown().accessibilitySource()->performAction(nodeId, action,
                                                               value);
  }

private:
  SlicePresentationSwitch &owner_;
};

SlicePresentationSwitch::SlicePresentationSwitch(
    std::shared_ptr<SlicePresentation> legacy)
    : roles_(std::make_unique<Roles>(*this)) {
  if (!legacy) {
    throw std::invalid_argument("a slice presentation switch needs a legacy "
                                "presentation");
  }
  entries_.push_back(
      Entry{.kind         = std::string(settings::kSlicePresentationLegacy),
            .presentation = std::move(legacy)});
}

SlicePresentationSwitch::~SlicePresentationSwitch() = default;

bool SlicePresentationSwitch::add(
    std::string kind, std::shared_ptr<SlicePresentation> presentation,
    Adopt adopt) {
  if (!presentation || std::ranges::contains(entries_, kind, &Entry::kind)) {
    return false;
  }
  presentation->setPresentationVisible(false);
  if (auto *const legacyStore = entries_.front().presentation->store()) {
    presentation->bindXuduStore(*legacyStore,
                                entries_.front().presentation->sliceHead());
  }
  entries_.push_back(Entry{.kind         = std::move(kind),
                           .presentation = std::move(presentation),
                           .adopt        = std::move(adopt)});
  return true;
}

bool SlicePresentationSwitch::show(const std::string_view kind) {
  const auto found = std::ranges::find(entries_, kind, &Entry::kind);
  if (found == entries_.end()) {
    return false;
  }
  const auto next = static_cast<std::size_t>(found - entries_.begin());
  if (next == shown_) {
    return true;
  }
  auto &leaving = *entries_[shown_].presentation;
  auto &coming  = *found->presentation;
  flushFocus();
  // The reader keeps their place: the same slice, the same cell, the same
  // axes, the same link marked.
  if (auto *const slice = leaving.store()) {
    coming.bindXuduStore(*slice, leaving.sliceHead());
  }
  if (found->adopt) {
    found->adopt(coming, leaving);
  }
  if (const auto cell = leaving.focusCell(); zigzag::noCell != cell) {
    coming.focusCell(cell);
  }
  coming.setCellHighlights(highlights_, highlightBorder_);
  coming.setPreviewCell(preview_);
  leaving.setPreviewCell(std::nullopt);
  leaving.setPresentationVisible(false);
  coming.setPresentationVisible(visible_);
  shown_ = next;
  ++switches_;
  if (invalidation_) {
    invalidation_(bridgeRevision());
  }
  return true;
}

SlicePresentationSwitch *
SlicePresentationSwitch::setFocusLog(std::shared_ptr<SliceFocusLog> log) {
  focusLog_ = std::move(log);
  return this;
}

SlicePresentationSwitch *SlicePresentationSwitch::flushFocus() {
  auto &now = shown();
  if (const auto *const slice = now.store(); focusLog_ && nullptr != slice) {
    if (const auto cell = now.focusCell(); zigzag::noCell != cell) {
      if (const auto kept = focusLog_->keep(slice->documentId(), cell); !kept) {
        GLEDITOR_LOG_WARN("xuzz.view", "the focus could not be kept: {}",
                          kept.error());
      }
    }
  }
  return this;
}

std::string_view SlicePresentationSwitch::shownKind() const noexcept {
  return entries_[shown_].kind;
}

SlicePresentation &SlicePresentationSwitch::shown() const noexcept {
  return *entries_[shown_].presentation;
}

std::vector<std::string> SlicePresentationSwitch::kinds() const {
  std::vector<std::string> out;
  for (const auto &entry : entries_) {
    out.push_back(entry.kind);
  }
  return out;
}

gleditor::FrameContributor *
SlicePresentationSwitch::frameContributor() noexcept {
  return roles_.get();
}
gleditor::PickObserver *SlicePresentationSwitch::pickObserver() noexcept {
  return roles_.get();
}
gleditor::a11y::Source *
SlicePresentationSwitch::accessibilitySource() noexcept {
  return roles_.get();
}
gleditor::ui::FocusScope *SlicePresentationSwitch::focusScope() noexcept {
  return entries_.front().presentation->focusScope();
}

SlicePresentationSwitch *
SlicePresentationSwitch::setPresentationVisible(const bool visible) {
  visible_ = visible;
  shown().setPresentationVisible(visible);
  return this;
}

SlicePresentationSwitch *
SlicePresentationSwitch::setPresentationOrigin(const glm::vec3 origin) {
  for (const auto &entry : entries_) {
    entry.presentation->setPresentationOrigin(origin);
  }
  return this;
}

SlicePresentationSwitch *
SlicePresentationSwitch::setPresentationTransformResolver(
    PresentationTransformResolver resolver) {
  for (const auto &entry : entries_) {
    entry.presentation->setPresentationTransformResolver(resolver);
  }
  return this;
}

SlicePresentationSwitch *SlicePresentationSwitch::setPresentationConfig(
    ZigzagPresentationConfig config) {
  for (const auto &entry : entries_) {
    entry.presentation->setPresentationConfig(config);
  }
  return this;
}

const ZigzagPresentationConfig &
SlicePresentationSwitch::presentationConfig() const noexcept {
  return shown().presentationConfig();
}

void SlicePresentationSwitch::bindXuduStore(Store &store,
                                            const MicroversionId &version) {
  // Every presentation follows the slice; one not shown folds it only when
  // it is shown.
  const auto *const before = entries_.front().presentation->store();
  if (before != &store) {
    flushFocus();
  }
  for (const auto &entry : entries_) {
    entry.presentation->bindXuduStore(store, version);
  }
  // A slice newly opened starts where the reader left it. The slice views
  // read that themselves; the visualizer does not, and the seam would carry
  // its home over a kept focus on the first switch.
  if (focusLog_ && before != &store) {
    auto &front = *entries_.front().presentation;
    if (const auto kept = focusLog_->last(store.documentId());
        kept && front.manifold().contains(*kept)) {
      front.focusCell(*kept);
    }
  }
}

Store *SlicePresentationSwitch::store() const noexcept {
  return shown().store();
}
MicroversionId SlicePresentationSwitch::sliceHead() const {
  return shown().sliceHead();
}
const zigzag::ViewAxisBinding &SlicePresentationSwitch::currentView() const {
  return shown().currentView();
}

SlicePresentationSwitch *SlicePresentationSwitch::setPreviewCell(
    const std::optional<zigzag::CellRef> cell) {
  preview_ = cell;
  shown().setPreviewCell(cell);
  return this;
}

SlicePresentationSwitch *
SlicePresentationSwitch::setExternInspector(ExternInspector inspector) {
  for (const auto &entry : entries_) {
    entry.presentation->setExternInspector(inspector);
  }
  return this;
}

SlicePresentationSwitch *
SlicePresentationSwitch::setHasKeyboard(const bool has) noexcept {
  for (const auto &entry : entries_) {
    entry.presentation->setHasKeyboard(has);
  }
  return this;
}

SlicePresentationSwitch *
SlicePresentationSwitch::setKeyHints(std::string here, std::string elsewhere) {
  entries_.front().presentation->setKeyHints(std::move(here),
                                             std::move(elsewhere));
  return this;
}

SlicePresentationSwitch *
SlicePresentationSwitch::setViewKeyHints(std::string here,
                                         std::string elsewhere) {
  for (std::size_t i = 1; i < entries_.size(); ++i) {
    entries_[i].presentation->setKeyHints(here, elsewhere);
  }
  return this;
}

bool SlicePresentationSwitch::dispatchAction(
    const std::string_view actionName) {
  return shown().dispatchAction(actionName);
}

bool SlicePresentationSwitch::insertConnectedTransclusion(
    const std::span<const PrimediaSpan> spans) {
  return shown().insertConnectedTransclusion(spans);
}

const zigzag::Manifold &SlicePresentationSwitch::manifold() const noexcept {
  return shown().manifold();
}
zigzag::CellRef SlicePresentationSwitch::focusCell() const noexcept {
  return shown().focusCell();
}
void SlicePresentationSwitch::focusCell(const zigzag::CellRef cell) {
  shown().focusCell(cell);
}
void SlicePresentationSwitch::activateCell(const zigzag::CellRef cell) {
  shown().activateCell(cell);
}
void SlicePresentationSwitch::setCellActivationCallback(
    CellActivationCallback callback) {
  for (const auto &entry : entries_) {
    entry.presentation->setCellActivationCallback(callback);
  }
}
int SlicePresentationSwitch::cellRadius() const noexcept {
  return shown().cellRadius();
}
void SlicePresentationSwitch::setCellRadius(const int radius) noexcept {
  for (const auto &entry : entries_) {
    entry.presentation->setCellRadius(radius);
  }
}
bool SlicePresentationSwitch::isCellLocked(
    const zigzag::CellRef cell) const noexcept {
  return shown().isCellLocked(cell);
}
std::optional<TranscopyrightDescriptor> SlicePresentationSwitch::cellRoyalty(
    const zigzag::CellRef cell) const noexcept {
  return shown().cellRoyalty(cell);
}
bool SlicePresentationSwitch::unlockCell(const zigzag::CellRef cell) {
  return shown().unlockCell(cell);
}
std::optional<CellAnchor>
SlicePresentationSwitch::cellAnchor(const zigzag::CellRef cell) const {
  return shown().cellAnchor(cell);
}
std::optional<glm::vec3> SlicePresentationSwitch::focusCentre() const {
  return shown().focusCentre();
}
void SlicePresentationSwitch::setCellHighlights(
    std::vector<CellHighlight> highlights, const std::uint32_t borderColour) {
  highlights_      = highlights;
  highlightBorder_ = borderColour;
  shown().setCellHighlights(std::move(highlights), borderColour);
}
std::uint64_t SlicePresentationSwitch::bridgeRevision() const noexcept {
  return (switches_ << kSwitchShift) + shown().bridgeRevision();
}
void SlicePresentationSwitch::setBridgeInvalidationCallback(
    InvalidationCallback callback) {
  invalidation_ = callback;
  for (const auto &entry : entries_) {
    entry.presentation->setBridgeInvalidationCallback(
        callback ? InvalidationCallback{[this](std::uint64_t) {
          if (invalidation_) {
            invalidation_(bridgeRevision());
          }
        }}
                 : InvalidationCallback{});
  }
}

} // namespace xanadu::view
