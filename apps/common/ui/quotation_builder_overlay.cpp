/**
 * @file quotation_builder_overlay.cpp
 * @brief Implementation of the interactive Quotation Builder Overlay.
 */
#include "common/ui/quotation_builder_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <gleditor/text/font.hpp>
#include <limits>
#include <ranges>
#include <stdexcept>

namespace xanadu {
namespace ui = gleditor::ui;

QuotationBuilderOverlay::QuotationBuilderOverlay(
    Store &localStore, MicroversionId activeVersion, RendererRef renderer,
    SwarmCatalog *catalog, std::string fontName, OpenStoresProvider openStores,
    VersionCommitCallback onCommit)
    : localStore_(localStore), activeVersion_(activeVersion),
      renderer_(std::move(renderer)), catalog_(catalog),
      fontName_(std::move(fontName)),
      openStoresProvider_(std::move(openStores)),
      onCommit_(std::move(onCommit)),
      overlay_({.id = 1, .model = ui::Modal{}}) {
  overlay_.setVisible(false);
  overlay_.setActionHandler([this](const auto &action) { queue(action); });
  fields_ = {
      {{vqlQueryText_, "VQL query", "query", vqlQueryText_.size()},
       {localDimName_, "Local dimension", "local-dim", localDimName_.size()},
       {labelText_, "Quotation label", "label", labelText_.size()}}};
  refreshSources();
}

QuotationBuilderOverlay::~QuotationBuilderOverlay() = default;

void QuotationBuilderOverlay::deviceReady(
    render::RenderDevice &device, const render::PipelineDesc &pipeline) {
  overlay_.deviceReady(device, pipeline);
}

bool QuotationBuilderOverlay::busy() const {
  const std::scoped_lock lock(guard_);
  return !pending_.empty();
}

QuotationBuilderOverlay *
QuotationBuilderOverlay::setVisible(const bool visible) {
  const std::scoped_lock lock(guard_);
  if (visible_.load() == visible) return this;
  visible_ = visible;
  ++epoch_;
  pending_.clear();
  actions_.clear();
  overlay_.setVisible(visible);
  if (visible) {
    closeId_ = allocate();
    for (auto &id : tabIds_) id = allocate();
    for (auto &id : fieldIds_) id = allocate();
    activate();
    if (activeVersion_.isZero() &&
        !localStore_.primaryCurrentVersion().isZero())
      activeVersion_ = localStore_.primaryCurrentVersion();
    refreshSources();
  } else {
    deactivate();
  }
  changed(true);
  return this;
}

QuotationBuilderOverlay *QuotationBuilderOverlay::toggle() {
  setVisible(!visible_);
  return this;
}

QuotationBuilderOverlay *QuotationBuilderOverlay::refreshSources() {
  const std::scoped_lock lock(guard_);
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
  return this;
}

QuotationBuilderOverlay *
QuotationBuilderOverlay::selectStore(const std::size_t index) {
  const std::scoped_lock lock(guard_);
  if (index >= foreignStores_.size()) {
    return this;
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

  selectedRankDimIndex_  = 0;
  selectedRootCellIndex_ = 0;
  scrollPx_              = 0;
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
  return this;
}

QuotationBuilderOverlay *
QuotationBuilderOverlay::selectRootCell(const std::size_t index) {
  const std::scoped_lock lock(guard_);
  if (index >= candidateRootCells_.size()) {
    return this;
  }
  selectedRootCellIndex_ = index;
  const auto rootCell    = candidateRootCells_[index].first;
  if (rootCell != zigzag::noCell) {
    builder_.setRootCell(rootCell);
  }
  recomputePreview();
  return this;
}

QuotationBuilderOverlay *
QuotationBuilderOverlay::setMode(const Selector::Kind mode) {
  const std::scoped_lock lock(guard_);
  builder_.setMode(mode);
  recomputePreview();
  return this;
}

QuotationBuilderOverlay *
QuotationBuilderOverlay::toggleCarriedDimension(const zigzag::DimRef dim) {
  const std::scoped_lock lock(guard_);
  if (selectedCarriedDims_.contains(dim)) {
    selectedCarriedDims_.erase(dim);
  } else {
    selectedCarriedDims_.insert(dim);
  }
  recomputePreview();
  return this;
}

QuotationBuilderOverlay *
QuotationBuilderOverlay::setVqlQuery(std::string query) {
  const std::scoped_lock lock(guard_);
  vqlQueryText_    = std::move(query);
  fields_[0].value = vqlQueryText_;
  fields_[0].caret = vqlQueryText_.size();
  builder_.setVqlQuery(vqlQueryText_);
  recomputePreview();
  return this;
}

void QuotationBuilderOverlay::recomputePreview() {
  const std::scoped_lock lock(guard_);
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
  observedLocalOps_  = localStore_.opCount();
  observedTargetOps_ = st->opCount();
  changed(true);
}

bool QuotationBuilderOverlay::commitQuotation() {
  const std::scoped_lock lock(guard_);
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

ui::WidgetId QuotationBuilderOverlay::allocate() {
  if (nextId_ == std::numeric_limits<ui::WidgetId>::max())
    throw std::length_error("Quotation action identities exhausted");
  return nextId_++;
}
ui::WidgetId QuotationBuilderOverlay::contentId(std::string_view name,
                                                zigzag::CellRef ref) {
  const auto key = std::to_string(page_) + ":" + std::string(name) + ":" +
                   std::to_string(ref);
  const auto found = contentIds_.find(key);
  if (found != contentIds_.end()) return found->second;
  const auto id = allocate();
  contentIds_.emplace(key, id);
  return id;
}
void QuotationBuilderOverlay::changed(bool semantic) {
  dirty_ = true;
  ++a11yRevision_;
  if (semantic) {
    ++semanticRevision_;
    contentIds_.clear();
  }
}
QuotationBuilderOverlay *
QuotationBuilderOverlay::setConfig(const ModalPresentationConfig &config) {
  const std::scoped_lock lock(guard_);
  if (config_ != config) {
    config_ = config;
    changed();
  }
  return this;
}
std::shared_ptr<const ui::WidgetScene>
QuotationBuilderOverlay::prepare(const ui::UiMetrics &metrics,
                                 const ui::Theme &sourceTheme) {
  const std::scoped_lock lock(guard_);
  if (!visible_) return {};
  observeStores();
  if (!dirty_ && metrics_ == metrics && sourceTheme_ == sourceTheme)
    return overlay_.snapshot();
  metrics_     = metrics;
  sourceTheme_ = sourceTheme;
  theme_ = ui::withFontOverride(sourceTheme, ui::FontRole::Label, fontName_);
  // Paging keeps configured typography intact in a short viewport.
  theme_.paddingEm = std::min(theme_.paddingEm, .1F);
  theme_.gapEm     = std::min(theme_.gapEm, .1F);
  const auto font  = gleditor::text::FontManager::instance().getFont(
      metrics.fontDescription(ui::FontRole::Label, theme_));
  const auto line = std::ceil(font->metrics().lineHeight) + 2;
  const auto safe = metrics.pixelSafeArea();
  const ModalPresentationConfig defaults;
  const auto share = [](float value, float fallback) {
    return std::isfinite(value) ? std::clamp(value, .1F, 1.F) : fallback;
  };
  const auto length = [&](float value, float fallback) {
    return metrics.px(std::isfinite(value) && value > 0 ? value : fallback);
  };
  const auto width       = std::floor(std::min(
      length(config_.widthPx, defaults.widthPx),
      safe.width * share(config_.maxWidthShare, defaults.maxWidthShare)));
  const auto height      = std::floor(std::min(
      length(config_.heightPx, defaults.heightPx),
      safe.height * share(config_.maxHeightShare, defaults.maxHeightShare)));
  const auto basePadding = theme_.paddingEm;
  const auto baseGap     = theme_.gapEm;
  const auto required    = [&](float factor) {
    const auto p = font->metrics().lineHeight * basePadding * factor;
    const auto g = font->metrics().lineHeight * baseGap * factor;
    const auto t =
        std::ceil(std::max(metrics.px(theme_.type.minTouchPx), line + p * 2)) +
        2;
    // Include nested flow padding and rounding slack. Compact Preview reserves
    // two complete candidate rows alongside its inspector.
    return std::max(t * 5 + p * 8 + g * 4 + 16,
                       t * 4 + line + p * 10 + g * 3 + 16);
  };
  float factor = 1;
  if (required(factor) > height) {
    float lower = 0, upper = 1;
    for (int i = 0; i < 20; ++i) {
      const auto middle = (lower + upper) * .5F;
      if (required(middle) <= height)
        lower = middle;
      else
        upper = middle;
    }
    factor = lower;
  }
  theme_.paddingEm = basePadding * factor;
  theme_.gapEm     = baseGap * factor;
  const auto pad   = font->metrics().lineHeight * theme_.paddingEm;
  const auto gap   = font->metrics().lineHeight * theme_.gapEm;
  const auto touch =
      std::ceil(std::max(metrics.px(theme_.type.minTouchPx), line + pad * 2)) +
      2;
  const auto flowHeight = touch + pad * 2 + 2;
  const auto inner      = std::max(0.F, width - pad * 2);
  ui::Widget model{
      .id        = 1,
      .model     = ui::Modal{},
      .preferred = {metrics.logical(width), metrics.logical(height)}};
  actions_.clear();
  listActions_.clear();
  const auto action = [&](ui::WidgetId id, std::string name,
                          std::size_t index   = 0,
                          zigzag::CellRef ref = zigzag::noCell) {
    actions_.emplace(
        id, Action{std::move(name), {}, index, epoch_, semanticRevision_, ref});
  };
  const auto button = [&](std::string label, std::string name, float w,
                          bool enabled = true, std::size_t index = 0,
                          zigzag::CellRef ref = zigzag::noCell) {
    const auto id = contentId(name, ref);
    action(id, name, index, ref);
    return ui::Widget{
        .id        = id,
        .model     = ui::Button{std::move(label), std::move(name), enabled},
        .preferred = {metrics.logical(w), metrics.logical(touch)}};
  };
  const auto row = [&](std::vector<ui::Widget> children) {
    return ui::Widget{.id        = allocate(),
                      .model     = ui::ButtonFlow{},
                      .children  = std::move(children),
                      .preferred = {0, metrics.logical(flowHeight)}};
  };
  const auto rowWidth = std::max(0.F, inner - pad * 2);
  const auto closeWidth =
      std::min(rowWidth * .3F,
               std::max(metrics.px(theme_.type.minTouchPx), line * 2.5F));
  action(closeId_, "close");
  model.children.push_back(row(
      {{.id        = 2,
        .model     = ui::Label{"Quotation Builder"},
        .preferred = {metrics.logical(
                          std::max(0.F, rowWidth - closeWidth - gap - 2)),
                      metrics.logical(touch)}},
       {.id        = closeId_,
        .model     = ui::Button{"Close", "close"},
        .preferred = {metrics.logical(closeWidth), metrics.logical(touch)}}}));
  ui::Tabs tabs{.selected = page_};
  const std::array<std::string, 4> names{"Source", "Selector", "Preview",
                                         "Commit"};
  for (std::size_t i = 0; i < names.size(); ++i) {
    tabs.tabs.push_back({tabIds_[i], names[i], "page"});
    action(tabIds_[i], "page", i);
  }
  model.children.push_back({.id        = 3,
                            .model     = std::move(tabs),
                            .preferred = {0, metrics.logical(flowHeight)}});
  const auto bodyHeight =
      std::max(0.F, height - pad * 2 - flowHeight * 2 - gap * 2 - 2);
  const auto &preview = builder_.preview();
  if (page_ == 0) {
    const auto store =
        foreignStores_.empty() ? "none" : foreignStores_[selectedStoreIndex_];
    const auto root = candidateRootCells_.empty()
                          ? "none"
                          : candidateRootCells_[selectedRootCellIndex_].second;
    model.children.push_back(button("Source store: " + store, "store-next",
                                    inner, foreignStores_.size() > 1));
    model.children.push_back(button("Root cell: " + root, "root-next", inner,
                                    candidateRootCells_.size() > 1));
    model.children.push_back(row({button("Previous store", "store-prev",
                                         std::floor((rowWidth - gap - 2) / 2),
                                         foreignStores_.size() > 1),
                                  button("Previous root", "root-prev",
                                         std::floor((rowWidth - gap - 2) / 2),
                                         candidateRootCells_.size() > 1)}));
  } else if (page_ == 1) {
    const auto mode = builder_.mode();
    const auto name = mode == Selector::Kind::Rank      ? "Rank"
                      : mode == Selector::Kind::Closure ? "Closure"
                                                        : "VQL Query";
    model.children.push_back(
        button(std::string("Selector: ") + name, "mode", inner));
    if (mode == Selector::Kind::Rank) {
      const auto dim = availableForeignDims_.empty()
                           ? "none"
                           : availableForeignDims_[selectedRankDimIndex_ %
                                                   availableForeignDims_.size()]
                                 .second;
      model.children.push_back(button("Rank dimension: " + dim, "rank-dim",
                                      inner, !availableForeignDims_.empty()));
      model.children.push_back(button(rankDir_ == zigzag::DimVector::POS
                                          ? "Direction: POS (+)"
                                          : "Direction: NEG (-)",
                                      "direction", inner));
    } else if (mode == Selector::Kind::Query) {
      const auto id = fieldIds_[0];
      action(id, "query");
      model.children.push_back({.id        = id,
                                .model     = fields_[0],
                                .preferred = {0, metrics.logical(touch)}});
      model.children.push_back({.id    = 4,
                                .model = ui::Label{preview.statusMessage.empty()
                                                       ? "Enter a VQL query"
                                                       : preview.statusMessage},
                                .preferred = {0, metrics.logical(touch)},
                                .maxLines  = 1});
    } else {
      ui::List list{.scrollPx = scrollPx_, .rowHeightPx = touch, .overscan = 0};
      for (const auto &[dim, name] : availableForeignDims_) {
        const auto id = contentId("carried-dim", dim);
        action(id, "carried-dim", 0, dim);
        listActions_.push_back(actions_.at(id));
        list.rows.push_back(
            {id,
             std::string(selectedCarriedDims_.contains(dim) ? "Carried: "
                                                            : "Excluded: ") +
                 name,
             "carried-dim"});
      }
      listHeight_ = std::max(touch, bodyHeight - touch - gap - pad * 2 - 2);
      rowHeight_  = touch;
      model.children.push_back(
          {.id        = 5,
           .model     = std::move(list),
           .preferred = {0, metrics.logical(listHeight_)}});
    }
  } else if (page_ == 2) {
    const auto budget = std::to_string(preview.cells.size()) + " cells / " +
                        std::to_string(preview.totalOpBytes) + " B ops · ";
    std::string inspector = budget + "No cell focused";
    if (preview.isValid && preview.focusedIndex < preview.cells.size()) {
      const auto &cell = preview.cells[preview.focusedIndex];
      inspector        = budget + "Cell #" + std::to_string(cell.foreignCell) +
                  " · Op " + cell.birthRef.produces.str() + " · Degree " +
                  std::to_string(cell.outboundEdges.size()) + " · " + cell.text;
    }
    const std::uint16_t inspectorLines =
        bodyHeight >= line * 2 + pad * 4 + gap + touch * 2 + 8 ? 2 : 1;
    const auto inspectorHeight = line * inspectorLines + pad * 2 + 2;
    model.children.push_back(
        {.id    = 4,
         .model = ui::Label{std::move(inspector), ui::TextPurpose::Description},
         .preferred = {0, metrics.logical(inspectorHeight)},
         .maxLines  = inspectorLines});
    if (!preview.isValid) {
      model.children.push_back(
          {.id = 5,
           .model =
               ui::Label{preview.statusMessage.empty() ? "No preview available"
                                                       : preview.statusMessage,
                         ui::TextPurpose::Description},
           .preferred = {
               0, metrics.logical(
                      std::max(0.F, bodyHeight - inspectorHeight - gap - 2))}});
    } else {
      ui::List list{.scrollPx = scrollPx_, .rowHeightPx = touch, .overscan = 0};
      for (std::size_t i = 0; i < preview.cells.size(); ++i) {
        const auto &cell = preview.cells[i];
        const auto id    = contentId("preview-cell", cell.foreignCell);
        action(id, "preview-cell", i, cell.foreignCell);
        listActions_.push_back(actions_.at(id));
        list.rows.push_back(
            {id,
             std::string(i == preview.focusedIndex ? "Focused " : "") +
                 "Cell #" + std::to_string(cell.foreignCell) + ": " +
                 (cell.text.empty() ? "(empty)" : cell.text),
             "preview-cell"});
      }
      rowHeight_  = touch;
      listHeight_ = std::max(touch, bodyHeight - inspectorHeight - gap - 2);
      model.children.push_back(
          {.id        = 5,
           .model     = std::move(list),
           .preferred = {0, metrics.logical(listHeight_)}});
    }
  } else {
    const auto field = [&](std::size_t index) {
      const auto id = fieldIds_[index];
      action(id, fields_[index].action);
      return ui::Widget{.id        = id,
                        .model     = fields_[index],
                        .preferred = {0, metrics.logical(touch)}};
    };
    model.children.push_back(field(1));
    model.children.push_back(field(2));
    model.children.push_back(
        row({button("Commit quotation", "commit",
                    std::floor((rowWidth - gap - 2) / 2), preview.isValid),
             button("Cancel", "close", std::floor((rowWidth - gap - 2) / 2))}));
  }
  overlay_.setModel(std::move(model));
  overlay_.setBounds(ui::clampToSafeArea(
      metrics.rounded({safe.left + (safe.width - width) / 2,
                       safe.bottom + (safe.height - height) / 2, width,
                       height}),
      safe));
  dirty_ = false;
  return overlay_.prepare(metrics, theme_);
}
void QuotationBuilderOverlay::queue(const ui::WidgetAction &incoming) {
  const std::scoped_lock lock(guard_);
  if (!visible_ || dirty_) return;
  if (incoming.id == 5 && incoming.itemIndex < listActions_.size()) {
    pending_.push_back(listActions_[incoming.itemIndex]);
    return;
  }
  // Tabs report their owner rather than the selected child's identity.
  const auto id    = incoming.id == 3 && incoming.itemIndex < tabIds_.size()
                         ? tabIds_[incoming.itemIndex]
                         : incoming.id;
  const auto found = actions_.find(id);
  if (found == actions_.end()) return;
  auto action  = found->second;
  action.value = incoming.value;
  for (std::size_t i = 0; i < fieldIds_.size(); ++i)
    if (incoming.id == fieldIds_[i]) {
      fields_[i].value = incoming.value;
      fields_[i].caret = incoming.caret.value_or(fields_[i].caret);
    }
  if ((action.name == "query" && incoming.value == vqlQueryText_) ||
      (action.name == "label" && incoming.value == labelText_) ||
      (action.name == "local-dim" && incoming.value == localDimName_))
    return;
  pending_.push_back(std::move(action));
}
void QuotationBuilderOverlay::observeStores() {
  if (observedLocalOps_ != localStore_.opCount() ||
      (selectedTargetStore_ &&
       observedTargetOps_ != selectedTargetStore_->opCount()))
    recomputePreview();
}
void QuotationBuilderOverlay::drain() {
  const std::scoped_lock lock(guard_);
  auto pending = std::move(pending_);
  pending_.clear();
  const auto startingSemantic = semanticRevision_;
  for (const auto &action : pending) {
    const auto editing = action.name == "query" || action.name == "label" ||
                         action.name == "local-dim";
    if (!visible_ || action.epoch != epoch_ ||
        action.semantic != (editing ? startingSemantic : semanticRevision_))
      continue;
    const auto &name = action.name;
    if (name == "close")
      setVisible(false);
    else if (name == "page") {
      page_     = action.index;
      scrollPx_ = 0;
      changed();
    } else if (name == "store-next" || name == "store-prev") {
      if (!foreignStores_.empty())
        selectStore((selectedStoreIndex_ + foreignStores_.size() +
                     (name == "store-next" ? 1 : -1)) %
                    foreignStores_.size());
    } else if (name == "root-next" || name == "root-prev") {
      if (!candidateRootCells_.empty())
        selectRootCell((selectedRootCellIndex_ + candidateRootCells_.size() +
                        (name == "root-next" ? 1 : -1)) %
                       candidateRootCells_.size());
    } else if (name == "mode") {
      const auto mode = builder_.mode();
      setMode(mode == Selector::Kind::Rank      ? Selector::Kind::Closure
              : mode == Selector::Kind::Closure ? Selector::Kind::Query
                                                : Selector::Kind::Rank);
    } else if (name == "rank-dim") {
      ++selectedRankDimIndex_;
      recomputePreview();
    } else if (name == "direction") {
      rankDir_ = rankDir_ == zigzag::DimVector::POS ? zigzag::DimVector::NEG
                                                    : zigzag::DimVector::POS;
      recomputePreview();
    } else if (name == "carried-dim") {
      if (std::ranges::any_of(availableForeignDims_, [&](const auto &dim) {
            return dim.first == action.ref;
          }))
        toggleCarriedDimension(action.ref);
    } else if (name == "preview-cell") {
      const auto &cells = builder_.preview().cells;
      if (action.index < cells.size() &&
          cells[action.index].foreignCell == action.ref) {
        builder_.focusPreviewCell(action.index);
        changed();
      }
    } else if (name == "query") {
      vqlQueryText_ = action.value;
      builder_.setVqlQuery(vqlQueryText_);
      recomputePreview();
    } else if (name == "label") {
      labelText_ = action.value;
      changed();
    } else if (name == "local-dim") {
      localDimName_ = action.value;
      changed();
    } else if (name == "commit")
      commitQuotation();
  }
}
void QuotationBuilderOverlay::drawFrame(gleditor::FrameContext &ctx) {
  const std::scoped_lock lock(guard_);
  observeStores();
  drain();
  if (!visible_) return;
  auto metrics         = ctx.metrics;
  metrics.screenWidth  = ctx.screenWidth;
  metrics.screenHeight = ctx.screenHeight;
  metrics.chrome       = ctx.chrome;
  std::ignore          = prepare(metrics, ctx.theme);
  gleditor::FrameContext draw{
      ctx.state,    ctx.viewProjection, ctx.screenWidth,   ctx.screenHeight,
      ctx.timeline, ctx.chrome,         ctx.settledChrome, metrics,
      theme_};
  overlay_.drawFrame(draw);
}
bool QuotationBuilderOverlay::picked(const render::PickingResult &pick,
                                     RenderState &state) {
  const std::scoped_lock lock(guard_);
  if (!visible_) return false;
  if (dirty_) return pick.tag.kind == render::tagKindOverlay;
  if (pick.requestId || pick.tag.docIndex || pick.tag.pageIndex) {
    if (pick.overlayWidgetId) requestFocus(*pick.overlayWidgetId);
    return overlay_.picked(pick, state);
  }
  // Legacy synthetic domain picks never originate from the retained renderer.
  if (pick.tag.kind != render::tagKindOverlay) return false;
  switch (pick.tag.clusterIndex) {
  case kTagClose:
  case kTagCancel:
    setVisible(false);
    return true;
  case kTagCommit:
    return commitQuotation();
  case kTagModeRank:
    setMode(Selector::Kind::Rank);
    return true;
  case kTagModeClosure:
    setMode(Selector::Kind::Closure);
    return true;
  case kTagModeQuery:
    setMode(Selector::Kind::Query);
    return true;
  default:
    return false;
  }
}
void QuotationBuilderOverlay::describe(gleditor::a11y::Builder &builder) {
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder retained(tree, builder.owner());
  overlay_.describe(retained);
  for (const auto &node : tree.nodes) {
    auto &copy = builder.add(gleditor::a11y::Ids::localOf(node.id), node.role);
    copy       = node;
    if (gleditor::a11y::Ids::localOf(node.id) == 1)
      copy.label = "Quotation Builder Dialog";
  }
  for (auto root : retained.roots()) builder.contribute(root);
}
bool QuotationBuilderOverlay::performAction(std::uint64_t id,
                                            gleditor::a11y::Action action,
                                            std::string_view value) {
  const std::scoped_lock lock(guard_);
  if (!visible_ || dirty_) return false;
  const auto local = gleditor::a11y::Ids::localOf(id);
  const auto scene = overlay_.snapshot();
  if (scene && scene->find(static_cast<ui::WidgetId>(local)))
    requestFocus(static_cast<ui::WidgetId>(local));
  return overlay_.performAction(id, action, value);
}
std::shared_ptr<const ui::LayoutResult>
QuotationBuilderOverlay::focusLayout() const {
  return overlay_.focusLayout();
}
void QuotationBuilderOverlay::focusedNodeChanged(std::uint32_t id) {
  const std::scoped_lock lock(guard_);
  overlay_.focusedNodeChanged(id);
}
bool QuotationBuilderOverlay::activateNode(std::uint32_t id) {
  const std::scoped_lock lock(guard_);
  return visible_ && !dirty_ && overlay_.activateNode(id);
}
void QuotationBuilderOverlay::focusChanged(bool focused) {
  overlay_.focusChanged(focused);
}
std::optional<gleditor::InputArea> QuotationBuilderOverlay::textArea() const {
  return overlay_.textArea();
}
std::optional<gleditor::InputArea>
QuotationBuilderOverlay::pointerArea() const {
  return overlay_.pointerArea();
}
void QuotationBuilderOverlay::scroll(float delta) {
  const std::scoped_lock lock(guard_);
  scrollPx_ = std::clamp(
      scrollPx_ + delta, 0.F,
      std::max(0.F, static_cast<float>(listActions_.size()) * rowHeight_ -
                        listHeight_ + 2));
  changed();
}
bool QuotationBuilderOverlay::pointerEvent(const ui::PointerEvent &event) {
  const std::scoped_lock lock(guard_);
  if (!visible_ || dirty_) return false;
  if (event.phase == ui::PointerPhase::Wheel && !listActions_.empty()) {
    const auto area = pointerArea();
    if (!area || event.x < area->x || event.x >= area->x + area->width ||
        event.y < area->y || event.y >= area->y + area->height)
      return false;
    scroll(-event.deltaY * rowHeight_);
    return true;
  }
  if (event.phase == ui::PointerPhase::Press && event.button == 1) {
    const auto scene = overlay_.snapshot();
    const auto *hit =
        scene
            ? scene->layout.hitTest(
                  event.x, static_cast<float>(metrics_.screenHeight) - event.y)
            : nullptr;
    if (hit && hit->focusable && hit->enabled) requestFocus(hit->id);
  }
  return overlay_.pointerEvent(event);
}
bool QuotationBuilderOverlay::keyPressed(gleditor::Key key,
                                         gleditor::KeyMods mods) {
  const std::scoped_lock lock(guard_);
  if (!visible_) return false;
  if (key == gleditor::Key::Escape) {
    setVisible(false);
    return true;
  }
  if (dirty_) return false;
  if (overlay_.keyPressed({key, mods})) return true;
  if ((key == gleditor::Key::PageDown || key == gleditor::Key::PageUp) &&
      !listActions_.empty()) {
    scroll((key == gleditor::Key::PageDown ? 1 : -1) * listHeight_);
    return true;
  }
  if (page_ == 2 &&
      (key == gleditor::Key::Left || key == gleditor::Key::Right)) {
    builder_.navigatePreview(key == gleditor::Key::Left ? -1 : 1);
    changed();
    return true;
  }
  if (key == gleditor::Key::Return) {
    const auto input = overlay_.textArea();
    if (!input && overlay_.keyPressed({gleditor::Key::Space, mods}))
      return true;
    if (!input && page_ == 3) {
      pending_.push_back({"commit", {}, 0, epoch_, semanticRevision_});
      return true;
    }
    return true;
  }
  return false;
}
void QuotationBuilderOverlay::textTyped(std::string_view utf8) {
  const std::scoped_lock lock(guard_);
  if (visible_ && !dirty_) {
    overlay_.textTyped(utf8);
  }
}
} // namespace xanadu
