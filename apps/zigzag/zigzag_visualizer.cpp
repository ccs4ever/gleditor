/**
 * @file zigzag_visualizer.cpp
 * @brief Implementation of the Xanadu ZigZag visualizer on gleditor.
 */
#include "zigzag_visualizer.hpp"
#include "core/zzcore.hpp"
#include "core/zzstructure_loader.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <iostream>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <gleditor/color.hpp>
#include <gleditor/paths.hpp>
#include <gleditor/render/diagnostics.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/spatial.hpp>

namespace zigzag {

namespace {

using gleditor::color::packRgba;

std::string shortenText(const std::string_view text,
                        const std::size_t maxLen = 32) {

  if (text.size() <= maxLen) {
    return std::string{text};
  }
  return std::string{text.substr(0, maxLen)} + "...";
}

} // namespace

ZigzagVisualizer::ZigzagVisualizer(std::string aFontName,
                                   const bool enablePrefletFetching)
    : fontName_(std::move(aFontName)), fetchingEnabled_(enablePrefletFetching),
      last_frame_time_(std::chrono::steady_clock::now()) {
  populateFallbackStructure();
}

ZigzagVisualizer::~ZigzagVisualizer() = default;

void ZigzagVisualizer::deviceReady(
    render::RenderDevice &device,
    const render::PipelineDesc &documentPipeline) {
  worldCanvas_ = std::make_unique<gleditor::Canvas>(&device, fontName_);
  worldCanvas_->createPipeline(documentPipeline, true);

  hudCanvas_ = std::make_unique<gleditor::Canvas>(&device, fontName_);
  hudCanvas_->createPipeline(documentPipeline, false);

  beams_ = std::make_unique<gleditor::Beams>(&device);
  beams_->createPipeline(gleditor::assetPath("shaders"),
                         gleditor::assetPath("shaders/vulkan"), true);
}

bool ZigzagVisualizer::busy() const {
  if (preflet_fetcher_.busy()) {
    return true;
  }
  for (const auto &[id, cell] : visible_cells_) {
    if (std::abs(cell.target_alpha - cell.current_alpha) > 0.05F ||
        glm::length(cell.target_pos - cell.current_pos) > 0.5F) {
      return true;
    }
  }
  return false;
}

void ZigzagVisualizer::populateFallbackStructure() {
  space_.clear();
  space_[1] = zzCell{.id         = 1,
                     .text_data  = "Root Focus Node",
                     .type       = "root",
                     .dimensions = {{"d.1", {2, 0}}, {"d.2", {3, 0}}},
                     .preflet    = std::nullopt};
  space_[2] = zzCell{.id         = 2,
                     .text_data  = "Horizontal Cell",
                     .type       = "item",
                     .dimensions = {{"d.1", {0, 1}}},
                     .preflet    = std::nullopt};
  space_[3] = zzCell{.id         = 3,
                     .text_data  = "Vertical Cell",
                     .type       = "item",
                     .dimensions = {{"d.2", {0, 1}}, {"d.3", {4, 0}}},
                     .preflet    = std::nullopt};
  space_[4] = zzCell{.id         = 4,
                     .text_data  = "Depth Layer Cell",
                     .type       = "detail",
                     .dimensions = {{"d.3", {0, 3}}},
                     .preflet    = std::nullopt};

  accursed_cell_focus_ = 1;
  current_view_        = ViewAxisBinding{"d.1", "d.2", "d.3"};
  structure_name_      = "Xanadu ZigZag Sample Structure";

  dimension_visuals_.clear();
  dimension_visuals_["d.1"] =
      DimensionVisual{glm::vec3{0.89F, 0.36F, 0.36F}, 240.0F, "Sequence"};
  dimension_visuals_["d.2"] =
      DimensionVisual{glm::vec3{0.35F, 0.76F, 0.48F}, 180.0F, "Detail"};
  dimension_visuals_["d.3"] =
      DimensionVisual{glm::vec3{0.31F, 0.62F, 0.88F}, 200.0F, "Reference"};

  scene_ = SceneVisual{
      .background  = glm::vec3{0.05F, 0.05F, 0.07F},
      .focus_color = glm::vec3{0.956F, 0.773F, 0.259F},
      .focus_scale = 1.4F,
      .cell_radius = 0.35F,
  };

  visible_cells_.clear();
  rebuildActiveViewTopology();
  for (auto &[id, cell] : visible_cells_) {
    cell.current_pos   = cell.target_pos;
    cell.current_alpha = cell.target_alpha;
  }
  invalidateAccessibility();
}

void ZigzagVisualizer::adoptDocument(ZzStructureDocument &&doc,
                                     std::string sourcePath) {
  structure_name_      = doc.meta.name.empty() ? sourcePath : doc.meta.name;
  current_slice_path_  = std::move(sourcePath);
  space_               = std::move(doc.cells);
  accursed_cell_focus_ = doc.focus;
  current_view_        = doc.view;

  scene_.background = glm::vec3{doc.scene.background.r, doc.scene.background.g,
                                doc.scene.background.b};
  scene_.focus_color =
      glm::vec3{doc.scene.focus_color.r, doc.scene.focus_color.g,
                doc.scene.focus_color.b};
  scene_.focus_scale = doc.scene.focus_scale;
  scene_.cell_radius = doc.scene.cell_radius;

  dimension_visuals_.clear();
  for (const auto &[dimName, meta] : doc.dimension_meta) {
    dimension_visuals_[dimName] = DimensionVisual{
        .color   = glm::vec3{meta.color.r, meta.color.g, meta.color.b},
        .spacing = meta.spacing * 100.0F,
        .label   = meta.label,
    };
  }

  visible_cells_.clear();
  rebuildActiveViewTopology();
  for (auto &[id, cell] : visible_cells_) {
    cell.current_pos   = cell.target_pos;
    cell.current_alpha = cell.target_alpha;
  }
  invalidateAccessibility();
}

void ZigzagVisualizer::adoptXuduStore(
    const xudu::Store &store,
    const std::vector<xudu::MicroversionId> &versions) {
  auto doc = projectStoreToZigzag(store, versions);
  adoptDocument(std::move(doc), "xudu_store");
}

void ZigzagVisualizer::adoptXuduDocs(const std::vector<XuduDocInput> &docs,
                                     const std::vector<xudu::Link> &links) {
  auto doc = projectXuduToZigzag(docs, links);
  adoptDocument(std::move(doc), "xudu_documents");
}

ZzRasterResult ZigzagVisualizer::rasterize(const DimID &primaryDim,
                                           const DimID &secondaryDim) const {
  const auto doc = document();
  return rasterizeZzStructure(doc, primaryDim, secondaryDim,
                              accursed_cell_focus_);
}

xudu::LinkPackage
ZigzagVisualizer::exportAsLinkPackage(const xudu::MutableKeys &keys,
                                      const std::string &salt,
                                      const std::int64_t sequence) const {
  const auto doc = document();
  return zzStructureToLinkPackage(doc, keys, salt, sequence);
}

ZzStructureDocument ZigzagVisualizer::document() const {
  ZzStructureDocument doc;
  doc.meta.name = structure_name_;
  doc.focus     = accursed_cell_focus_;
  doc.view      = current_view_;
  doc.cells     = space_;
  for (const auto &[dim, vis] : dimension_visuals_) {
    doc.dimension_meta[dim] = DimensionMeta{
        .label       = vis.label,
        .description = "",
        .color       = RgbColor{vis.color.r, vis.color.g, vis.color.b},
        .spacing     = vis.spacing / 100.0F,
    };
  }
  return doc;
}

CellID ZigzagVisualizer::createCell(std::string text, std::string type) {
  CellID newId = 1;
  for (const auto &[id, _] : space_) {
    if (id >= newId) {
      newId = id + 1;
    }
  }
  space_[newId] = zzCell{
      .id         = newId,
      .text_data  = std::move(text),
      .type       = std::move(type),
      .dimensions = {},
      .preflet    = std::nullopt,
  };
  if (accursed_cell_focus_ == 0) {
    accursed_cell_focus_ = newId;
  }
  rebuildActiveViewTopology();
  invalidateAccessibility();
  return newId;
}

bool ZigzagVisualizer::insertConnectedCell(std::string text,
                                           const DimID &dimension,
                                           const bool positive) {
  if (space_.empty() || accursed_cell_focus_ == 0) {
    createCell(std::move(text));
    return true;
  }
  const auto newId = createCell(std::move(text));
  if (positive) {
    const auto oldPos = space_[accursed_cell_focus_].dimensions[dimension].pos;
    space_[accursed_cell_focus_].dimensions[dimension].pos = newId;
    space_[newId].dimensions[dimension].neg = accursed_cell_focus_;
    if (oldPos != 0 && space_.contains(oldPos)) {
      space_[newId].dimensions[dimension].pos  = oldPos;
      space_[oldPos].dimensions[dimension].neg = newId;
    }
  } else {
    const auto oldNeg = space_[accursed_cell_focus_].dimensions[dimension].neg;
    space_[accursed_cell_focus_].dimensions[dimension].neg = newId;
    space_[newId].dimensions[dimension].pos = accursed_cell_focus_;
    if (oldNeg != 0 && space_.contains(oldNeg)) {
      space_[newId].dimensions[dimension].neg  = oldNeg;
      space_[oldNeg].dimensions[dimension].pos = newId;
    }
  }
  accursed_cell_focus_ = newId;
  rebuildActiveViewTopology();
  invalidateAccessibility();
  return true;
}

bool ZigzagVisualizer::linkFocusAlong(const DimID &dimension,
                                      const CellID targetId,
                                      const bool positive) {
  if (targetId == 0 || targetId == accursed_cell_focus_ ||
      !space_.contains(targetId) || !space_.contains(accursed_cell_focus_)) {
    return false;
  }
  if (positive) {
    space_[accursed_cell_focus_].dimensions[dimension].pos = targetId;
    space_[targetId].dimensions[dimension].neg = accursed_cell_focus_;
  } else {
    space_[accursed_cell_focus_].dimensions[dimension].neg = targetId;
    space_[targetId].dimensions[dimension].pos = accursed_cell_focus_;
  }
  rebuildActiveViewTopology();
  invalidateAccessibility();
  return true;
}

bool ZigzagVisualizer::unlinkFocusAlong(const DimID &dimension,
                                        const bool positive) {
  if (accursed_cell_focus_ == 0 || !space_.contains(accursed_cell_focus_)) {
    return false;
  }
  if (positive) {
    const auto targetId =
        space_[accursed_cell_focus_].dimensions[dimension].pos;
    if (targetId == 0) {
      return false;
    }
    space_[accursed_cell_focus_].dimensions[dimension].pos = 0;
    if (space_.contains(targetId)) {
      space_[targetId].dimensions[dimension].neg = 0;
    }
  } else {
    const auto targetId =
        space_[accursed_cell_focus_].dimensions[dimension].neg;
    if (targetId == 0) {
      return false;
    }
    space_[accursed_cell_focus_].dimensions[dimension].neg = 0;
    if (space_.contains(targetId)) {
      space_[targetId].dimensions[dimension].pos = 0;
    }
  }
  rebuildActiveViewTopology();
  invalidateAccessibility();
  return true;
}

void ZigzagVisualizer::updateFocusCellText(std::string text) {
  if (space_.contains(accursed_cell_focus_)) {
    zzcore::updateMasterText(space_, accursed_cell_focus_, std::move(text));
    rebuildActiveViewTopology();
    invalidateAccessibility();
  }
}

bool ZigzagVisualizer::saveStructureYaml(const std::string &filePath) const {
  const auto savePath = filePath.empty() ? current_slice_path_ : filePath;
  if (savePath.empty()) {
    return false;
  }
  const auto doc = document();
  return saveZzStructure(doc, savePath);
}

const zzCell *ZigzagVisualizer::findCell(const CellID id) const {
  return zzcore::findCell(space_, id);
}

LinkPairs ZigzagVisualizer::linksOn(const zzCell *const cell,
                                    const DimID &dimension) {
  return zzcore::linksOn(cell, dimension);
}

DimensionVisual
ZigzagVisualizer::dimensionVisual(const DimID &dimension) const {
  const auto it = dimension_visuals_.find(dimension);
  if (it != dimension_visuals_.end()) {
    return it->second;
  }
  return DimensionVisual{
      .color   = glm::vec3{0.7F, 0.7F, 0.75F},
      .spacing = 200.0F,
      .label   = dimension,
  };
}

glm::vec3 ZigzagVisualizer::tintForPreflet(const glm::vec3 base,
                                           const bool hasPreflet) {
  if (!hasPreflet) {
    return base;
  }
  const glm::vec3 portalColor{0.61F, 0.35F, 0.71F};
  return base * 0.55F + portalColor * 0.45F;
}

void ZigzagVisualizer::rebuildActiveViewTopology() {
  for (auto &[id, render_cell] : visible_cells_) {
    render_cell.target_alpha = 0.0F;
  }

  const zzCell *const focus = findCell(accursed_cell_focus_);
  const bool focusIsClone   = zzcore::isCloneCell(space_, accursed_cell_focus_);
  const CellID focusMaster =
      zzcore::findCloneMaster(space_, accursed_cell_focus_);
  const auto focusText =
      zzcore::getEffectiveCellText(space_, accursed_cell_focus_);

  if (!visible_cells_.contains(accursed_cell_focus_)) {
    visible_cells_[accursed_cell_focus_] = RenderStateCell{
        .id              = accursed_cell_focus_,
        .text            = std::string{focusText},
        .type            = focus ? focus->type : "",
        .has_preflet     = focus && focus->preflet.has_value(),
        .is_clone        = focusIsClone,
        .clone_master_id = focusMaster,
    };
  } else {
    visible_cells_[accursed_cell_focus_].text     = std::string{focusText};
    visible_cells_[accursed_cell_focus_].is_clone = focusIsClone;
    visible_cells_[accursed_cell_focus_].clone_master_id = focusMaster;
  }

  auto &focusRenderState        = visible_cells_[accursed_cell_focus_];
  focusRenderState.target_pos   = glm::vec3{0.0F, 0.0F, 0.0F};
  focusRenderState.target_alpha = 1.0F;
  focusRenderState.base_color =
      tintForPreflet(scene_.focus_color, focus && focus->preflet.has_value());

  auto mapNeighbor = [&](const CellID parentId, const CellID childId,
                         const glm::vec3 &offset, const glm::vec3 &axisColor) {
    if (childId == 0) {
      return;
    }
    const zzCell *const child = findCell(childId);
    const bool childIsClone   = zzcore::isCloneCell(space_, childId);
    const CellID childMaster  = zzcore::findCloneMaster(space_, childId);
    const auto childText      = zzcore::getEffectiveCellText(space_, childId);

    if (!visible_cells_.contains(childId)) {
      RenderStateCell newCell{
          .id              = childId,
          .text            = std::string{childText},
          .type            = child ? child->type : "",
          .has_preflet     = child && child->preflet.has_value(),
          .is_clone        = childIsClone,
          .clone_master_id = childMaster,
          .current_pos     = visible_cells_[parentId].current_pos,
      };
      visible_cells_[childId] = newCell;
    } else {
      visible_cells_[childId].text            = std::string{childText};
      visible_cells_[childId].is_clone        = childIsClone;
      visible_cells_[childId].clone_master_id = childMaster;
    }

    auto &childCell        = visible_cells_[childId];
    childCell.target_pos   = visible_cells_[parentId].target_pos + offset;
    childCell.target_alpha = 1.0F;
    childCell.base_color =
        tintForPreflet(axisColor, child && child->preflet.has_value());
  };

  if (focus) {
    const LinkPairs xLinks = linksOn(focus, current_view_.x_dimension);
    const LinkPairs yLinks = linksOn(focus, current_view_.y_dimension);
    const LinkPairs zLinks = linksOn(focus, current_view_.z_dimension);

    const DimensionVisual xVisual = dimensionVisual(current_view_.x_dimension);
    const DimensionVisual yVisual = dimensionVisual(current_view_.y_dimension);
    const DimensionVisual zVisual = dimensionVisual(current_view_.z_dimension);

    mapNeighbor(accursed_cell_focus_, xLinks.pos,
                glm::vec3{xVisual.spacing, 0.0F, 0.0F}, xVisual.color);
    mapNeighbor(accursed_cell_focus_, xLinks.neg,
                glm::vec3{-xVisual.spacing, 0.0F, 0.0F}, xVisual.color);

    mapNeighbor(accursed_cell_focus_, yLinks.pos,
                glm::vec3{0.0F, yVisual.spacing, 0.0F}, yVisual.color);
    mapNeighbor(accursed_cell_focus_, yLinks.neg,
                glm::vec3{0.0F, -yVisual.spacing, 0.0F}, yVisual.color);

    mapNeighbor(accursed_cell_focus_, zLinks.pos,
                glm::vec3{0.0F, 0.0F, zVisual.spacing}, zVisual.color);
    mapNeighbor(accursed_cell_focus_, zLinks.neg,
                glm::vec3{0.0F, 0.0F, -zVisual.spacing}, zVisual.color);
  }
}

void ZigzagVisualizer::updateCellPositions(const float rawDeltaTime) {
  const float deltaTime       = std::clamp(rawDeltaTime, 1.0F / 120.0F, 0.1F);
  constexpr float layoutSpeed = 12.0F;
  constexpr float alphaSpeed  = 8.0F;

  const float spatialFactor = 1.0F - std::exp(-layoutSpeed * deltaTime);
  const float alphaFactor   = 1.0F - std::exp(-alphaSpeed * deltaTime);

  for (auto &[id, cell] : visible_cells_) {
    cell.current_pos += (cell.target_pos - cell.current_pos) * spatialFactor;
    cell.current_alpha +=
        (cell.target_alpha - cell.current_alpha) * alphaFactor;

    if (glm::length(cell.target_pos - cell.current_pos) < 0.05F) {
      cell.current_pos = cell.target_pos;
    }
    if (std::abs(cell.target_alpha - cell.current_alpha) < 0.01F) {
      cell.current_alpha = cell.target_alpha;
    }
  }

  std::erase_if(visible_cells_, [](const auto &pair) {
    return pair.second.target_alpha <= 0.0F &&
           pair.second.current_alpha < 0.01F;
  });
}

void ZigzagVisualizer::pollPrefletFetch() {
  if (!fetchingEnabled_) {
    return;
  }
  const auto before = preflet_fetcher_.progress().status;
  preflet_fetcher_.poll();
  const auto &progress = preflet_fetcher_.progress();

  if (progress.status == PrefletFetcher::Status::Ready) {
    const std::string path = progress.slice_path;
    preflet_fetcher_.acknowledge();
    if (auto doc = loadZzStructure(path)) {
      if (!current_slice_path_.empty()) {
        slice_stack_.push_back(current_slice_path_);
      }
      adoptDocument(std::move(*doc), path);
    }
  } else if (progress.status != before) {
    invalidateAccessibility();
  }
}

void ZigzagVisualizer::navigateFocus(const DimID &dimension,
                                     const bool positive) {
  const zzCell *const cell = findCell(accursed_cell_focus_);
  if (!cell) {
    return;
  }
  const LinkPairs links = linksOn(cell, dimension);
  const CellID next     = positive ? links.pos : links.neg;
  if (next == 0) {
    return;
  }

  accursed_cell_focus_ = next;
  rebuildActiveViewTopology();
  invalidateAccessibility();
}

void ZigzagVisualizer::navigateFocusTo(const CellID id) {
  if (id == 0 || !space_.contains(id)) {
    return;
  }
  accursed_cell_focus_ = id;
  rebuildActiveViewTopology();
  invalidateAccessibility();
}

void ZigzagVisualizer::swapDimensions(const int axis1, const int axis2) {
  std::array<DimID *, 3> dims = {&current_view_.x_dimension,
                                 &current_view_.y_dimension,
                                 &current_view_.z_dimension};
  if (axis1 >= 0 && axis1 < 3 && axis2 >= 0 && axis2 < 3 && axis1 != axis2) {
    std::swap(*dims[axis1], *dims[axis2]);
    rebuildActiveViewTopology();
    invalidateAccessibility();
  }
}

void ZigzagVisualizer::cycleDimensions(const bool forward) {
  if (forward) {
    const DimID tmp           = current_view_.x_dimension;
    current_view_.x_dimension = current_view_.y_dimension;
    current_view_.y_dimension = current_view_.z_dimension;
    current_view_.z_dimension = tmp;
  } else {
    const DimID tmp           = current_view_.z_dimension;
    current_view_.z_dimension = current_view_.y_dimension;
    current_view_.y_dimension = current_view_.x_dimension;
    current_view_.x_dimension = tmp;
  }
  rebuildActiveViewTopology();
  invalidateAccessibility();
}

void ZigzagVisualizer::followPrefletAtFocus() {
  const zzCell *const cell = findCell(accursed_cell_focus_);
  if (!cell || !cell->preflet) {
    return;
  }
  if (!fetchingEnabled_) {
    std::cerr << "preflet: fetching is disabled (--no-fetch)\n";
    return;
  }
  if (preflet_fetcher_.busy()) {
    return;
  }

  std::string error;
  preflet_fetcher_.acknowledge();
  if (!preflet_fetcher_.begin(*cell->preflet, error)) {
    std::cerr << std::format("preflet: fetch error -- {}\n", error);
  }
}

void ZigzagVisualizer::returnToPreviousSlice() {
  if (slice_stack_.empty()) {
    return;
  }
  const std::string prev = slice_stack_.back();
  slice_stack_.pop_back();
  if (auto doc = loadZzStructure(prev)) {
    adoptDocument(std::move(*doc), prev);
  }
}

void ZigzagVisualizer::cancelPrefletFetch() {
  if (preflet_fetcher_.busy()) {
    preflet_fetcher_.cancel();
  }
}

bool ZigzagVisualizer::picked(const render::PickingResult &pick,
                              RenderState &) {
  if (pick.tag.kind == render::tagKindOverlay && pick.tag.clusterIndex != 0) {
    const auto targetId = static_cast<CellID>(pick.tag.clusterIndex);
    if (space_.contains(targetId)) {
      navigateFocusTo(targetId);
      return true;
    }
  }
  return false;
}

void ZigzagVisualizer::drawFrame(gleditor::FrameContext &ctx) {
  const auto now = std::chrono::steady_clock::now();
  const float deltaTime =
      std::chrono::duration<float>(now - last_frame_time_).count();
  last_frame_time_ = now;

  pollPrefletFetch();
  updateCellPositions(deltaTime);

  if (!beams_ || !worldCanvas_ || !hudCanvas_) {
    return;
  }

  // --- 1. Draw 3D Connection Beams ---
  beams_->clear();
  std::vector<std::pair<CellID, CellID>> drawnEdges;

  for (const auto &[id, cell] : visible_cells_) {
    const zzCell *const spaceCell = findCell(id);
    if (!spaceCell) {
      continue;
    }

    for (const auto &[dimName, links] : spaceCell->dimensions) {
      const DimensionVisual visual = dimensionVisual(dimName);
      for (const CellID neighborId : {links.pos, links.neg}) {
        if (neighborId == 0 || neighborId == id) {
          continue;
        }
        if (!visible_cells_.contains(neighborId)) {
          continue;
        }

        const auto edge =
            std::pair{std::min(id, neighborId), std::max(id, neighborId)};
        if (std::ranges::find(drawnEdges, edge) != drawnEdges.end()) {
          continue;
        }
        drawnEdges.push_back(edge);

        const auto &neighborCell = visible_cells_.at(neighborId);
        const float edgeAlpha =
            std::min(cell.current_alpha, neighborCell.current_alpha);
        const std::uint32_t col =
            packRgba(visual.color.r, visual.color.g, visual.color.b, edgeAlpha);

        beams_->add(cell.current_pos, neighborCell.current_pos, 4.0F, col,
                    static_cast<std::uint32_t>(id));
      }
    }
  }

  if (beams_->pending() > 0) {
    beams_->commit();
    beams_->draw(ctx.state, ctx.viewProjection, 1.0F, 0);
  }

  // --- 2. Draw 3D Cell Nodes & Text ---
  worldCanvas_->clear();

  for (const auto &[id, cell] : visible_cells_) {
    if (cell.current_alpha < 0.02F) {
      continue;
    }

    worldCanvas_->setTag(render::tagKindOverlay,
                         static_cast<std::uint32_t>(id));

    const bool isFocus     = (id == accursed_cell_focus_);
    const float nodeWidth  = isFocus ? 160.0F : 120.0F;
    const float nodeHeight = isFocus ? 70.0F : 50.0F;

    const float left   = cell.current_pos.x - (nodeWidth / 2.0F);
    const float bottom = cell.current_pos.y - (nodeHeight / 2.0F);

    const std::uint32_t bgCol =
        packRgba(cell.base_color.r * 0.25F, cell.base_color.g * 0.25F,
                 cell.base_color.b * 0.25F, cell.current_alpha);
    const std::uint32_t borderCol =
        packRgba(cell.base_color.r, cell.base_color.g, cell.base_color.b,
                 cell.current_alpha);
    const std::uint32_t textCol =
        isFocus ? 0xFFFFFFFFU : packRgba(0.9F, 0.9F, 0.9F, cell.current_alpha);

    // Node Box Body
    worldCanvas_->addRect(left, bottom, nodeWidth, nodeHeight, bgCol);

    // Node Border
    constexpr float borderThick = 2.0F;
    worldCanvas_->addLine(left, bottom, left + nodeWidth, bottom, borderThick,
                          borderCol);
    worldCanvas_->addLine(left + nodeWidth, bottom, left + nodeWidth,
                          bottom + nodeHeight, borderThick, borderCol);
    worldCanvas_->addLine(left + nodeWidth, bottom + nodeHeight, left,
                          bottom + nodeHeight, borderThick, borderCol);
    worldCanvas_->addLine(left, bottom + nodeHeight, left, bottom, borderThick,
                          borderCol);

    // Title / ID
    const std::string idText = std::format("#{}", id);
    worldCanvas_->addText(ctx.state, left + 6.0F, bottom + nodeHeight - 6.0F,
                          idText, borderCol, bgCol);

    // Label Text
    const std::string textPreview = shortenText(cell.text, isFocus ? 18 : 12);
    worldCanvas_->addText(ctx.state, left + 6.0F, bottom + nodeHeight - 26.0F,
                          textPreview, textCol, bgCol);

    // Badges: type, clone, & preflet
    if (!cell.type.empty() || cell.is_clone || cell.has_preflet) {
      std::string badge;
      if (!cell.type.empty()) {
        badge += "[" + cell.type + "] ";
      }
      if (cell.is_clone) {
        badge += std::format("[clone #{}] ", cell.clone_master_id);
      }
      if (cell.has_preflet) {
        badge += "-> [preflet]";
      }
      worldCanvas_->addText(ctx.state, left + 6.0F, bottom + 16.0F, badge,
                            borderCol, bgCol);
    }
  }

  worldCanvas_->commit();
  worldCanvas_->draw(ctx.state, ctx.viewProjection, 1.0F);

  // --- 3. Draw 2D Screen Overlay HUD ---
  hudCanvas_->clear();
  hudCanvas_->setTag(render::tagKindNone, 0);

  const auto width  = static_cast<float>(ctx.screenWidth);
  const auto height = static_cast<float>(ctx.screenHeight);

  // Top Bar Background
  hudCanvas_->addRect(0.0F, height - 60.0F, width, 60.0F, 0x0D0D12DDU);
  hudCanvas_->addLine(0.0F, height - 60.0F, width, height - 60.0F, 1.0F,
                      0x333344FFU);

  // Structure Name
  hudCanvas_->addText(ctx.state, 16.0F, height - 12.0F, structure_name_,
                      0xF4C542FFU, 0x0D0D12DDU);

  // Focus Status
  std::string focusLabel = "Focus: none";
  if (const zzCell *const cur = findCell(accursed_cell_focus_)) {
    const auto effText =
        zzcore::getEffectiveCellText(space_, accursed_cell_focus_);
    focusLabel = std::format("Focus: #{} \"{}\" {}", cur->id, effText,
                             cur->type.empty() ? "" : "[" + cur->type + "]");
    if (zzcore::isCloneCell(space_, accursed_cell_focus_)) {
      focusLabel +=
          std::format(" [clone of #{}]",
                      zzcore::findCloneMaster(space_, accursed_cell_focus_));
    }
    if (cur->preflet) {
      focusLabel += " -> (Preflet link attached)";
    }
  }
  hudCanvas_->addText(ctx.state, 16.0F, height - 34.0F, focusLabel, 0xFFFFFFFFU,
                      0x0D0D12DDU);

  // Dimension Bindings on Top Right
  const DimensionVisual xVis = dimensionVisual(current_view_.x_dimension);
  const DimensionVisual yVis = dimensionVisual(current_view_.y_dimension);
  const DimensionVisual zVis = dimensionVisual(current_view_.z_dimension);

  const std::string dimsInfo =
      std::format("[X: {}] [Y: {}] [Z: {}]",
                  xVis.label.empty() ? current_view_.x_dimension : xVis.label,
                  yVis.label.empty() ? current_view_.y_dimension : yVis.label,
                  zVis.label.empty() ? current_view_.z_dimension : zVis.label);

  const auto dimsMetrics = hudCanvas_->measureText(dimsInfo);
  hudCanvas_->addText(ctx.state, width - dimsMetrics.width - 16.0F,
                      height - 12.0F, dimsInfo, 0x70B0FFFFU, 0x0D0D12DDU);

  // Preflet Fetch Progress Banner
  const auto &fetch = preflet_fetcher_.progress();
  if (fetch.status == PrefletFetcher::Status::Fetching) {
    const std::string fetchMsg =
        std::format("Fetching Slice: {:.0f}% -- {}", fetch.fraction * 100.0F,
                    fetch.message);
    hudCanvas_->addRect(0.0F, height - 90.0F, width, 30.0F, 0x332244EEU);
    hudCanvas_->addText(ctx.state, 16.0F, height - 68.0F, fetchMsg, 0xE080FFFFU,
                        0x332244EEU);
  } else if (fetch.status == PrefletFetcher::Status::Failed) {
    const std::string failMsg =
        std::format("Preflet Fetch Failed: {}", fetch.message);
    hudCanvas_->addRect(0.0F, height - 90.0F, width, 30.0F, 0x551111EEU);
    hudCanvas_->addText(ctx.state, 16.0F, height - 68.0F, failMsg, 0xFF8888FFU,
                        0x551111EEU);
  }

  // Bottom Command Key Hints
  hudCanvas_->addRect(0.0F, 0.0F, width, 28.0F, 0x0D0D12DDU);
  hudCanvas_->addLine(0.0F, 28.0F, width, 28.0F, 1.0F, 0x222233FFU);
  const std::string hints =
      "Arrows: Step X/Y | PgUp/PgDn: Step Z | Space: Swap X/Y | Tab: Cycle | "
      "Enter: Follow Preflet | Bksp: Back | R: Reset View";
  hudCanvas_->addText(ctx.state, 16.0F, 22.0F, hints, 0x888899FFU, 0x0D0D12DDU);

  hudCanvas_->commit();
  const glm::mat4 ortho = glm::ortho(0.0F, width, 0.0F, height, -1.0F, 1.0F);
  hudCanvas_->draw(ctx.state, ortho, 1.0F);
}

void ZigzagVisualizer::describe(gleditor::a11y::Builder &into) {
  std::vector<std::uint64_t> rootChildren;

  auto &dimsNode = into.add(2, gleditor::a11y::Role::Group);
  dimsNode.label = "Active View Dimensions: X=" + current_view_.x_dimension +
                   ", Y=" + current_view_.y_dimension +
                   ", Z=" + current_view_.z_dimension;
  rootChildren.push_back(into.id(2));

  std::uint64_t nextNodeId = 10;
  for (const auto &[id, cell] : space_) {
    const bool isFocus = (id == accursed_cell_focus_);
    const auto effText = zzcore::getEffectiveCellText(space_, id);
    std::string desc   = std::format("Cell #{}: {}", id, effText);
    if (!cell.type.empty()) {
      desc += " [" + cell.type + "]";
    }
    if (zzcore::isCloneCell(space_, id)) {
      desc +=
          std::format(" [Clone of #{}]", zzcore::findCloneMaster(space_, id));
    }
    if (isFocus) {
      desc += " (Focused)";
    }
    if (cell.preflet) {
      desc += " (Outbound Preflet: " + cell.preflet->resource_identifier + ")";
    }

    auto &cellNode   = into.add(nextNodeId, gleditor::a11y::Role::ListItem);
    cellNode.label   = std::move(desc);
    cellNode.actions = gleditor::a11y::bit(gleditor::a11y::Action::Click) |
                       gleditor::a11y::bit(gleditor::a11y::Action::Focus);
    rootChildren.push_back(into.id(nextNodeId));

    if (isFocus) {
      into.takeFocus(into.id(nextNodeId));
    }

    nextNodeId++;
  }

  auto &rootNode    = into.add(1, gleditor::a11y::Role::Group);
  rootNode.label    = "Xanadu ZigZag: " + structure_name_;
  rootNode.children = std::move(rootChildren);
  into.contribute(into.id(1));
}

bool ZigzagVisualizer::performAction(const std::uint64_t nodeId,
                                     const gleditor::a11y::Action action,
                                     const std::string_view) {
  if (action == gleditor::a11y::Action::Click ||
      action == gleditor::a11y::Action::Focus) {
    const auto localId = gleditor::a11y::Ids::localOf(nodeId);
    if (localId >= 10) {
      const auto cellIndex = localId - 10;
      if (cellIndex < space_.size()) {
        auto it = space_.begin();
        std::advance(it, cellIndex);
        navigateFocusTo(it->first);
        return true;
      }
    }
  }
  return false;
}

} // namespace zigzag
