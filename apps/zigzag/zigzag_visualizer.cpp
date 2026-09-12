/**
 * @file zigzag_visualizer.cpp
 * @brief Implementation of the Xanadu ZigZag visualizer on gleditor.
 */
#include "zigzag_visualizer.hpp"
#include "core/zzcore.hpp"
#include "core/zzstructure_loader.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
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

ZigzagVisualizer::ZigzagVisualizer(std::string aFontName)
    : fontName_(std::move(aFontName)),
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

  imageCache_ = std::make_unique<gleditor::ImageCache>(&device);
}

bool ZigzagVisualizer::busy() const {
  for (const auto &[id, cell] : visible_cells_) {
    if (std::abs(cell.target_alpha - cell.current_alpha) > 0.05F ||
        glm::length(cell.target_pos - cell.current_pos) > 0.5F) {
      return true;
    }
  }
  return false;
}

void ZigzagVisualizer::populateFallbackStructure() {
  ZzStructureDocument doc;
  doc.meta.name = "Xanadu ZigZag Sample Structure";
  doc.focus     = 1;
  doc.view      = ViewAxisBinding{"d.1", "d.2", "d.3"};

  Cell c1;
  c1.id         = 1;
  c1.data       = std::string{"Root Focus Node"};
  c1.role       = "root";
  c1.dimensions = {{"d.1", {2, 0}}, {"d.2", {3, 0}}};
  doc.cells[1]  = std::move(c1);

  Cell c2;
  c2.id         = 2;
  c2.data       = std::string{"Horizontal Cell"};
  c2.role       = "item";
  c2.dimensions = {{"d.1", {0, 1}}};
  doc.cells[2]  = std::move(c2);

  Cell c3;
  c3.id         = 3;
  c3.data       = std::string{"Vertical Cell"};
  c3.role       = "item";
  c3.dimensions = {{"d.2", {0, 1}}, {"d.3", {4, 0}}};
  doc.cells[3]  = std::move(c3);

  Cell c4;
  c4.id         = 4;
  c4.data       = std::string{"Depth Layer Cell"};
  c4.role       = "detail";
  c4.dimensions = {{"d.3", {0, 3}}};
  doc.cells[4]  = std::move(c4);

  DimensionMeta dm1;
  dm1.label                 = "Sequence";
  dm1.color                 = RgbColor{0.89F, 0.36F, 0.36F};
  dm1.spacing               = 2.4F;
  doc.dimension_meta["d.1"] = std::move(dm1);

  DimensionMeta dm2;
  dm2.label                 = "Detail";
  dm2.color                 = RgbColor{0.35F, 0.76F, 0.48F};
  dm2.spacing               = 1.8F;
  doc.dimension_meta["d.2"] = std::move(dm2);

  DimensionMeta dm3;
  dm3.label                 = "Reference";
  dm3.color                 = RgbColor{0.31F, 0.62F, 0.88F};
  dm3.spacing               = 2.0F;
  doc.dimension_meta["d.3"] = std::move(dm3);

  adoptDocument(std::move(doc), "fallback");
}

void ZigzagVisualizer::adoptDocument(ZzStructureDocument &&doc,
                                     std::string sourcePath) {
  structure_name_     = doc.meta.name.empty() ? sourcePath : doc.meta.name;
  current_slice_path_ = std::move(sourcePath);
  current_view_       = doc.view;

  scene_.background = glm::vec3{doc.scene.background.r, doc.scene.background.g,
                                doc.scene.background.b};
  scene_.focus_color =
      glm::vec3{doc.scene.focus_color.r, doc.scene.focus_color.g,
                doc.scene.focus_color.b};
  scene_.focus_scale         = doc.scene.focus_scale;
  scene_.cell_radius         = doc.scene.cell_radius;
  scene_.layout_speed        = doc.scene.layout_speed;
  scene_.alpha_speed         = doc.scene.alpha_speed;
  scene_.border_thickness    = doc.scene.border_thickness;
  scene_.neighborhood_radius = doc.scene.neighborhood_radius;

  dimension_visuals_.clear();
  for (const auto &[dimName, meta] : doc.dimension_meta) {
    dimension_visuals_[dimName] = DimensionVisual{
        .color   = glm::vec3{meta.color.r, meta.color.g, meta.color.b},
        .spacing = meta.spacing * 100.0F,
        .label   = meta.label,
    };
  }

  store_            = std::make_unique<xanadu::Store>();
  const auto sliced = sliceToStore(doc, *store_);
  engine_           = std::make_unique<UnifiedTransclusionEngine>(*store_);

  // The dimensions this slice is navigated along, minted here because adopting
  // a document is the moment a document is *built* -- the one place a write
  // belongs. Navigation used to mint them on first use, which meant pressing an
  // arrow key, or drawing a frame, appended operations to the document; see
  // dimensionRef() and R8. d.clone is included because clone navigation is a
  // well-known dimension a slice is expected to have, not something a keypress
  // should invent.
  for (const auto &wellKnown :
       {current_view_.x_dimension, current_view_.y_dimension,
        current_view_.z_dimension, DimID{"d.clone"}}) {
    if (!wellKnown.empty()) {
      static_cast<void>(engine_->dimensionFor(wellKnown));
    }
  }

  accursed_cell_focus_ = sliced.focus;
  if (accursed_cell_focus_ == 0 ||
      !engine_->findCell(static_cast<CellRef>(accursed_cell_focus_))) {
    accursed_cell_focus_ = engine_->manifold().home();
    if (accursed_cell_focus_ == 0 && engine_->manifold().cellCount() > 0) {
      accursed_cell_focus_ = engine_->manifold().cells().front().birthOp;
    }
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
    const xanadu::Store &store,
    const std::vector<xanadu::MicroversionId> &versions) {
  auto doc = projectStoreToZigzag(store, versions);
  adoptDocument(std::move(doc), "xudu_store");
}

void ZigzagVisualizer::adoptXuduDocs(const std::vector<XuduDocInput> &docs,
                                     const std::vector<xanadu::Link> &links) {
  auto doc = projectXuduToZigzag(docs, links);
  adoptDocument(std::move(doc), "xudu_documents");
}

ZzRasterResult ZigzagVisualizer::rasterize(const DimID &primaryDim,
                                           const DimID &secondaryDim) const {
  const auto doc = document();
  return rasterizeZzStructure(doc, primaryDim, secondaryDim,
                              accursed_cell_focus_);
}

xanadu::LinkPackage
ZigzagVisualizer::exportAsLinkPackage(const xanadu::MutableKeys &keys,
                                      const std::string &salt,
                                      const std::int64_t sequence) const {
  if (!engine_ || !store_) {
    return {};
  }
  return storeToLinkPackage(*store_, engine_->manifold(), keys, salt, sequence,
                            structure_name_);
}

ZzStructureDocument ZigzagVisualizer::document() const {
  if (!engine_ || !store_) {
    return {};
  }
  auto doc      = storeToSlice(*store_, engine_->manifold(),
                               static_cast<CellRef>(accursed_cell_focus_));
  doc.meta.name = structure_name_;
  doc.view      = current_view_;
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

CellID ZigzagVisualizer::createCell(std::string text, std::string role) {
  if (!engine_) {
    return 0;
  }
  const CellRef newId = engine_->addCell(text);
  if (!role.empty()) {
    const auto roleDim = engine_->dimensionFor("d.role");
    const auto attrRef = engine_->addCell(role);
    engine_->linkCells(newId, attrRef, roleDim, DimVector::POS);
  }
  if (accursed_cell_focus_ == 0) {
    accursed_cell_focus_ = newId;
  }
  rebuildActiveViewTopology();
  invalidateAccessibility();
  return newId;
}

bool ZigzagVisualizer::insertConnectedCell(std::string text,
                                           const DimID &dimension,
                                           const DimVector dir) {
  if (!engine_ || accursed_cell_focus_ == 0) {
    createCell(std::move(text));
    return true;
  }
  if (dimension == "d.dims") {
    return false;
  }
  const auto focus = static_cast<CellRef>(accursed_cell_focus_);
  if (isEphemeral(focus)) {
    return false;
  }
  const auto newId  = static_cast<CellRef>(createCell(std::move(text)));
  const auto dimRef = engine_->dimensionFor(dimension);

  const auto oldNeighbor = engine_->manifold().linked(focus, dimRef, dir);
  engine_->linkCells(focus, newId, dimRef, dir);
  if (oldNeighbor != zigzag::noCell) {
    engine_->linkCells(newId, oldNeighbor, dimRef, dir);
  }
  accursed_cell_focus_ = newId;
  rebuildActiveViewTopology();
  invalidateAccessibility();
  return true;
}

bool ZigzagVisualizer::linkFocusAlong(const DimID &dimension,
                                      const CellID targetId,
                                      const DimVector dir) {
  if (!engine_ || targetId == 0 || targetId == accursed_cell_focus_) {
    return false;
  }
  const auto focus  = static_cast<CellRef>(accursed_cell_focus_);
  const auto target = static_cast<CellRef>(targetId);
  if (!engine_->findCell(target) || !engine_->findCell(focus) ||
      isEphemeral(target) || isEphemeral(focus)) {
    return false;
  }
  // Protection against system instability: d.dims links cannot be altered
  // manually
  if (dimension == "d.dims") {
    return false;
  }
  const auto dimRef = engine_->dimensionFor(dimension);
  engine_->linkCells(focus, target, dimRef, dir);
  rebuildActiveViewTopology();
  invalidateAccessibility();
  return true;
}

bool ZigzagVisualizer::unlinkFocusAlong(const DimID &dimension,
                                        const DimVector dir) {
  if (!engine_ || accursed_cell_focus_ == 0) {
    return false;
  }
  const auto focus = static_cast<CellRef>(accursed_cell_focus_);
  if (!engine_->findCell(focus) || isEphemeral(focus)) {
    return false;
  }
  const auto dimRef =
      engine_->manifold().dimensionNamed(dimension, engine_->store());
  if (dimRef == zigzag::noCell) {
    return false;
  }
  // Protection against system instability: d.dims links cannot be unlinked
  if (dimRef == engine_->manifold().dimsDimension() || dimension == "d.dims") {
    return false;
  }
  const auto target = engine_->linked(focus, dimRef, dir);
  if (target == zigzag::noCell || isEphemeral(target)) {
    return false;
  }
  engine_->linkCells(focus, zigzag::noCell, dimRef, dir);
  rebuildActiveViewTopology();
  invalidateAccessibility();
  return true;
}

bool ZigzagVisualizer::deleteFocusCell() {
  if (!engine_ || accursed_cell_focus_ == 0) {
    return false;
  }
  const auto focus = static_cast<CellRef>(accursed_cell_focus_);
  if (isProtected(focus)) {
    return false; // Protected from deletion!
  }
  const auto linkSpan = engine_->manifold().dimensionsOf(focus);
  const std::vector<DimLink> links(linkSpan.begin(), linkSpan.end());

  CellRef nextFocus = zigzag::noCell;

  // 1) Prioritize adjacent cells along active view dimensions (X, Y, Z)
  const std::array<DimID, 3> viewDims = {current_view_.x_dimension,
                                         current_view_.y_dimension,
                                         current_view_.z_dimension};
  for (const auto &dimId : viewDims) {
    const auto dimRef = dimensionRef(dimId);
    if (dimRef == zigzag::noCell) {
      continue;
    }
    const auto posNeighbor =
        engine_->manifold().linked(focus, dimRef, DimVector::POS);
    if (posNeighbor != zigzag::noCell && posNeighbor != focus &&
        !isProtected(posNeighbor) && !isEphemeral(posNeighbor)) {
      nextFocus = posNeighbor;
      break;
    }
    const auto negNeighbor =
        engine_->manifold().linked(focus, dimRef, DimVector::NEG);
    if (negNeighbor != zigzag::noCell && negNeighbor != focus &&
        !isProtected(negNeighbor) && !isEphemeral(negNeighbor)) {
      nextFocus = negNeighbor;
      break;
    }
  }

  // 2) Fallback to any connected cell on non-metadata dimensions
  if (nextFocus == zigzag::noCell) {
    for (const auto &link : links) {
      const auto dimName =
          engine_->manifold().textOf(link.dim, engine_->store());
      if (dimName == "d.role" || dimName == "d.mime" || dimName == "d.media" ||
          dimName == "d.dims") {
        continue;
      }
      if (link.pos != zigzag::noCell && link.pos != focus &&
          !isProtected(link.pos) && !isEphemeral(link.pos)) {
        nextFocus = link.pos;
        break;
      }
      if (link.neg != zigzag::noCell && link.neg != focus &&
          !isProtected(link.neg) && !isEphemeral(link.neg)) {
        nextFocus = link.neg;
        break;
      }
    }
  }

  // 3) Ultimate fallback: home cell
  if (nextFocus == zigzag::noCell) {
    nextFocus = engine_->manifold().home();
  }

  // Splice around focus and unlink it, in as few operations as the fold allows.
  //
  // One operation per dimension, not three. Joining the two neighbours already
  // unlinks the cell between them: a link is one edge, so setting neg's posward
  // to pos displaces this cell from *both* its sides -- the fold clears the
  // negward side of what neg used to point at and the posward side of what pos
  // used to point back to, and both of those are this cell. Following that with
  // an explicit clear restates a link that is already clear, and linkCells()
  // has no idempotence guard, so each of those was a dead operation in an
  // append-only spool. Deleting a cell on two dimensions recorded four
  // operations where two carried the change.
  for (const auto &link : links) {
    if (link.neg != zigzag::noCell && link.pos != zigzag::noCell) {
      // A rank of exactly two loses one and becomes a cell linked to itself,
      // which is a degenerate rank rather than a broken one.
      engine_->linkCells(link.neg, link.pos, link.dim, DimVector::POS);
      continue;
    }
    if (link.pos != zigzag::noCell) {
      engine_->linkCells(focus, zigzag::noCell, link.dim, DimVector::POS);
    } else if (link.neg != zigzag::noCell) {
      engine_->linkCells(focus, zigzag::noCell, link.dim, DimVector::NEG);
    }
  }
  accursed_cell_focus_ = nextFocus;
  rebuildActiveViewTopology();
  invalidateAccessibility();
  return true;
}

void ZigzagVisualizer::updateFocusCellText(std::string text) {
  if (!engine_ || accursed_cell_focus_ == 0) {
    return;
  }
  const auto focus = static_cast<CellRef>(accursed_cell_focus_);
  if (!engine_->findCell(focus)) {
    return;
  }
  CellRef targetCell = focus;
  const auto cloneDim =
      engine_->manifold().dimensionNamed("d.clone", engine_->store());
  if (cloneDim != zigzag::noCell) {
    targetCell = engine_->cloneMaster(focus, cloneDim);
  }
  if ((targetCell == engine_->manifold().home() ||
       targetCell == engine_->manifold().dimsDimension()) &&
      text.empty()) {
    return;
  }
  engine_->updateCellText(targetCell, text);
  rebuildActiveViewTopology();
  invalidateAccessibility();
}

bool ZigzagVisualizer::saveStructureYaml(const std::string &filePath) const {
  const auto savePath = filePath.empty() ? current_slice_path_ : filePath;
  if (savePath.empty()) {
    return false;
  }
  const auto doc = document();
  return saveZzStructure(doc, savePath);
}

std::size_t ZigzagVisualizer::operationCount() const {
  return engine_ ? engine_->store().opCount() : 0;
}

DimRef ZigzagVisualizer::dimensionRef(const DimID &name) const {
  if (!engine_) {
    return zigzag::noCell;
  }
  // d.meta-dims is derived and is stored in no operation (R12), so it is a
  // sentinel rather than something to look up or mint.
  if (name == "d.meta-dims") {
    return UnifiedTransclusionEngine::metaDimension();
  }
  return engine_->manifold().dimensionNamed(name, engine_->store());
}

bool ZigzagVisualizer::isProtected(const CellRef id) const {
  if (!engine_) {
    return true;
  }
  return engine_->isProtected(id);
}

ZigzagVisualizer::CellInfo
ZigzagVisualizer::inspectCell(const CellRef id) const {
  if (!engine_ || zigzag::noCell == id) {
    return {};
  }
  const auto &manifold = engine_->manifold();
  if (!manifold.contains(id) && !isEphemeral(id)) {
    return {};
  }
  const auto &store = engine_->store();

  CellInfo info;
  info.id   = id;
  info.text = engine_->resolveCellText(id);

  if (id == manifold.home()) {
    info.role = "home";
  } else if (id == manifold.dimsDimension()) {
    info.role = "dimension";
  } else {
    for (const auto dim : manifold.dimensions()) {
      if (id == dim) {
        info.role = "dimension";
        break;
      }
    }
  }

  if (isEphemeral(id)) {
    info.role            = "dimension";
    info.is_clone        = true;
    const auto cloneDim  = manifold.dimensionNamed("d.clone", store);
    info.clone_master_id = engine_->cloneMaster(id, cloneDim);
  }

  const auto roleDim  = manifold.dimensionNamed("d.role", store);
  const auto mimeDim  = manifold.dimensionNamed("d.mime", store);
  const auto mediaDim = manifold.dimensionNamed("d.media", store);

  if (info.role.empty() && roleDim != zigzag::noCell && !isEphemeral(id)) {
    const auto held = manifold.linked(id, roleDim, DimVector::POS);
    if (held != zigzag::noCell) {
      info.role = manifold.textOf(held, store);
    }
  }
  if (info.role.empty()) {
    if (const auto *cold = engine_->coldOf(id)) {
      info.role = cold->type;
    }
  }

  if (!isEphemeral(id)) {
    if (mimeDim != zigzag::noCell) {
      const auto held = manifold.linked(id, mimeDim, DimVector::POS);
      if (held != zigzag::noCell) {
        info.mime_type = manifold.textOf(held, store);
      }
    }

    if (mediaDim != zigzag::noCell) {
      const auto held = manifold.linked(id, mediaDim, DimVector::POS);
      if (held != zigzag::noCell) {
        info.media_path = manifold.textOf(held, store);
      }
    }
  }

  info.is_image =
      info.mime_type.starts_with("image/") ||
      (!info.media_path.empty() && (info.media_path.ends_with(".png") ||
                                    info.media_path.ends_with(".jpg") ||
                                    info.media_path.ends_with(".jpeg") ||
                                    info.media_path.ends_with(".webp") ||
                                    info.media_path.ends_with(".gif") ||
                                    info.media_path.ends_with(".svg") ||
                                    info.media_path.ends_with(".bmp")));

  if (!isEphemeral(id)) {
    info.is_clone        = false;
    info.clone_master_id = id;
    const auto cloneDim  = manifold.dimensionNamed("d.clone", store);
    if (cloneDim != zigzag::noCell) {
      if (manifold.linked(id, cloneDim, DimVector::NEG) != zigzag::noCell) {
        info.is_clone        = true;
        info.clone_master_id = manifold.cloneMaster(id, cloneDim);
      }
    }
  }

  return info;
}

DimensionVisual
ZigzagVisualizer::dimensionVisual(const DimID &dimension) const {
  const auto it = dimension_visuals_.find(dimension);
  if (it != dimension_visuals_.end()) {
    return it->second;
  }
  if (dimension == "d.meta-dims") {
    return DimensionVisual{
        .color   = glm::vec3{0.2F, 0.8F, 0.8F},
        .spacing = 220.0F,
        .label   = "Meta-Dimensions",
    };
  }
  if (dimension == "d.dims") {
    return DimensionVisual{
        .color   = glm::vec3{0.6F, 0.4F, 0.9F},
        .spacing = 200.0F,
        .label   = "Dimensions",
    };
  }
  if (dimension == "d.clone") {
    return DimensionVisual{
        .color   = glm::vec3{0.95F, 0.75F, 0.2F},
        .spacing = 180.0F,
        .label   = "Clones",
    };
  }
  if (dimension == "d.role") {
    return DimensionVisual{
        .color   = glm::vec3{0.5F, 0.5F, 0.6F},
        .spacing = 150.0F,
        .label   = "Role",
    };
  }
  if (dimension == "d.mime") {
    return DimensionVisual{
        .color   = glm::vec3{0.4F, 0.5F, 0.7F},
        .spacing = 150.0F,
        .label   = "MIME",
    };
  }
  if (dimension == "d.media") {
    return DimensionVisual{
        .color   = glm::vec3{0.3F, 0.7F, 0.5F},
        .spacing = 150.0F,
        .label   = "Media",
    };
  }
  return DimensionVisual{
      .color   = glm::vec3{0.7F, 0.7F, 0.75F},
      .spacing = 200.0F,
      .label   = dimension,
  };
}

void ZigzagVisualizer::rebuildActiveViewTopology() {
  for (auto &[id, render_cell] : visible_cells_) {
    render_cell.target_alpha = 0.0F;
  }

  if (!engine_ || accursed_cell_focus_ == 0) {
    return;
  }

  const auto focusRef  = static_cast<CellRef>(accursed_cell_focus_);
  const auto focusInfo = inspectCell(focusRef);

  if (!visible_cells_.contains(accursed_cell_focus_)) {
    visible_cells_[accursed_cell_focus_] = RenderStateCell{
        .id              = accursed_cell_focus_,
        .text            = focusInfo.text,
        .type            = focusInfo.role,
        .mime_type       = focusInfo.mime_type,
        .media_path      = focusInfo.media_path,
        .is_image        = focusInfo.is_image,
        .is_clone        = focusInfo.is_clone,
        .clone_master_id = focusInfo.clone_master_id,
    };
  } else {
    visible_cells_[accursed_cell_focus_].text       = focusInfo.text;
    visible_cells_[accursed_cell_focus_].type       = focusInfo.role;
    visible_cells_[accursed_cell_focus_].mime_type  = focusInfo.mime_type;
    visible_cells_[accursed_cell_focus_].media_path = focusInfo.media_path;
    visible_cells_[accursed_cell_focus_].is_image   = focusInfo.is_image;
    visible_cells_[accursed_cell_focus_].is_clone   = focusInfo.is_clone;
    visible_cells_[accursed_cell_focus_].clone_master_id =
        focusInfo.clone_master_id;
  }

  auto &focusRenderState        = visible_cells_[accursed_cell_focus_];
  focusRenderState.target_pos   = glm::vec3{0.0F, 0.0F, 0.0F};
  focusRenderState.target_alpha = 1.0F;
  focusRenderState.base_color   = scene_.focus_color;

  auto mapNeighbor = [&](const CellRef parentId, const CellRef childId,
                         const glm::vec3 &offset, const glm::vec3 &axisColor) {
    if (childId == 0) {
      return;
    }
    const auto childInfo = inspectCell(childId);

    if (!visible_cells_.contains(childId)) {
      RenderStateCell newCell{
          .id              = childId,
          .text            = childInfo.text,
          .type            = childInfo.role,
          .mime_type       = childInfo.mime_type,
          .media_path      = childInfo.media_path,
          .is_image        = childInfo.is_image,
          .is_clone        = childInfo.is_clone,
          .clone_master_id = childInfo.clone_master_id,
          .current_pos     = visible_cells_[parentId].current_pos,
      };
      visible_cells_[childId] = newCell;
    } else {
      visible_cells_[childId].text            = childInfo.text;
      visible_cells_[childId].type            = childInfo.role;
      visible_cells_[childId].mime_type       = childInfo.mime_type;
      visible_cells_[childId].media_path      = childInfo.media_path;
      visible_cells_[childId].is_image        = childInfo.is_image;
      visible_cells_[childId].is_clone        = childInfo.is_clone;
      visible_cells_[childId].clone_master_id = childInfo.clone_master_id;
    }

    auto &childCell        = visible_cells_[childId];
    childCell.target_pos   = visible_cells_[parentId].target_pos + offset;
    childCell.target_alpha = 1.0F;
    childCell.base_color   = axisColor;
  };

  const DimensionVisual xVisual = dimensionVisual(current_view_.x_dimension);
  const DimensionVisual yVisual = dimensionVisual(current_view_.y_dimension);
  const DimensionVisual zVisual = dimensionVisual(current_view_.z_dimension);

  const float xSpace =
      (view_mode_ == ViewMode::CellContent) ? 260.0F : xVisual.spacing;
  const float ySpace =
      (view_mode_ == ViewMode::CellContent) ? 140.0F : yVisual.spacing;
  const float zSpace =
      (view_mode_ == ViewMode::CellContent) ? 180.0F : zVisual.spacing;

  const int radius = std::max(1, scene_.neighborhood_radius);

  auto mapAxis = [&](const DimID &dim, const glm::vec3 &unitDir,
                     const DimensionVisual &visual, const float spacing) {
    const auto dimRef = dimensionRef(dim);
    if (dimRef == zigzag::noCell) {
      return;
    }

    std::unordered_set<CellRef> visitedPos;
    visitedPos.insert(focusRef);

    // Positive walk
    CellRef parent = focusRef;
    for (int r = 1; r <= radius; ++r) {
      const CellRef nextId = engine_->linked(parent, dimRef, DimVector::POS);
      if (nextId == zigzag::noCell || visitedPos.contains(nextId)) {
        break;
      }
      visitedPos.insert(nextId);
      mapNeighbor(parent, nextId, unitDir * spacing, visual.color);
      parent = nextId;
    }

    std::unordered_set<CellRef> visitedNeg;
    visitedNeg.insert(focusRef);

    // Negative walk
    parent = focusRef;
    for (int r = 1; r <= radius; ++r) {
      const CellRef nextId = engine_->linked(parent, dimRef, DimVector::NEG);
      if (nextId == zigzag::noCell || visitedNeg.contains(nextId)) {
        break;
      }
      visitedNeg.insert(nextId);
      mapNeighbor(parent, nextId, -unitDir * spacing, visual.color);
      parent = nextId;
    }
  };

  mapAxis(current_view_.x_dimension, glm::vec3{1.0F, 0.0F, 0.0F}, xVisual,
          xSpace);
  mapAxis(current_view_.y_dimension, glm::vec3{0.0F, 1.0F, 0.0F}, yVisual,
          ySpace);
  mapAxis(current_view_.z_dimension, glm::vec3{0.0F, 0.0F, 1.0F}, zVisual,
          zSpace);
}

void ZigzagVisualizer::updateCellPositions(const float rawDeltaTime) {
  const float deltaTime   = std::clamp(rawDeltaTime, 1.0F / 120.0F, 0.1F);
  const float layoutSpeed = scene_.layout_speed;
  const float alphaSpeed  = scene_.alpha_speed;

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

void ZigzagVisualizer::navigateFocus(const DimID &dimension,
                                     const DimVector dir) {
  if (!engine_ || accursed_cell_focus_ == 0) {
    return;
  }
  const auto focus  = static_cast<CellRef>(accursed_cell_focus_);
  const auto dimRef = dimensionRef(dimension);
  if (dimRef == zigzag::noCell) {
    return;
  }
  const CellRef next = engine_->linked(focus, dimRef, dir);
  if (next == zigzag::noCell) {
    return;
  }

  accursed_cell_focus_ = next;
  rebuildActiveViewTopology();
  invalidateAccessibility();
}

void ZigzagVisualizer::navigateFocusTo(const CellID id) {
  const auto ref = static_cast<CellRef>(id);
  if (!engine_ || id == 0 || !engine_->findCell(ref)) {
    return;
  }
  accursed_cell_focus_ = id;
  rebuildActiveViewTopology();
  invalidateAccessibility();
}

void ZigzagVisualizer::setViewMode(const ViewMode mode) {
  view_mode_ = mode;
  rebuildActiveViewTopology();
  invalidateAccessibility();
}

void ZigzagVisualizer::toggleViewMode() {
  setViewMode(view_mode_ == ViewMode::CellContent ? ViewMode::Topology
                                                  : ViewMode::CellContent);
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

bool ZigzagVisualizer::picked(const render::PickingResult &pick,
                              RenderState &) {
  if (pick.tag.kind == render::tagKindOverlay && pick.tag.clusterIndex != 0) {
    const auto targetId = static_cast<CellRef>(pick.tag.clusterIndex);
    if (engine_ && engine_->findCell(targetId)) {
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

  updateCellPositions(deltaTime);

  if (!beams_ || !worldCanvas_ || !hudCanvas_) {
    return;
  }

  // --- 1. Draw 3D Connection Beams ---
  beams_->clear();
  std::vector<std::pair<CellID, CellID>> drawnEdges;

  if (engine_) {
    const auto &manifold = engine_->manifold();
    const auto &store    = engine_->store();

    // Resolved once per frame, not once per cell. Which cell an axis is
    // asked about does not change which dimension the axis names, and the
    // lookup is a rank walk that reads each dimension cell's content through
    // the SpanReader -- a std::string per dimension per call. Sixty visible
    // cells times three axes times a dozen dimensions was a couple of thousand
    // allocations a frame, in the one path that exists to stage without them.
    const std::array<std::pair<DimID, DimRef>, 3> viewAxes{
        std::pair{current_view_.x_dimension,
                  dimensionRef(current_view_.x_dimension)},
        std::pair{current_view_.y_dimension,
                  dimensionRef(current_view_.y_dimension)},
        std::pair{current_view_.z_dimension,
                  dimensionRef(current_view_.z_dimension)},
    };

    for (const auto &[id, cell] : visible_cells_) {
      const auto cellRef = static_cast<CellRef>(id);

      // 1) View dimensions (covers ephemeral meta-dims and clones as well)
      for (const auto &[dimName, dimRef] : viewAxes) {
        if (dimRef == zigzag::noCell) {
          continue;
        }
        for (const auto dir : {DimVector::POS, DimVector::NEG}) {
          const auto neighborId = engine_->linked(cellRef, dimRef, dir);
          if (neighborId == 0 || neighborId == cellRef ||
              !visible_cells_.contains(neighborId)) {
            continue;
          }
          const auto edge =
              std::pair{std::min(id, static_cast<CellID>(neighborId)),
                        std::max(id, static_cast<CellID>(neighborId))};
          if (std::ranges::find(drawnEdges, edge) != drawnEdges.end()) {
            continue;
          }
          drawnEdges.push_back(edge);

          const auto &neighborCell = visible_cells_.at(neighborId);
          const auto visual        = dimensionVisual(dimName);
          const float edgeAlpha =
              std::min(cell.current_alpha, neighborCell.current_alpha);
          const std::uint32_t col = packRgba(visual.color.r, visual.color.g,
                                             visual.color.b, edgeAlpha);

          beams_->add(cell.current_pos, neighborCell.current_pos, 4.0F, col,
                      static_cast<std::uint32_t>(id));
        }
      }

      // 2) Stored dimensions of normal cells
      if (!isEphemeral(cellRef)) {
        for (const auto &link : manifold.dimensionsOf(cellRef)) {
          const auto dimName = manifold.textOf(link.dim, store);
          if (dimName == "d.role" || dimName == "d.mime" ||
              dimName == "d.media") {
            continue;
          }
          const DimensionVisual visual = dimensionVisual(dimName);
          for (const CellRef neighborId : {link.pos, link.neg}) {
            if (neighborId == 0 || neighborId == cellRef ||
                !visible_cells_.contains(neighborId)) {
              continue;
            }

            const auto edge =
                std::pair{std::min(id, static_cast<CellID>(neighborId)),
                          std::max(id, static_cast<CellID>(neighborId))};
            if (std::ranges::find(drawnEdges, edge) != drawnEdges.end()) {
              continue;
            }
            drawnEdges.push_back(edge);

            const auto &neighborCell = visible_cells_.at(neighborId);
            const float edgeAlpha =
                std::min(cell.current_alpha, neighborCell.current_alpha);
            const std::uint32_t col = packRgba(visual.color.r, visual.color.g,
                                               visual.color.b, edgeAlpha);

            beams_->add(cell.current_pos, neighborCell.current_pos, 4.0F, col,
                        static_cast<std::uint32_t>(id));
          }
        }
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

    const bool isFocus    = (id == accursed_cell_focus_);
    const float nodeWidth = (view_mode_ == ViewMode::CellContent)
                                ? (isFocus ? 260.0F : 200.0F)
                                : (isFocus ? 140.0F : 110.0F);
    // Extra vertical room for the inline image preview below, on top of
    // whichever view mode's height already applies -- the same +50/+40px an
    // image cell got before CellContent mode existed, carried forward rather
    // than only honoured in Topology mode.
    const float imageBonus = cell.is_image ? (isFocus ? 50.0F : 40.0F) : 0.0F;
    const float nodeHeight =
        ((view_mode_ == ViewMode::CellContent) ? (isFocus ? 95.0F : 70.0F)
                                               : (isFocus ? 54.0F : 45.0F)) +
        imageBonus;

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

    // Image Preview (if image cell with media path)
    if (cell.is_image && !cell.media_path.empty() && imageCache_) {
      std::string resolvedPath = cell.media_path;
      if (!current_slice_path_.empty() && !resolvedPath.starts_with("/")) {
        const auto parent =
            std::filesystem::path(current_slice_path_).parent_path();
        if (!parent.empty()) {
          resolvedPath = (parent / resolvedPath).string();
        }
      }
      auto imgRes = imageCache_->find(resolvedPath);
      if (!imgRes) {
        imgRes = imageCache_->loadFile(resolvedPath);
      }
      if (imgRes && imgRes->valid()) {
        const float imgMargin = 6.0F;
        const float imgW      = nodeWidth - (imgMargin * 2.0F);
        const float imgH      = isFocus ? 60.0F : 40.0F;
        const float imgLeft   = left + imgMargin;
        const float imgBottom = bottom + 24.0F;
        worldCanvas_->addImage(imgLeft, imgBottom, imgW, imgH, *imgRes,
                               packRgba(1.0F, 1.0F, 1.0F, cell.current_alpha));
      }
    }

    // Node Border
    const float borderThick = scene_.border_thickness;
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
    const std::size_t maxTextLen  = (view_mode_ == ViewMode::CellContent)
                                        ? (isFocus ? 48 : 32)
                                        : (isFocus ? 16 : 10);
    const std::string textPreview = shortenText(cell.text, maxTextLen);
    worldCanvas_->addText(ctx.state, left + 6.0F, bottom + nodeHeight - 24.0F,
                          textPreview, textCol, bgCol);

    // Badges: type, mime, clone
    if (!cell.type.empty() || !cell.mime_type.empty() || cell.is_clone) {
      std::string badge;
      if (!cell.type.empty()) {
        badge += "[" + cell.type + "] ";
      } else if (!cell.mime_type.empty()) {
        badge += "<" + cell.mime_type + "> ";
      }
      if (cell.is_clone) {
        badge += std::format("[clone #{}] ", cell.clone_master_id);
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
  if (engine_ && accursed_cell_focus_ != 0) {
    const auto cur = inspectCell(static_cast<CellRef>(accursed_cell_focus_));
    std::string mediaTag;
    if (!cur.mime_type.empty()) {
      mediaTag = std::format(" <{}>", cur.mime_type);
    }
    focusLabel =
        std::format("Focus: #{}{} \"{}\" {}", cur.id, mediaTag, cur.text,
                    cur.role.empty() ? "" : "[" + cur.role + "]");
    if (cur.is_clone) {
      focusLabel += std::format(" [clone of #{}]", cur.clone_master_id);
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

  // View Mode Status Indicator
  const std::string modeLabel = (view_mode_ == ViewMode::CellContent)
                                    ? "[ View: 📄 Content (1/V) ]"
                                    : "[ View: 🌐 Topology (2/T) ]";
  const auto modeMetrics      = hudCanvas_->measureText(modeLabel);
  hudCanvas_->addText(ctx.state,
                      width - dimsMetrics.width - modeMetrics.width - 32.0F,
                      height - 12.0F, modeLabel, 0xF59E0BFFU, 0x0D0D12DDU);

  // Bottom Command Key Hints
  hudCanvas_->addRect(0.0F, 0.0F, width, 28.0F, 0x0D0D12DDU);
  hudCanvas_->addLine(0.0F, 28.0F, width, 28.0F, 1.0F, 0x222233FFU);
  const std::string hints =
      "Arrows: Step X/Y | PgUp/PgDn: Step Z | Space: Swap X/Y | Tab: Cycle | "
      "N/D: Insert | U: Unlink | Del: Delete | R: Reset View";
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
  if (engine_) {
    for (const auto &slot : engine_->manifold().cells()) {
      // An unlinked cell is not announced. Deleting a cell unlinks it and
      // cannot remove it -- DELETE is REARRANGE TO LIMBO, so its MakeCell stays
      // on the ancestral path and the manifold still holds it. The visual path
      // is a walk outward from the focus, so it stops drawing the cell; this
      // tree enumerates every cell, so without this it went on announcing one
      // the sighted user had just watched disappear. Reachability is the honest
      // shared test, and it keeps every cell a person can actually get to.
      //
      // Reachability is asked as "does any link name a cell", not "is the run
      // empty": clearing a link leaves the run entry behind holding noCell on
      // both sides, so an unlinked cell still has as many entries as it ever
      // had. Having a slot for a dimension is not being linked along it.
      const auto runOf = engine_->manifold().dimensionsOf(slot.birthOp);
      const bool reachable =
          std::ranges::any_of(runOf, [](const DimLink &link) {
            return link.pos != zigzag::noCell || link.neg != zigzag::noCell;
          });
      if (!reachable && slot.birthOp != engine_->manifold().home() &&
          slot.birthOp != engine_->manifold().dimsDimension()) {
        continue;
      }
      const auto cellInfo = inspectCell(slot.birthOp);
      const bool isFocus  = (slot.birthOp == accursed_cell_focus_);
      std::string desc =
          std::format("Cell #{}: {}", slot.birthOp, cellInfo.text);
      if (!cellInfo.role.empty()) {
        desc += " [" + cellInfo.role + "]";
      }
      if (cellInfo.is_clone) {
        desc += std::format(" [Clone of #{}]", cellInfo.clone_master_id);
      }
      if (isFocus) {
        desc += " (Focused)";
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

    if (isEphemeral(static_cast<CellRef>(accursed_cell_focus_))) {
      const auto focusRef = static_cast<CellRef>(accursed_cell_focus_);
      const auto cellInfo = inspectCell(focusRef);
      std::string desc =
          std::format("Cell #{}: {} [Clone of #{}] (Focused)", focusRef,
                      cellInfo.text, cellInfo.clone_master_id);
      if (!cellInfo.role.empty()) {
        desc += " [" + cellInfo.role + "]";
      }
      auto &cellNode   = into.add(nextNodeId, gleditor::a11y::Role::ListItem);
      cellNode.label   = std::move(desc);
      cellNode.actions = gleditor::a11y::bit(gleditor::a11y::Action::Click) |
                         gleditor::a11y::bit(gleditor::a11y::Action::Focus);
      rootChildren.push_back(into.id(nextNodeId));
      into.takeFocus(into.id(nextNodeId));
      nextNodeId++;
    }
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
    if (localId >= 10 && engine_) {
      const auto cellIndex = localId - 10;
      if (cellIndex < engine_->manifold().cells().size()) {
        navigateFocusTo(engine_->manifold().cells()[cellIndex].birthOp);
        return true;
      }
      if (cellIndex == engine_->manifold().cells().size() &&
          isEphemeral(static_cast<CellRef>(accursed_cell_focus_))) {
        return true;
      }
    }
  }
  return false;
}

} // namespace zigzag
