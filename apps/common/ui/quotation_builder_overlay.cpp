/**
 * @file quotation_builder_overlay.cpp
 * @brief Implementation of the interactive Quotation Builder Overlay.
 */
#include "common/ui/quotation_builder_overlay.hpp"

#include <algorithm>
#include <format>
#include <ranges>

#include <glm/gtc/matrix_transform.hpp>

namespace xanadu {

namespace {

constexpr std::uint32_t kColorBgOverlay   = 0x0B0F19F5;
constexpr std::uint32_t kColorHeaderBg    = 0x111827FF;
constexpr std::uint32_t kColorBorder      = 0x334155FF;
constexpr std::uint32_t kColorBorderCyan  = 0x38BDF8FF;
constexpr std::uint32_t kColorCanvasBg    = 0x050811FF;
constexpr std::uint32_t kColorActiveTab   = 0x0284C7DD;
constexpr std::uint32_t kColorInactiveTab = 0x1E293B88;
constexpr std::uint32_t kColorTextWhite   = 0xFFFFFFFF;
constexpr std::uint32_t kColorTextMuted   = 0x94A3B8CC;
constexpr std::uint32_t kColorSkyBlue     = 0x38BDF8FF;
constexpr std::uint32_t kColorCellBg      = 0x1E293BCC;
constexpr std::uint32_t kColorCellFocusBg = 0x0369A1CC;
constexpr std::uint32_t kColorEdge        = 0x38BDF888;
constexpr std::uint32_t kColorSuccess     = 0x10B981FF;
constexpr std::uint32_t kColorDanger      = 0xEF4444FF;
constexpr std::uint32_t kColorCommitBtn   = 0x059669EE;

[[maybe_unused]] std::string truncateString(std::string_view str,
                                            const std::size_t maxLen) {
  if (str.size() <= maxLen) {
    return std::string(str);
  }
  return std::string(str.substr(0, maxLen - 3)) + "...";
}

} // namespace

QuotationBuilderOverlay::QuotationBuilderOverlay(
    Store &localStore, MicroversionId activeVersion, RendererRef renderer,
    SwarmCatalog *catalog, std::string fontName, OpenStoresProvider openStores,
    VersionCommitCallback onCommit)
    : localStore_(localStore), activeVersion_(activeVersion),
      renderer_(std::move(renderer)), catalog_(catalog),
      fontName_(std::move(fontName)),
      openStoresProvider_(std::move(openStores)),
      onCommit_(std::move(onCommit)) {
  refreshSources();
}

QuotationBuilderOverlay::~QuotationBuilderOverlay() = default;

void QuotationBuilderOverlay::deviceReady(
    render::RenderDevice &device, const render::PipelineDesc &pipeline) {
  canvas_ = std::make_unique<gleditor::Canvas>(&device, fontName_);
  canvas_->createPipeline(pipeline, false);
}

bool QuotationBuilderOverlay::busy() const { return false; }

void QuotationBuilderOverlay::setVisible(const bool visible) {
  visible_ = visible;
  if (visible_) {
    if (activeVersion_.isZero() &&
        !localStore_.primaryCurrentVersion().isZero()) {
      activeVersion_ = localStore_.primaryCurrentVersion();
    }
    refreshSources();
    recomputePreview();
  }
  ++a11yRevision_;
}

void QuotationBuilderOverlay::toggle() { setVisible(!visible_); }

void QuotationBuilderOverlay::refreshSources() {
  foreignStores_.clear();
  const auto &reg = localStore_.scrollRegistry();
  for (const auto &rec : reg.scrolls) {
    if (!rec.globalKey.empty()) {
      foreignStores_.push_back(rec.globalKey);
    }
  }

  // Also query followed authors from catalog if present
  if (catalog_) {
    for (const auto &author : catalog_->followedAuthors()) {
      if (!author.fingerprint.empty() &&
          !std::ranges::contains(foreignStores_, author.fingerprint)) {
        foreignStores_.push_back(author.fingerprint);
      }
    }
  }

  if (openStoresProvider_) {
    const auto stores = openStoresProvider_();
    for (const auto *st : stores) {
      if (st) {
        const auto docId = st->documentId().str();
        if (!docId.empty() && !std::ranges::contains(foreignStores_, docId)) {
          foreignStores_.push_back(docId);
        }
      }
    }
  }

  if (foreignStores_.empty()) {
    foreignStores_.push_back("local.scroll");
  }

  selectStore(0);
}

void QuotationBuilderOverlay::selectStore(const std::size_t index) {
  if (index >= foreignStores_.size()) {
    return;
  }
  selectedStoreIndex_ = index;
  const auto &key     = foreignStores_[index];

  Store *targetStore = nullptr;
  if (openStoresProvider_) {
    const auto stores = openStoresProvider_();
    for (auto *st : stores) {
      if (st && st->documentId().str() == key) {
        targetStore = st;
        break;
      }
    }
  }
  if (targetStore == nullptr) {
    targetStore = &localStore_;
  }
  selectedTargetStore_ = targetStore;

  builder_.setForeignStore(key, targetStore);

  // Populate candidate roots from the store
  candidateRootCells_.clear();
  const auto home = targetStore->homeCell();
  if (home != zigzag::noCell) {
    candidateRootCells_.emplace_back(home, "home");
  }

  const auto fold = targetStore->rebuildManifold(targetStore->latest());
  for (const auto &c : fold.cells()) {
    if (c.birthOp != home && c.birthOp != fold.dimsDimension()) {
      const auto txt = fold.textOf(c.birthOp, *targetStore);
      candidateRootCells_.emplace_back(
          c.birthOp,
          txt.empty() ? ("cell #" + std::to_string(c.birthOp)) : txt);
      if (candidateRootCells_.size() >= 20U) {
        break;
      }
    }
  }

  if (candidateRootCells_.empty()) {
    candidateRootCells_.emplace_back(zigzag::noCell, "(empty)");
  }

  // Available foreign dimensions
  availableForeignDims_.clear();
  selectedCarriedDims_.clear();
  for (const auto &dimId : fold.dimensions()) {
    auto dimName = fold.textOf(dimId, *targetStore);
    if (dimName.empty()) {
      dimName = "dim #" + std::to_string(dimId);
    }
    availableForeignDims_.emplace_back(dimId, std::move(dimName));
    selectedCarriedDims_.insert(dimId);
  }

  selectRootCell(0);
}

void QuotationBuilderOverlay::selectRootCell(const std::size_t index) {
  if (index >= candidateRootCells_.size()) {
    return;
  }
  selectedRootCellIndex_ = index;
  const auto rootCell    = candidateRootCells_[index].first;
  if (rootCell != zigzag::noCell) {
    builder_.setRootCell(rootCell);
  }
  recomputePreview();
}

void QuotationBuilderOverlay::setMode(const Selector::Kind mode) {
  builder_.setMode(mode);
  recomputePreview();
}

void QuotationBuilderOverlay::toggleCarriedDimension(const zigzag::DimRef dim) {
  if (selectedCarriedDims_.contains(dim)) {
    selectedCarriedDims_.erase(dim);
  } else {
    selectedCarriedDims_.insert(dim);
  }
  recomputePreview();
}

void QuotationBuilderOverlay::setVqlQuery(std::string query) {
  vqlQueryText_ = std::move(query);
  builder_.setVqlQuery(vqlQueryText_);
  recomputePreview();
}

void QuotationBuilderOverlay::recomputePreview() {
  const auto *st = selectedTargetStore_ ? selectedTargetStore_ : &localStore_;
  const auto scrollKey =
      foreignStores_.empty() ? "" : foreignStores_[selectedStoreIndex_];
  const auto scrollId =
      localStore_.scrollRegistry().scrollIdForKey(scrollKey).value_or(1);

  if (builder_.mode() == Selector::Kind::Closure) {
    std::vector<ExternOpRef> dims;
    dims.reserve(selectedCarriedDims_.size());
    for (const auto d : selectedCarriedDims_) {
      dims.push_back(ExternOpRef{
          .scroll   = scrollId,
          .produces = st->segmentedOps().idOf(d),
      });
    }
    builder_.setCarriedDimensions(std::move(dims));
  } else if (builder_.mode() == Selector::Kind::Rank) {
    if (!availableForeignDims_.empty()) {
      const auto dim = availableForeignDims_[selectedRankDimIndex_ %
                                             availableForeignDims_.size()]
                           .first;
      builder_.setRankStep(
          ExternOpRef{
              .scroll   = scrollId,
              .produces = st->segmentedOps().idOf(dim),
          },
          rankDir_);
    }
  } else if (builder_.mode() == Selector::Kind::Query) {
    builder_.setVqlQuery(vqlQueryText_);
  }

  QuotationBudget budget{
      .maxCells   = 200U,
      .maxDepth   = 10U,
      .maxOpBytes = 1024U * 1024U,
      .deadline   = std::chrono::milliseconds(500),
  };
  builder_.recomputePreview(budget);
  ++a11yRevision_;
}

bool QuotationBuilderOverlay::commitQuotation() {
  if (!builder_.preview().isValid) {
    return false;
  }
  builder_.setQuotationLabel(labelText_);
  builder_.setLocalRankDim(localDimName_);

  auto &st        = localStore_;
  auto curVersion = activeVersion_;
  if (curVersion.isZero()) {
    curVersion = st.primaryCurrentVersion();
    if (curVersion.isZero()) {
      curVersion = st.latest();
    }
  }

  const auto fold          = st.rebuildManifold(curVersion);
  auto dimLocal            = fold.dimensionNamed(localDimName_, st);
  zigzag::DimRef targetDim = zigzag::noCell;
  MicroversionId baseVer   = curVersion;

  if (!dimLocal) {
    const auto minted = st.makeDimension(baseVer, localDimName_);
    baseVer           = minted.version;
    targetDim         = minted.dim;
  } else {
    targetDim = *dimLocal;
  }

  zigzag::CellRef localHead = st.homeCell();
  if (localHead == zigzag::noCell) {
    baseVer   = st.makeCell(baseVer, "Quotation Anchor");
    localHead = st.cellRefOf(baseVer);
  }

  const auto q   = builder_.commit(st, baseVer, localHead, targetDim);
  activeVersion_ = q.version;

  if (onCommit_) {
    onCommit_(q.version, q.quotationCell);
  }

  setVisible(false);
  return true;
}

bool QuotationBuilderOverlay::keyPressed(const gleditor::Key key,
                                         const gleditor::KeyMods /*mods*/) {
  if (!visible_) {
    return false;
  }

  if (key == gleditor::Key::Escape) {
    setVisible(false);
    return true;
  }

  if (key == gleditor::Key::Return) {
    if (activeInput_ == ActiveInput::None) {
      return commitQuotation();
    }
    activeInput_ = ActiveInput::None;
    recomputePreview();
    return true;
  }

  // Navigation across preview cells
  if (key == gleditor::Key::Left) {
    builder_.navigatePreview(-1);
    return true;
  }
  if (key == gleditor::Key::Right) {
    builder_.navigatePreview(1);
    return true;
  }

  // Text box editing
  if (activeInput_ != ActiveInput::None) {
    std::string *target = nullptr;
    if (activeInput_ == ActiveInput::Query) {
      target = &vqlQueryText_;
    } else if (activeInput_ == ActiveInput::Label) {
      target = &labelText_;
    } else if (activeInput_ == ActiveInput::LocalDim) {
      target = &localDimName_;
    }

    if (target != nullptr) {
      if (key == gleditor::Key::Backspace && !target->empty()) {
        target->pop_back();
        if (activeInput_ == ActiveInput::Query) {
          builder_.setVqlQuery(vqlQueryText_);
          recomputePreview();
        }
        return true;
      }
    }
  }

  return true; // Consume modal keys
}

void QuotationBuilderOverlay::textTyped(const std::string &utf8) {
  if (!visible_ || activeInput_ == ActiveInput::None) {
    return;
  }

  if (activeInput_ == ActiveInput::Query) {
    vqlQueryText_ += utf8;
    builder_.setVqlQuery(vqlQueryText_);
    recomputePreview();
  } else if (activeInput_ == ActiveInput::Label) {
    labelText_ += utf8;
  } else if (activeInput_ == ActiveInput::LocalDim) {
    localDimName_ += utf8;
  }
}

std::optional<gleditor::InputArea> QuotationBuilderOverlay::textArea() const {
  if (!visible_) {
    return std::nullopt;
  }
  return gleditor::InputArea{.x = 100, .y = 100, .width = 400, .height = 30};
}

bool QuotationBuilderOverlay::picked(const render::PickingResult &pick,
                                     RenderState & /*state*/) {
  if (!visible_ || pick.tag.kind != render::tagKindOverlay) {
    return false;
  }

  const auto tag = pick.tag.clusterIndex;
  if (tag == 0U) {
    return false;
  }

  if (tag == kTagClose || tag == kTagCancel) {
    setVisible(false);
    return true;
  }

  if (tag == kTagCommit) {
    return commitQuotation();
  }

  if (tag == kTagModeRank) {
    setMode(Selector::Kind::Rank);
    return true;
  }
  if (tag == kTagModeClosure) {
    setMode(Selector::Kind::Closure);
    return true;
  }
  if (tag == kTagModeQuery) {
    setMode(Selector::Kind::Query);
    return true;
  }

  if (tag == kTagDirToggle) {
    rankDir_ = (rankDir_ == zigzag::DimVector::POS) ? zigzag::DimVector::NEG
                                                    : zigzag::DimVector::POS;
    recomputePreview();
    return true;
  }

  if (tag == kTagStorePrev) {
    if (selectedStoreIndex_ > 0) {
      selectStore(selectedStoreIndex_ - 1);
    }
    return true;
  }
  if (tag == kTagStoreNext) {
    if (selectedStoreIndex_ + 1 < foreignStores_.size()) {
      selectStore(selectedStoreIndex_ + 1);
    }
    return true;
  }

  if (tag == kTagRootPrev) {
    if (selectedRootCellIndex_ > 0) {
      selectRootCell(selectedRootCellIndex_ - 1);
    }
    return true;
  }
  if (tag == kTagRootNext) {
    if (selectedRootCellIndex_ + 1 < candidateRootCells_.size()) {
      selectRootCell(selectedRootCellIndex_ + 1);
    }
    return true;
  }

  if (tag == kTagInputQuery) {
    activeInput_ = ActiveInput::Query;
    return true;
  }
  if (tag == kTagInputLabel) {
    activeInput_ = ActiveInput::Label;
    return true;
  }
  if (tag == kTagInputDim) {
    activeInput_ = ActiveInput::LocalDim;
    return true;
  }

  // Carried dimensions toggles
  if (tag >= kTagDimBase && tag < kTagDimBase + availableForeignDims_.size()) {
    const auto idx = tag - kTagDimBase;
    toggleCarriedDimension(availableForeignDims_[idx].first);
    return true;
  }

  // Preview cell picking
  const auto &preview = builder_.preview();
  if (tag >= kTagPreviewBase && tag < kTagPreviewBase + preview.cells.size()) {
    const auto idx = tag - kTagPreviewBase;
    builder_.focusPreviewCell(idx);
    return true;
  }

  return true;
}

void QuotationBuilderOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!visible_ || !canvas_) {
    return;
  }

  const auto screenW = static_cast<float>(ctx.screenWidth);
  const auto screenH = static_cast<float>(ctx.screenHeight);
  const auto ortho   = glm::ortho(0.0F, screenW, 0.0F, screenH, -1.0F, 1.0F);

  // Modal dimensions: 840x560 centered
  const float modalW = std::min(840.0F, screenW - 40.0F);
  const float modalH = std::min(560.0F, screenH - 40.0F);
  const float modalX = (screenW - modalW) * 0.5F;
  const float modalY = (screenH - modalH) * 0.5F;
  const float topY   = modalY + modalH;

  canvas_->clear();

  // Background scrim
  canvas_->setTag(render::tagKindOverlay, 0);
  canvas_->addRect(0.0F, 0.0F, screenW, screenH, 0x00000088);

  // Modal background & borders
  canvas_->addRect(modalX, modalY, modalW, modalH, kColorBgOverlay);
  canvas_->addLine(modalX, topY, modalX + modalW, topY, 2.0F, kColorBorderCyan);
  canvas_->addLine(modalX, modalY, modalX + modalW, modalY, 1.0F, kColorBorder);
  canvas_->addLine(modalX, modalY, modalX, topY, 1.0F, kColorBorder);
  canvas_->addLine(modalX + modalW, modalY, modalX + modalW, topY, 1.0F,
                   kColorBorder);

  // Header
  canvas_->addRect(modalX, topY - 40.0F, modalW, 40.0F, kColorHeaderBg);
  canvas_->addText(ctx.state, modalX + 16.0F, topY - 26.0F,
                   "Quotation Builder (Section 5.10)", kColorSkyBlue, 0);

  // Close button [x]
  canvas_->setTag(render::tagKindOverlay, kTagClose);
  canvas_->addRect(modalX + modalW - 36.0F, topY - 32.0F, 24.0F, 24.0F,
                   0xDC2626CC);
  canvas_->addText(ctx.state, modalX + modalW - 28.0F, topY - 18.0F, "x",
                   kColorTextWhite, 0);

  // 1. Source & Root configuration row
  const float row1Y = topY - 76.0F;
  canvas_->setTag(render::tagKindOverlay, 0);
  canvas_->addText(ctx.state, modalX + 16.0F, row1Y + 4.0F,
                   "Foreign Store:", kColorTextMuted, 0);

  const auto curStoreName =
      foreignStores_.empty() ? "none" : foreignStores_[selectedStoreIndex_];
  canvas_->setTag(render::tagKindOverlay, kTagStorePrev);
  canvas_->addRect(modalX + 120.0F, row1Y, 24.0F, 24.0F, kColorInactiveTab);
  canvas_->addText(ctx.state, modalX + 128.0F, row1Y + 5.0F, "<",
                   kColorTextWhite, 0);

  canvas_->setTag(render::tagKindOverlay, 0);
  canvas_->addRect(modalX + 148.0F, row1Y, 180.0F, 24.0F, 0x1E293BEE);
  canvas_->addText(ctx.state, modalX + 154.0F, row1Y + 5.0F,
                   truncateString(curStoreName, 20), kColorSkyBlue, 0);

  canvas_->setTag(render::tagKindOverlay, kTagStoreNext);
  canvas_->addRect(modalX + 332.0F, row1Y, 24.0F, 24.0F, kColorInactiveTab);
  canvas_->addText(ctx.state, modalX + 340.0F, row1Y + 5.0F, ">",
                   kColorTextWhite, 0);

  // Root cell selector
  canvas_->setTag(render::tagKindOverlay, 0);
  canvas_->addText(ctx.state, modalX + 375.0F, row1Y + 4.0F,
                   "Root Cell:", kColorTextMuted, 0);
  const auto curRootName =
      candidateRootCells_.empty()
          ? "none"
          : candidateRootCells_[selectedRootCellIndex_].second;

  canvas_->setTag(render::tagKindOverlay, kTagRootPrev);
  canvas_->addRect(modalX + 450.0F, row1Y, 24.0F, 24.0F, kColorInactiveTab);
  canvas_->addText(ctx.state, modalX + 458.0F, row1Y + 5.0F, "<",
                   kColorTextWhite, 0);

  canvas_->setTag(render::tagKindOverlay, 0);
  canvas_->addRect(modalX + 478.0F, row1Y, 180.0F, 24.0F, 0x1E293BEE);
  canvas_->addText(ctx.state, modalX + 484.0F, row1Y + 5.0F,
                   truncateString(curRootName, 20), kColorSkyBlue, 0);

  canvas_->setTag(render::tagKindOverlay, kTagRootNext);
  canvas_->addRect(modalX + 662.0F, row1Y, 24.0F, 24.0F, kColorInactiveTab);
  canvas_->addText(ctx.state, modalX + 670.0F, row1Y + 5.0F, ">",
                   kColorTextWhite, 0);

  // 2. Selector Mode Tabs
  const float tabY   = row1Y - 36.0F;
  const auto curMode = builder_.mode();

  canvas_->setTag(render::tagKindOverlay, kTagModeRank);
  canvas_->addRect(modalX + 16.0F, tabY, 80.0F, 26.0F,
                   curMode == Selector::Kind::Rank ? kColorActiveTab
                                                   : kColorInactiveTab);
  canvas_->addText(ctx.state, modalX + 36.0F, tabY + 6.0F, "Rank",
                   kColorTextWhite, 0);

  canvas_->setTag(render::tagKindOverlay, kTagModeClosure);
  canvas_->addRect(modalX + 104.0F, tabY, 80.0F, 26.0F,
                   curMode == Selector::Kind::Closure ? kColorActiveTab
                                                      : kColorInactiveTab);
  canvas_->addText(ctx.state, modalX + 118.0F, tabY + 6.0F, "Closure",
                   kColorTextWhite, 0);

  canvas_->setTag(render::tagKindOverlay, kTagModeQuery);
  canvas_->addRect(modalX + 192.0F, tabY, 90.0F, 26.0F,
                   curMode == Selector::Kind::Query ? kColorActiveTab
                                                    : kColorInactiveTab);
  canvas_->addText(ctx.state, modalX + 204.0F, tabY + 6.0F, "VQL Query",
                   kColorTextWhite, 0);

  // Selector controls based on mode
  const float ctrlY = tabY - 34.0F;
  if (curMode == Selector::Kind::Closure) {
    canvas_->setTag(render::tagKindOverlay, 0);
    canvas_->addText(ctx.state, modalX + 16.0F, ctrlY + 4.0F,
                     "Carried Dims:", kColorTextMuted, 0);
    float dimX = modalX + 110.0F;
    for (std::size_t i = 0; i < availableForeignDims_.size(); ++i) {
      const auto &dimPair   = availableForeignDims_[i];
      const bool isSelected = selectedCarriedDims_.contains(dimPair.first);
      const auto btnW       = 80.0F;
      if (dimX + btnW > modalX + modalW - 16.0F) {
        break;
      }
      canvas_->setTag(render::tagKindOverlay, kTagDimBase + i);
      canvas_->addRect(dimX, ctrlY, btnW, 22.0F,
                       isSelected ? 0x059669EE : 0x1E293B88);
      canvas_->addText(ctx.state, dimX + 6.0F, ctrlY + 4.0F,
                       truncateString(dimPair.second, 9), kColorTextWhite, 0);
      dimX += btnW + 6.0F;
    }
  } else if (curMode == Selector::Kind::Rank) {
    canvas_->setTag(render::tagKindOverlay, 0);
    canvas_->addText(ctx.state, modalX + 16.0F, ctrlY + 4.0F,
                     "Direction:", kColorTextMuted, 0);
    canvas_->setTag(render::tagKindOverlay, kTagDirToggle);
    canvas_->addRect(modalX + 85.0F, ctrlY, 70.0F, 22.0F, kColorActiveTab);
    canvas_->addText(ctx.state, modalX + 92.0F, ctrlY + 4.0F,
                     rankDir_ == zigzag::DimVector::POS ? "POS (+)" : "NEG (-)",
                     kColorTextWhite, 0);
  } else if (curMode == Selector::Kind::Query) {
    canvas_->setTag(render::tagKindOverlay, 0);
    canvas_->addText(ctx.state, modalX + 16.0F, ctrlY + 4.0F,
                     "VQL Query:", kColorTextMuted, 0);
    canvas_->setTag(render::tagKindOverlay, kTagInputQuery);
    canvas_->addRect(modalX + 90.0F, ctrlY, 400.0F, 22.0F,
                     activeInput_ == ActiveInput::Query ? 0x0369A1CC
                                                        : 0x1E293BEE);
    canvas_->addText(ctx.state, modalX + 96.0F, ctrlY + 4.0F,
                     vqlQueryText_ +
                         (activeInput_ == ActiveInput::Query ? "_" : ""),
                     kColorSkyBlue, 0);
  }

  // 3. 2D Navigable Preview Canvas
  const float cvsX = modalX + 16.0F;
  const float cvsY = modalY + 60.0F;
  const float cvsW = modalW - 240.0F;
  const float cvsH = ctrlY - cvsY - 12.0F;

  canvas_->setTag(render::tagKindOverlay, 0);
  canvas_->addRect(cvsX, cvsY, cvsW, cvsH, kColorCanvasBg);
  canvas_->addLine(cvsX, cvsY, cvsX + cvsW, cvsY, 1.0F, kColorBorder);
  canvas_->addLine(cvsX, cvsY + cvsH, cvsX + cvsW, cvsY + cvsH, 1.0F,
                   kColorBorder);
  canvas_->addLine(cvsX, cvsY, cvsX, cvsY + cvsH, 1.0F, kColorBorder);
  canvas_->addLine(cvsX + cvsW, cvsY, cvsX + cvsW, cvsY + cvsH, 1.0F,
                   kColorBorder);

  const auto &preview = builder_.preview();
  if (!preview.isValid) {
    canvas_->addText(ctx.state, cvsX + 20.0F, cvsY + cvsH * 0.5F,
                     preview.statusMessage.empty() ? "No preview available"
                                                   : preview.statusMessage,
                     kColorDanger, 0);
  } else {
    // Render preview cells in a grid layout
    const float cellW = 100.0F;
    const float cellH = 40.0F;
    const float gapX  = 24.0F;
    const float gapY  = 16.0F;
    const int cols =
        std::max(1, static_cast<int>((cvsW - 20.0F) / (cellW + gapX)));

    for (std::size_t i = 0; i < preview.cells.size(); ++i) {
      const auto &c  = preview.cells[i];
      const int col  = static_cast<int>(i % cols);
      const int row  = static_cast<int>(i / cols);
      const float px = cvsX + 16.0F + static_cast<float>(col) * (cellW + gapX);
      const float py =
          cvsY + cvsH - 56.0F - static_cast<float>(row) * (cellH + gapY);

      if (py < cvsY) {
        break; // Bounded by viewport
      }

      const bool isFocused = (i == preview.focusedIndex);
      canvas_->setTag(render::tagKindOverlay, kTagPreviewBase + i);
      canvas_->addRect(px, py, cellW, cellH,
                       isFocused ? kColorCellFocusBg : kColorCellBg);
      canvas_->addLine(px, py, px + cellW, py, 1.0F,
                       isFocused ? kColorSkyBlue : kColorBorder);
      canvas_->addLine(px, py + cellH, px + cellW, py + cellH, 1.0F,
                       isFocused ? kColorSkyBlue : kColorBorder);
      canvas_->addLine(px, py, px, py + cellH, 1.0F,
                       isFocused ? kColorSkyBlue : kColorBorder);
      canvas_->addLine(px + cellW, py, px + cellW, py + cellH, 1.0F,
                       isFocused ? kColorSkyBlue : kColorBorder);

      canvas_->addText(ctx.state, px + 6.0F, py + 22.0F,
                       truncateString(c.text.empty() ? "(cell)" : c.text, 10),
                       isFocused ? kColorSkyBlue : kColorTextWhite, 0);
      canvas_->addText(ctx.state, px + 6.0F, py + 6.0F,
                       "#" + std::to_string(c.foreignCell), kColorTextMuted, 0);

      // Directional link connector
      if (col < cols - 1 && i + 1 < preview.cells.size()) {
        canvas_->setTag(render::tagKindOverlay, 0);
        canvas_->addRect(px + cellW, py + cellH * 0.5F - 1.0F, gapX, 2.0F,
                         kColorEdge);
      }
    }
  }

  // 4. Candidate Inspector Sidebar
  const float inspX = cvsX + cvsW + 12.0F;
  const float inspW = modalW - (inspX - modalX) - 16.0F;
  canvas_->setTag(render::tagKindOverlay, 0);
  canvas_->addRect(inspX, cvsY, inspW, cvsH, 0x0B0F19EE);
  canvas_->addText(ctx.state, inspX + 10.0F, cvsY + cvsH - 22.0F,
                   "Candidate Inspector", kColorSkyBlue, 0);

  if (preview.isValid && preview.focusedIndex < preview.cells.size()) {
    const auto &focused = preview.cells[preview.focusedIndex];
    canvas_->addText(ctx.state, inspX + 10.0F, cvsY + cvsH - 46.0F,
                     "Foreign ID: #" + std::to_string(focused.foreignCell),
                     kColorSkyBlue, 0);
    canvas_->addText(ctx.state, inspX + 10.0F, cvsY + cvsH - 66.0F,
                     "Op ID: #" + focused.birthRef.produces.str(),
                     kColorTextMuted, 0);
    canvas_->addText(ctx.state, inspX + 10.0F, cvsY + cvsH - 86.0F,
                     "Degree: " + std::to_string(focused.outboundEdges.size()),
                     kColorTextMuted, 0);
    canvas_->addText(ctx.state, inspX + 10.0F, cvsY + cvsH - 110.0F,
                     "Content:", kColorTextWhite, 0);
    canvas_->addText(ctx.state, inspX + 10.0F, cvsY + cvsH - 130.0F,
                     truncateString(focused.text, 18), kColorTextWhite, 0);
  } else {
    canvas_->addText(ctx.state, inspX + 10.0F, cvsY + cvsH - 50.0F,
                     "(No cell focused)", kColorTextMuted, 0);
  }

  // 5. Footer Configuration & Commit Bar
  const float footY = modalY + 16.0F;
  canvas_->setTag(render::tagKindOverlay, 0);
  canvas_->addText(ctx.state, modalX + 16.0F, footY + 4.0F,
                   "Local Dim:", kColorTextMuted, 0);
  canvas_->setTag(render::tagKindOverlay, kTagInputDim);
  canvas_->addRect(modalX + 85.0F, footY, 90.0F, 24.0F,
                   activeInput_ == ActiveInput::LocalDim ? 0x0369A1CC
                                                         : 0x1E293BEE);
  canvas_->addText(ctx.state, modalX + 90.0F, footY + 4.0F,
                   localDimName_ +
                       (activeInput_ == ActiveInput::LocalDim ? "_" : ""),
                   kColorSkyBlue, 0);

  canvas_->setTag(render::tagKindOverlay, 0);
  canvas_->addText(ctx.state, modalX + 185.0F, footY + 4.0F,
                   "Label:", kColorTextMuted, 0);
  canvas_->setTag(render::tagKindOverlay, kTagInputLabel);
  canvas_->addRect(modalX + 230.0F, footY, 130.0F, 24.0F,
                   activeInput_ == ActiveInput::Label ? 0x0369A1CC
                                                      : 0x1E293BEE);
  canvas_->addText(ctx.state, modalX + 236.0F, footY + 4.0F,
                   labelText_ + (activeInput_ == ActiveInput::Label ? "_" : ""),
                   kColorSkyBlue, 0);

  // Budget status indicator
  const auto budgetStr =
      preview.isValid ? std::format("{} cells / {} B ops [OK]",
                                    preview.cells.size(), preview.totalOpBytes)
                      : "Budget / state invalid";
  canvas_->setTag(render::tagKindOverlay, 0);
  canvas_->addText(ctx.state, modalX + 375.0F, footY + 4.0F, budgetStr,
                   preview.isValid ? kColorSuccess : kColorDanger, 0);

  // Commit / Cancel Buttons
  canvas_->setTag(render::tagKindOverlay, kTagCommit);
  canvas_->addRect(modalX + modalW - 200.0F, footY, 110.0F, 28.0F,
                   preview.isValid ? kColorCommitBtn : 0x1E293B88);
  canvas_->addText(ctx.state, modalX + modalW - 192.0F, footY + 6.0F,
                   "Commit (Enter)", kColorTextWhite, 0);

  canvas_->setTag(render::tagKindOverlay, kTagCancel);
  canvas_->addRect(modalX + modalW - 80.0F, footY, 65.0F, 28.0F,
                   kColorInactiveTab);
  canvas_->addText(ctx.state, modalX + modalW - 70.0F, footY + 6.0F, "Cancel",
                   kColorTextWhite, 0);

  canvas_->commit();
  canvas_->draw(ctx.state, ortho);
}

void QuotationBuilderOverlay::describe(gleditor::a11y::Builder &into) {
  if (!visible_) {
    return;
  }
  constexpr std::uint64_t kDialogNodeId = 0x80000000ULL;
  auto &dialogNode = into.add(kDialogNodeId, gleditor::a11y::Role::Group);
  dialogNode.label = "Quotation Builder Dialog";
  into.contribute(kDialogNodeId);

  const std::string storeKey =
      foreignStores_.empty() ? "none" : foreignStores_[selectedStoreIndex_];
  constexpr std::uint64_t kStoreNodeId = 0x80000001ULL;
  auto &storeNode = into.add(kStoreNodeId, gleditor::a11y::Role::Label);
  storeNode.label = "Source Store: " + storeKey;
  dialogNode.children.push_back(kStoreNodeId);

  constexpr std::uint64_t kPreviewNodeId = 0x80000002ULL;
  auto &previewNode = into.add(kPreviewNodeId, gleditor::a11y::Role::Group);
  previewNode.label = "Quotation Preview (" +
                      std::to_string(builder_.preview().cells.size()) +
                      " cells)";
  dialogNode.children.push_back(kPreviewNodeId);
}

} // namespace xanadu
