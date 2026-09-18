/**
 * @file zigzag_visualizer.cpp
 * @brief Implementation of the Xanadu ZigZag visualizer on gleditor.
 */
#include "zigzag_visualizer.hpp"
#include "core/format_resolver.hpp"
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

} // namespace

ZigzagVisualizer::ZigzagVisualizer(std::string aFontName)
    : fontName_(std::move(aFontName)),
      last_frame_time_(std::chrono::steady_clock::now()) {
  presentation_transform_ =
      glm::scale(glm::mat4{1.0F}, glm::vec3{Doc::pixelsToWorld});
  populateFallbackStructure();
}

ZigzagVisualizer::~ZigzagVisualizer() = default;

void ZigzagVisualizer::setCellRadius(const int radius) noexcept {
  const int clamped = std::max(1, radius);
  if (scene_.neighborhood_radius == clamped) {
    return;
  }
  scene_.neighborhood_radius = clamped;
  rebuildActiveViewTopology();
  invalidateAccessibility();
}

void ZigzagVisualizer::setPresentationConfig(
    xanadu::ZigzagPresentationConfig config) {
  if (presentation_config_ == config) {
    return;
  }
  presentation_config_ = config;
  cell_layouts_dirty_  = true;
  rebuildActiveViewTopology();
  invalidateAccessibility();
}

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

std::string ZigzagVisualizer::documentId() const {
  return engine_ ? engine_->store().documentId().str() : std::string{};
}

std::string ZigzagVisualizer::documentVersion() const {
  return engine_ ? engine_->head().str() : std::string{};
}

void ZigzagVisualizer::populateFallbackStructure() {
  ZzStructureDocument doc;
  doc.meta.name = "Xanadu ZigZag Sample Structure";
  doc.focus     = 1;
  doc.view      = ViewAxisBinding{
      .x_dimension = "d.1", .y_dimension = "d.2", .z_dimension = "d.3"};

  Cell c1;
  c1.id         = 1;
  c1.data       = std::string{"Root Focus Node"};
  c1.role       = "root";
  c1.dimensions = {{"d.1", {.pos = 2, .neg = 0}},
                   {"d.2", {.pos = 3, .neg = 0}}};
  doc.cells[1]  = std::move(c1);

  Cell c2;
  c2.id         = 2;
  c2.data       = std::string{"Horizontal Cell"};
  c2.role       = "item";
  c2.dimensions = {{"d.1", {.pos = 0, .neg = 1}}};
  doc.cells[2]  = std::move(c2);

  Cell c3;
  c3.id         = 3;
  c3.data       = std::string{"Vertical Cell"};
  c3.role       = "item";
  c3.dimensions = {{"d.2", {.pos = 0, .neg = 1}},
                   {"d.3", {.pos = 4, .neg = 0}}};
  doc.cells[3]  = std::move(c3);

  Cell c4;
  c4.id         = 4;
  c4.data       = std::string{"Depth Layer Cell"};
  c4.role       = "detail";
  c4.dimensions = {{"d.3", {.pos = 0, .neg = 3}}};
  doc.cells[4]  = std::move(c4);

  DimensionMeta dm1;
  dm1.label                 = "Sequence";
  dm1.color                 = RgbColor{.r = 0.89F, .g = 0.36F, .b = 0.36F};
  dm1.spacing               = 2.4F;
  doc.dimension_meta["d.1"] = std::move(dm1);

  DimensionMeta dm2;
  dm2.label                 = "Detail";
  dm2.color                 = RgbColor{.r = 0.35F, .g = 0.76F, .b = 0.48F};
  dm2.spacing               = 1.8F;
  doc.dimension_meta["d.2"] = std::move(dm2);

  DimensionMeta dm3;
  dm3.label                 = "Reference";
  dm3.color                 = RgbColor{.r = 0.31F, .g = 0.62F, .b = 0.88F};
  dm3.spacing               = 2.0F;
  doc.dimension_meta["d.3"] = std::move(dm3);

  adoptDocument(std::move(doc), "fallback");
}

void ZigzagVisualizer::adoptDocument(
    ZzStructureDocument &&doc, std::string sourcePath,
    const std::unordered_map<CellID, XuduProjectionProvenance> *const origins) {
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

  ownedStore_       = std::make_unique<xanadu::Store>();
  store_            = ownedStore_.get();
  const auto sliced = sliceToStore(doc, *store_);
  sourceOrigins_.clear();
  pickTargets_.clear();
  pickTargetVersion_.clear();
  if (nullptr != origins) {
    sourceOrigins_.reserve(origins->size());
    for (const auto &[projectedCell, source] : *origins) {
      if (const auto minted = sliced.cells.find(projectedCell);
          minted != sliced.cells.end()) {
        sourceOrigins_.emplace(minted->second, source);
      }
    }
  }
  engine_ = std::make_unique<UnifiedTransclusionEngine>(*store_);

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
  ensureVortexHost();
  invalidateAccessibility();
}

void ZigzagVisualizer::adoptXuduStore(
    const xanadu::Store &store,
    const std::vector<xanadu::MicroversionId> &versions) {
  auto projected = projectStoreWithProvenance(store, versions);
  adoptDocument(std::move(projected.document), "xudu_store",
                &projected.sourceByProjectedCell);
}

void ZigzagVisualizer::bindXuduStore(xanadu::Store &store,
                                     const xanadu::MicroversionId &version) {
  engine_.reset();
  ownedStore_.reset();
  store_          = &store;
  engine_         = std::make_unique<UnifiedTransclusionEngine>(store, version);
  structure_name_ = "Xudu/Zigzag unified store";
  current_slice_path_.clear();

  accursed_cell_focus_ = engine_->manifold().home();
  if (accursed_cell_focus_ == 0 && engine_->manifold().cellCount() > 0) {
    accursed_cell_focus_ = engine_->manifold().cells().front().birthOp;
  }
  visible_cells_.clear();
  rebuildActiveViewTopology();
  for (auto &[id, cell] : visible_cells_) {
    cell.current_pos   = cell.target_pos;
    cell.current_alpha = cell.target_alpha;
  }
  ensureVortexHost();
  invalidateAccessibility();
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
        .color = RgbColor{.r = vis.color.r, .g = vis.color.g, .b = vis.color.b},
        .spacing = vis.spacing / 100.0F,
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

bool ZigzagVisualizer::saveStore(const std::string &filePath) const {
  const auto savePath = filePath.empty() ? current_slice_path_ : filePath;
  if (savePath.empty() || !engine_) {
    return false;
  }
  try {
    engine_->store().save(savePath);
    return true;
  } catch (...) {
    return false;
  }
}

bool ZigzagVisualizer::saveStructureYaml(const std::string &filePath) const {
  return saveStore(filePath);
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
  // Standard Vortex Dimensions
  if (dimension == "d.spin") {
    return DimensionVisual{
        .color   = glm::vec3{0.2F, 0.85F, 0.85F},
        .spacing = 180.0F,
        .label   = "Spin",
    };
  }
  if (dimension == "d.step") {
    return DimensionVisual{
        .color   = glm::vec3{0.3F, 0.6F, 0.95F},
        .spacing = 180.0F,
        .label   = "Step",
    };
  }
  if (dimension == "d.branch") {
    return DimensionVisual{
        .color   = glm::vec3{0.7F, 0.4F, 0.9F},
        .spacing = 180.0F,
        .label   = "Branch",
    };
  }
  if (dimension == "d.lexical") {
    return DimensionVisual{
        .color   = glm::vec3{0.95F, 0.55F, 0.2F},
        .spacing = 180.0F,
        .label   = "Lexical",
    };
  }
  if (dimension == "d.dynamic") {
    return DimensionVisual{
        .color   = glm::vec3{0.9F, 0.7F, 0.2F},
        .spacing = 180.0F,
        .label   = "Dynamic",
    };
  }
  if (dimension == "d.env") {
    return DimensionVisual{
        .color   = glm::vec3{0.85F, 0.85F, 0.2F},
        .spacing = 180.0F,
        .label   = "Env",
    };
  }
  if (dimension == "d.require") {
    return DimensionVisual{
        .color   = glm::vec3{0.2F, 0.85F, 0.4F},
        .spacing = 180.0F,
        .label   = "Require",
    };
  }
  if (dimension == "d.ensure") {
    return DimensionVisual{
        .color   = glm::vec3{0.1F, 0.75F, 0.5F},
        .spacing = 180.0F,
        .label   = "Ensure",
    };
  }
  if (dimension == "d.invariant") {
    return DimensionVisual{
        .color   = glm::vec3{0.3F, 0.9F, 0.6F},
        .spacing = 180.0F,
        .label   = "Invariant",
    };
  }
  if (dimension == "d.clause") {
    return DimensionVisual{
        .color   = glm::vec3{0.9F, 0.3F, 0.6F},
        .spacing = 180.0F,
        .label   = "Clause",
    };
  }
  if (dimension == "d.predicate") {
    return DimensionVisual{
        .color   = glm::vec3{0.95F, 0.4F, 0.5F},
        .spacing = 180.0F,
        .label   = "Predicate",
    };
  }
  if (dimension == "d.var") {
    return DimensionVisual{
        .color   = glm::vec3{0.75F, 0.4F, 0.85F},
        .spacing = 180.0F,
        .label   = "Var",
    };
  }
  if (dimension == "d.stdlib") {
    return DimensionVisual{
        .color   = glm::vec3{0.4F, 0.5F, 0.95F},
        .spacing = 180.0F,
        .label   = "Stdlib",
    };
  }
  if (dimension == "d.symbol") {
    return DimensionVisual{
        .color   = glm::vec3{0.3F, 0.75F, 0.95F},
        .spacing = 180.0F,
        .label   = "Symbol",
    };
  }
  if (dimension == "d.version") {
    return DimensionVisual{
        .color   = glm::vec3{0.6F, 0.6F, 0.7F},
        .spacing = 180.0F,
        .label   = "Version",
    };
  }
  if (dimension == "d.grab") {
    return DimensionVisual{
        .color   = glm::vec3{0.92F, 0.40F, 0.70F},
        .spacing = 180.0F,
        .label   = "Grab (Wings)",
    };
  }
  if (dimension == "d.vars") {
    return DimensionVisual{
        .color   = glm::vec3{0.25F, 0.75F, 0.85F},
        .spacing = 180.0F,
        .label   = "Vars (Scope)",
    };
  }
  if (dimension == "d.values") {
    return DimensionVisual{
        .color   = glm::vec3{0.95F, 0.78F, 0.25F},
        .spacing = 180.0F,
        .label   = "Values (Payload)",
    };
  }
  if (dimension == "d.cache") {
    return DimensionVisual{
        .color   = glm::vec3{0.70F, 0.80F, 0.88F},
        .spacing = 180.0F,
        .label   = "Cache (Memo)",
    };
  }
  return DimensionVisual{
      .color   = glm::vec3{0.7F, 0.7F, 0.75F},
      .spacing = 200.0F,
      .label   = dimension,
  };
}

CellLayoutMetrics
ZigzagVisualizer::measureCellLayout(const RenderStateCell &cell,
                                    const bool isFocus) const {
  static_cast<void>(isFocus);
  const float widthLimit = view_mode_ == ViewMode::CellContent
                               ? presentation_config_.contentMaxWidthPx
                               : presentation_config_.topologyMaxWidthPx;

  CellLayoutMetrics metrics;
  metrics.idText = std::format("#{}", cell.id);
  if (!cell.type.empty()) {
    metrics.badgeText = "[" + cell.type + "]";
  } else if (!cell.mime_type.empty()) {
    metrics.badgeText = "<" + cell.mime_type + ">";
  }
  if (cell.is_clone) {
    if (!metrics.badgeText.empty()) {
      metrics.badgeText += " ";
    }
    metrics.badgeText += std::format("[clone #{}]", cell.clone_master_id);
  }

  if (!worldCanvas_) {
    // Before deviceReady() no font has been selected, so only the explicitly
    // configured breathing room is knowable. The real measurement replaces
    // this before the first rendered frame.
    metrics.width  = 2.0F * presentation_config_.cellHorizontalPaddingPx;
    metrics.height = 2.0F * presentation_config_.cellVerticalPaddingPx;
    metrics.labelWidthLimit = widthLimit;
    metrics.labelLineHeight = 2.0F * presentation_config_.cellVerticalPaddingPx;
    return metrics;
  }

  const auto titleMetrics = worldCanvas_->measureText(metrics.idText);
  worldCanvas_->setTextWidthLimit(static_cast<int>(widthLimit));
  const auto labelMetrics = worldCanvas_->measureText(cell.text);
  worldCanvas_->setTextWidthLimit(0);
  const auto badgeMetrics = metrics.badgeText.empty()
                                ? gleditor::TextMetrics{}
                                : worldCanvas_->measureText(metrics.badgeText);

  metrics.labelWidthLimit = widthLimit;
  metrics.labelLineHeight = labelMetrics.height;
  metrics.width =
      std::max({titleMetrics.width, labelMetrics.width, badgeMetrics.width}) +
      (2.0F * presentation_config_.cellHorizontalPaddingPx);

  const bool hasBadge = !metrics.badgeText.empty();
  const float gaps =
      presentation_config_.cellBandGapPx * (hasBadge ? 2.0F : 1.0F);
  metrics.height = (2.0F * presentation_config_.cellVerticalPaddingPx) +
                   titleMetrics.height + labelMetrics.height +
                   badgeMetrics.height + gaps;

  metrics.titleTop =
      metrics.height - presentation_config_.cellVerticalPaddingPx;
  const float titleBottom = metrics.titleTop - titleMetrics.height;
  const float labelBottom =
      hasBadge ? presentation_config_.cellVerticalPaddingPx +
                     badgeMetrics.height + presentation_config_.cellBandGapPx
               : presentation_config_.cellVerticalPaddingPx;
  const float labelCeiling = titleBottom - presentation_config_.cellBandGapPx;
  metrics.labelTop =
      labelBottom + ((labelCeiling - labelBottom + labelMetrics.height) / 2.0F);
  metrics.badgeTop =
      presentation_config_.cellVerticalPaddingPx + badgeMetrics.height;
  return metrics;
}

const CellLayoutMetrics &
ZigzagVisualizer::cellLayout(const CellID id, const RenderStateCell &cell,
                             const bool isFocus) const {
  if (const auto found = cell_layouts_.find(id); found != cell_layouts_.end()) {
    return found->second;
  }
  return cell_layouts_.emplace(id, measureCellLayout(cell, isFocus))
      .first->second;
}

void ZigzagVisualizer::refreshCellLayouts() {
  cell_layouts_.clear();
  cell_layouts_.reserve(visible_cells_.size());
  for (const auto &[id, cell] : visible_cells_) {
    cell_layouts_.emplace(id,
                          measureCellLayout(cell, id == accursed_cell_focus_));
  }
}

void ZigzagVisualizer::rebuildActiveViewTopology() {
  cell_layouts_dirty_ = true;
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
        .id               = accursed_cell_focus_,
        .text             = focusInfo.text,
        .type             = focusInfo.role,
        .mime_type        = focusInfo.mime_type,
        .media_path       = focusInfo.media_path,
        .is_image         = focusInfo.is_image,
        .is_clone         = focusInfo.is_clone,
        .clone_master_id  = focusInfo.clone_master_id,
        .current_pos      = {},
        .target_pos       = {},
        .current_alpha    = 0.0F,
        .target_alpha     = 1.0F,
        .base_color       = {},
        .decorated_ranges = {},
        .block_styles     = {},
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

  const xanadu::FormatResolver formatResolver(engine_->store());
  auto updateCellFormatting = [&](RenderStateCell &rc, const CellRef cr) {
    const auto *slot = engine_->manifold().slot(cr);
    if (__builtin_expect(slot && slot->formatFlags != 0, 0)) {
      auto res            = formatResolver.resolveCell(engine_->manifold(), cr);
      rc.decorated_ranges = std::move(res.decoratedRanges);
      rc.block_styles     = std::move(res.blockStyles);
    } else {
      rc.decorated_ranges.clear();
      rc.block_styles.clear();
    }
  };

  auto &focusRenderState        = visible_cells_[accursed_cell_focus_];
  focusRenderState.target_pos   = glm::vec3{0.0F, 0.0F, depth_tier_};
  focusRenderState.target_alpha = depth_tier_opacity_;
  focusRenderState.base_color   = scene_.focus_color;
  updateCellFormatting(focusRenderState, focusRef);

  auto mapNeighbor = [&](const CellRef parentId, const CellRef childId,
                         const glm::vec3 &offset, const glm::vec3 &axisColor) {
    if (childId == 0) {
      return;
    }
    const auto childInfo = inspectCell(childId);

    if (!visible_cells_.contains(childId)) {
      RenderStateCell newCell{
          .id               = childId,
          .text             = childInfo.text,
          .type             = childInfo.role,
          .mime_type        = childInfo.mime_type,
          .media_path       = childInfo.media_path,
          .is_image         = childInfo.is_image,
          .is_clone         = childInfo.is_clone,
          .clone_master_id  = childInfo.clone_master_id,
          .current_pos      = visible_cells_[parentId].current_pos,
          .target_pos       = {},
          .current_alpha    = 0.0F,
          .target_alpha     = depth_tier_opacity_,
          .base_color       = {},
          .decorated_ranges = {},
          .block_styles     = {},
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
    childCell.target_alpha = depth_tier_opacity_;
    childCell.base_color   = axisColor;
    updateCellFormatting(childCell, childId);
  };

  const DimensionVisual xVisual = dimensionVisual(current_view_.x_dimension);
  const DimensionVisual yVisual = dimensionVisual(current_view_.y_dimension);
  const DimensionVisual zVisual = dimensionVisual(current_view_.z_dimension);

  float widestCell  = 0.0F;
  float tallestCell = 0.0F;
  for (const auto &[id, visible] : visible_cells_) {
    const auto &layout = cellLayout(id, visible, id == accursed_cell_focus_);
    widestCell         = std::max(widestCell, layout.width);
    tallestCell        = std::max(tallestCell, layout.height);
  }
  const auto &focusLayout =
      cellLayout(accursed_cell_focus_, focusRenderState, true);
  // A content-view rank is spaced from the measured extents of the cards it
  // contains. The clearance is a reading policy; the extents are not.
  const float xSpace = view_mode_ == ViewMode::CellContent
                           ? ((focusLayout.width + widestCell) / 2.0F) +
                                 presentation_config_.rankClearancePx
                           : xVisual.spacing;
  const float ySpace = view_mode_ == ViewMode::CellContent
                           ? ((focusLayout.height + tallestCell) / 2.0F) +
                                 presentation_config_.rankClearancePx
                           : yVisual.spacing;
  const float zSpace = view_mode_ == ViewMode::CellContent
                           ? (std::max(focusLayout.width, widestCell) / 2.0F) +
                                 presentation_config_.rankClearancePx
                           : zVisual.spacing;

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

void ZigzagVisualizer::setDepthTier(const float baseDepthZ,
                                    const float opacityMultiplier) {
  const float deltaZ  = baseDepthZ - depth_tier_;
  depth_tier_         = baseDepthZ;
  depth_tier_opacity_ = std::clamp(opacityMultiplier, 0.0F, 1.0F);
  for (auto &[id, cell] : visible_cells_) {
    cell.current_pos.z += deltaZ;
  }
  rebuildActiveViewTopology();
  invalidateAccessibility();
}

void ZigzagVisualizer::setPresentationOrigin(const glm::vec3 origin) {
  presentation_origin_ = origin;
  presentation_transform_ =
      glm::translate(glm::mat4{1.0F}, origin) *
      glm::scale(glm::mat4{1.0F}, glm::vec3{Doc::pixelsToWorld});
  invalidateAccessibility();
}

void ZigzagVisualizer::swapDimensions(const int axis1, const int axis2) {
  std::array<DimID *, 3> dims = {&current_view_.x_dimension,
                                 &current_view_.y_dimension,
                                 &current_view_.z_dimension};
  if (axis1 >= 0 && axis1 < 3 && axis2 >= 0 && axis2 < 3 && axis1 != axis2) {
    std::swap(*dims[axis1], *dims[axis2]);
    dimension_bundle_ = DimensionBundle::Custom;
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
  dimension_bundle_ = DimensionBundle::Custom;
  rebuildActiveViewTopology();
  invalidateAccessibility();
}

bool ZigzagVisualizer::picked(const render::PickingResult &pick,
                              RenderState & /*state*/) {
  if (pick.tag.kind != render::tagKindOverlay || !engine_ ||
      !pick.semanticTarget || !pick.semanticTarget->cellRef ||
      pick.semanticTarget->documentId != engine_->store().documentId().str() ||
      pick.semanticTarget->microversion != engine_->head().str()) {
    return false;
  }
  const auto targetId = static_cast<CellRef>(*pick.semanticTarget->cellRef);
  if (!isEphemeral(targetId) && engine_->findCell(targetId)) {
    navigateFocusTo(targetId);
    return true;
  }
  return false;
}

void ZigzagVisualizer::drawFrame(gleditor::FrameContext &ctx) {
  if (presentationTransformResolver_) {
    const auto transform = presentationTransformResolver_();
    if (!transform) {
      return;
    }
    presentation_transform_ = *transform;
    presentation_origin_    = glm::vec3(presentation_transform_[3]);
  }
  if (presentationOriginResolver_) {
    if (const auto origin = presentationOriginResolver_();
        origin.has_value() && *origin != presentation_origin_) {
      presentation_origin_ = *origin;
      invalidateAccessibility();
    }
  }

  if (vortex_host_) {
    vortex_host_->stepScheduler(100);
  }

  const auto now = std::chrono::steady_clock::now();
  const float deltaTime =
      std::chrono::duration<float>(now - last_frame_time_).count();
  last_frame_time_ = now;

  updateCellPositions(deltaTime);

  if (!beams_ || !worldCanvas_ || !hudCanvas_) {
    return;
  }

  if (cell_layouts_dirty_) {
    // The first pass measures the existing neighborhood; rebuilding can add a
    // newly discovered neighbour, so measure once more before publishing this
    // stable geometry to drawing, anchors, and the next rank walk.
    refreshCellLayouts();
    rebuildActiveViewTopology();
    refreshCellLayouts();
    cell_layouts_dirty_ = false;
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

          beams_->add(cell.current_pos, neighborCell.current_pos,
                      presentation_config_.connectionBeamWidthPx, col,
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

            beams_->add(cell.current_pos, neighborCell.current_pos,
                        presentation_config_.connectionBeamWidthPx, col,
                        static_cast<std::uint32_t>(id));
          }
        }
      }
    }
  }

  if (beams_->pending() > 0) {
    beams_->commit();
    beams_->draw(ctx.state, ctx.viewProjection * presentation_transform_, 1.0F,
                 0);
  }

  // --- 2. Draw 3D Cell Nodes & Text ---
  worldCanvas_->clear();
  const auto pickScope = ctx.state.allocateOverlayPickScope();
  worldCanvas_->setIdentity(pickScope, 0);
  const auto &pickStore  = engine_->store();
  const auto pickVersion = engine_->head().str();
  if (pickTargetVersion_ != pickVersion) {
    pickTargets_.clear();
    pickTargetVersion_ = pickVersion;
  }

  for (const auto &[id, cell] : visible_cells_) {
    if (cell.current_alpha < 0.02F) {
      continue;
    }

    const auto cellRef = static_cast<CellRef>(id);
    auto &target       = pickTargets_[cellRef];
    if (!target) {
      render::PickSemanticTarget targetValue{
          .documentId   = pickStore.documentId().str(),
          .microversion = pickVersion,
          .cellRef =
              isEphemeral(cellRef) ? std::nullopt : std::optional{cellRef}};
      if (const auto source = sourceOrigins_.find(cellRef);
          source != sourceOrigins_.end()) {
        const auto &origin = source->second;
        targetValue.source = render::PickSemanticTarget::SourceProvenance{
            .documentId   = origin.documentId,
            .microversion = origin.version.str(),
            .cellRef      = origin.sourceCell,
            .scroll       = origin.span.scroll,
            .start        = origin.span.start,
            .length       = origin.span.length};
      }
      target =
          std::make_shared<render::PickSemanticTarget>(std::move(targetValue));
    }
    const render::PickingTag pickTag{.kind      = render::tagKindOverlay,
                                     .docIndex  = pickScope,
                                     .pageIndex = 0,
                                     .clusterIndex =
                                         static_cast<std::uint32_t>(id)};
    ctx.state.bindOverlayPick(pickTag, target);
    worldCanvas_->setTag(render::tagKindOverlay,
                         static_cast<std::uint32_t>(id));

    const bool isFocus     = (id == accursed_cell_focus_);
    const auto &layout     = cellLayout(id, cell, isFocus);
    const float nodeWidth  = layout.width;
    const float nodeHeight = layout.height;

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
        const float imgMargin = presentation_config_.cellHorizontalPaddingPx;
        const float imgW      = nodeWidth - (imgMargin * 2.0F);
        const float imgH      = std::min(
            imgW * (static_cast<float>(imgRes->height) /
                    static_cast<float>(imgRes->width)),
            nodeHeight - (2.0F * presentation_config_.cellVerticalPaddingPx));
        const float imgLeft = left + imgMargin;
        const float imgBottom =
            bottom + presentation_config_.cellVerticalPaddingPx;
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
    worldCanvas_->addText(
        ctx.state, left + presentation_config_.cellHorizontalPaddingPx,
        bottom + layout.titleTop, layout.idText, borderCol, bgCol);

    // Label Text
    worldCanvas_->setTextWidthLimit(static_cast<int>(layout.labelWidthLimit));
    const auto textMetrics = worldCanvas_->measureText(cell.text);
    const float textLeft =
        left + std::max(presentation_config_.cellHorizontalPaddingPx,
                        (nodeWidth - textMetrics.width) / 2.0F);
    const float textTop = bottom + layout.labelTop;
    worldCanvas_->addText(ctx.state, textLeft, textTop, cell.text, textCol,
                          bgCol, cell.decorated_ranges);
    worldCanvas_->setTextWidthLimit(0);

    // Badges: type, mime, clone
    if (!layout.badgeText.empty()) {
      worldCanvas_->addText(
          ctx.state, left + presentation_config_.cellHorizontalPaddingPx,
          bottom + layout.badgeTop, layout.badgeText, borderCol, bgCol);
    }
  }

  worldCanvas_->commit();
  worldCanvas_->draw(ctx.state, ctx.viewProjection * presentation_transform_,
                     1.0F);

  // --- 3. Draw 2D Screen Overlay HUD ---
  hudCanvas_->clear();
  hudCanvas_->setTag(render::tagKindNone, 0);

  const auto width  = static_cast<float>(ctx.screenWidth);
  const auto height = static_cast<float>(ctx.screenHeight);

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

  const auto structureMetrics = hudCanvas_->measureText(structure_name_);
  const auto focusMetrics     = hudCanvas_->measureText(focusLabel);
  const float topBarHeight =
      (3.0F * presentation_config_.hudVerticalPaddingPx) +
      structureMetrics.height + focusMetrics.height;
  const float topBarBottom = height - topBarHeight;

  // Top Bar Background
  hudCanvas_->addRect(0.0F, topBarBottom, width, topBarHeight, 0x0D0D12DDU);
  hudCanvas_->addLine(0.0F, topBarBottom, width, topBarBottom, 1.0F,
                      0x333344FFU);

  const float structureTop = height - presentation_config_.hudVerticalPaddingPx;
  const float focusTop     = structureTop - structureMetrics.height -
                             presentation_config_.hudVerticalPaddingPx;
  hudCanvas_->addText(ctx.state, presentation_config_.hudHorizontalPaddingPx,
                      structureTop, structure_name_, 0xF4C542FFU, 0x0D0D12DDU);
  hudCanvas_->addText(ctx.state, presentation_config_.hudHorizontalPaddingPx,
                      focusTop, focusLabel, 0xFFFFFFFFU, 0x0D0D12DDU);

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
  hudCanvas_->addText(ctx.state,
                      width - dimsMetrics.width -
                          presentation_config_.hudHorizontalPaddingPx,
                      structureTop, dimsInfo, 0x70B0FFFFU, 0x0D0D12DDU);

  // View Mode Status Indicator
  const std::string modeLabel = (view_mode_ == ViewMode::CellContent)
                                    ? "[ View: 📄 Content (1/V) ]"
                                    : "[ View: 🌐 Topology (2/T) ]";
  const auto modeMetrics      = hudCanvas_->measureText(modeLabel);
  hudCanvas_->addText(ctx.state,
                      width - dimsMetrics.width - modeMetrics.width -
                          presentation_config_.hudHorizontalPaddingPx -
                          presentation_config_.hudColumnGapPx,
                      structureTop, modeLabel, 0xF59E0BFFU, 0x0D0D12DDU);

  // Dimension Bundle Indicator
  const std::string bundleLabel = std::format(
      "[ Bundle: {} (Ctrl+1..5) ]", dimensionBundleName(dimension_bundle_));
  const auto bundleMetrics = hudCanvas_->measureText(bundleLabel);
  hudCanvas_->addText(ctx.state,
                      width - dimsMetrics.width - modeMetrics.width -
                          bundleMetrics.width -
                          presentation_config_.hudHorizontalPaddingPx -
                          (2.0F * presentation_config_.hudColumnGapPx),
                      structureTop, bundleLabel, 0x38BDF8FFU, 0x0D0D12DDU);

  // Bottom Command Key Hints
  const std::string hints =
      "Arrows: Step X/Y | PgUp/PgDn: Step Z | Space: Swap X/Y | Tab: Cycle | "
      "N/D: Insert | U: Unlink | Del: Delete | F4: Palette | / or : or F2: "
      "Omnibar";
  const auto hintsMetrics = hudCanvas_->measureText(hints);
  const float bottomBarHeight =
      hintsMetrics.height + (2.0F * presentation_config_.hudVerticalPaddingPx);
  hudCanvas_->addRect(0.0F, 0.0F, width, bottomBarHeight, 0x0D0D12DDU);
  hudCanvas_->addLine(0.0F, bottomBarHeight, width, bottomBarHeight, 1.0F,
                      0x222233FFU);
  hudCanvas_->addText(ctx.state, presentation_config_.hudHorizontalPaddingPx,
                      bottomBarHeight -
                          presentation_config_.hudVerticalPaddingPx,
                      hints, 0x888899FFU, 0x0D0D12DDU);

  // Palette HUD Overlay
  if (paletteVisible_) {
    const auto items      = paletteItems();
    const float palWidth  = std::min(520.0F, width - 40.0F);
    const float palHeight = std::min(360.0F, height - 120.0F);
    const float palX      = (width - palWidth) * 0.5F;
    const float palY      = (height - palHeight) * 0.5F;

    // Palette background & border
    hudCanvas_->addRect(palX, palY, palWidth, palHeight, 0x141624F0U);
    hudCanvas_->addLine(palX, palY, palX + palWidth, palY, 1.5F, 0x475569FFU);
    hudCanvas_->addLine(palX, palY + palHeight, palX + palWidth,
                        palY + palHeight, 1.5F, 0x475569FFU);
    hudCanvas_->addLine(palX, palY, palX, palY + palHeight, 1.5F, 0x475569FFU);
    hudCanvas_->addLine(palX + palWidth, palY, palX + palWidth,
                        palY + palHeight, 1.5F, 0x475569FFU);

    // Title bar
    const std::string palTitle = "Vortex Opcode & Library Palette [F4 / Esc]";
    const float titleTop       = palY + palHeight - 14.0F;
    hudCanvas_->addText(ctx.state, palX + 16.0F, titleTop, palTitle,
                        0xF59E0BFFU, 0x141624F0U);
    hudCanvas_->addLine(palX, palY + palHeight - 32.0F, palX + palWidth,
                        palY + palHeight - 32.0F, 1.0F, 0x334155FFU);

    // Items list
    const int maxVisible = 8;
    const int total      = static_cast<int>(items.size());
    int startIdx         = 0;
    if (total > maxVisible) {
      startIdx =
          std::clamp(static_cast<int>(paletteSelectedIndex_) - (maxVisible / 2),
                     0, total - maxVisible);
    }
    const int endIdx = std::min(total, startIdx + maxVisible);

    const float itemLineHeight = 28.0F;
    float currentY             = palY + palHeight - 64.0F;
    for (int i = startIdx; i < endIdx; ++i) {
      const bool isSelected =
          (static_cast<std::size_t>(i) == paletteSelectedIndex_);
      if (isSelected) {
        hudCanvas_->addRect(palX + 8.0F, currentY - 6.0F, palWidth - 16.0F,
                            itemLineHeight, 0x1E3A8ABBU);
      }
      const auto &item = items[static_cast<std::size_t>(i)];
      std::uint32_t fg = 0xE2E8F0FFU;
      if (item.starts_with("#")) {
        fg = 0xFBBF24FFU; // gold for opcodes
      } else if (item.starts_with("std:zigzag")) {
        fg = 0x38BDF8FFU; // sky blue for zigzag stdlib
      } else if (item.starts_with("std:gc")) {
        fg = 0x34D399FFU; // emerald for gc stdlib
      } else if (item.starts_with("VQL: ")) {
        fg = 0xC084FCFFU; // lavender for VQL compilation
      }
      const std::string label = (isSelected ? " > " : "   ") + item;
      hudCanvas_->addText(ctx.state, palX + 12.0F, currentY + 14.0F, label, fg,
                          0x00000000U);
      currentY -= itemLineHeight;
    }

    // Bottom prompt
    const std::string palHelp =
        "Up/Down: Navigate | Enter: Clone / Compile VQL | F5: Translate VQL | "
        "Esc: Close";
    hudCanvas_->addText(ctx.state, palX + 16.0F, palY + 24.0F, palHelp,
                        0x94A3B8FFU, 0x141624F0U);
  }

  // Command Omnibar HUD Overlay
  if (commandBarVisible_) {
    const float barWidth  = std::min(700.0F, width - 40.0F);
    const float barHeight = 76.0F;
    const float barX      = (width - barWidth) * 0.5F;
    const float barY      = height - topBarBottom - barHeight - 20.0F;

    // Background & borders
    hudCanvas_->addRect(barX, barY, barWidth, barHeight, 0x0F172AF0U);
    hudCanvas_->addLine(barX, barY, barX + barWidth, barY, 1.5F, 0x38BDF8FFU);
    hudCanvas_->addLine(barX, barY + barHeight, barX + barWidth,
                        barY + barHeight, 1.5F, 0x38BDF8FFU);
    hudCanvas_->addLine(barX, barY, barX, barY + barHeight, 1.5F, 0x38BDF8FFU);
    hudCanvas_->addLine(barX + barWidth, barY, barX + barWidth,
                        barY + barHeight, 1.5F, 0x38BDF8FFU);

    // Mode badge
    std::string badge;
    std::uint32_t badgeColor = 0x38BDF8FFU; // Sky blue
    if (commandBarText_.starts_with(":macro")) {
      badge      = "[MACRO DEF]";
      badgeColor = 0xF59E0BFFU; // Amber
    } else if (commandBarText_.starts_with(":")) {
      badge      = "[COMMAND]";
      badgeColor = 0x818CF8FFU; // Indigo
    } else if (commandBarText_.starts_with("/") ||
               commandBarText_.starts_with("##")) {
      badge      = "[VQL NAV]";
      badgeColor = 0x34D399FFU; // Emerald
    } else if (commandBarText_.starts_with("weave") ||
               commandBarText_.starts_with("let") ||
               commandBarText_.starts_with("for")) {
      badge      = "[VQL SCRIPT]";
      badgeColor = 0xC084FCFFU; // Purple
    } else {
      badge      = "[VQL OMNIBAR]";
      badgeColor = 0x38BDF8FFU; // Sky blue
    }

    const float badgeY = barY + barHeight - 16.0F;
    hudCanvas_->addText(ctx.state, barX + 16.0F, badgeY, badge, badgeColor,
                        0x0F172AF0U);

    // Input prompt line
    const std::string prompt = "> " + commandBarText_ + "_";
    const float inputY       = barY + 36.0F;
    hudCanvas_->addText(ctx.state, barX + 16.0F, inputY, prompt, 0xFFFFFFFFU,
                        0x00000000U);

    // Feedback or helper line
    const float helpY = barY + 14.0F;
    if (!commandBarFeedback_.empty()) {
      const std::uint32_t fbCol =
          commandBarFeedbackIsError_ ? 0xEF4444FFU : 0x34D399FFU;
      hudCanvas_->addText(ctx.state, barX + 16.0F, helpY, commandBarFeedback_,
                          fbCol, 0x00000000U);
    } else {
      hudCanvas_->addText(
          ctx.state, barX + 16.0F, helpY,
          "Enter: Execute | /: Path Nav | weave {...}: Script | :macro <name> "
          "<vql> [key] | Esc: Close",
          0x64748BFFU, 0x00000000U);
    }
  }

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
                                     const std::string_view /*value*/) {
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

bool ZigzagVisualizer::grabbing() const {
  return commandBarVisible_ || paletteVisible_;
}

bool ZigzagVisualizer::keyPressed(const gleditor::Key key,
                                  const gleditor::KeyMods /*mods*/) {
  if (commandBarVisible_) {
    switch (key) {
    case gleditor::Key::Return:
      executeCommandBar();
      return true;
    case gleditor::Key::Escape:
      setCommandBarVisible(false);
      return true;
    case gleditor::Key::Backspace:
      commandBarBackspace();
      return true;
    default:
      return false;
    }
  }
  if (paletteVisible_) {
    switch (key) {
    case gleditor::Key::Return:
      paletteCloneSelectedToFocus();
      setPaletteVisible(false);
      return true;
    case gleditor::Key::Escape:
      setPaletteVisible(false);
      return true;
    case gleditor::Key::Up:
      palettePrev();
      return true;
    case gleditor::Key::Down:
      paletteNext();
      return true;
    case gleditor::Key::Backspace:
      paletteBackspace();
      return true;
    default:
      return false;
    }
  }
  if (key == gleditor::Key::Return) {
    const auto focus = static_cast<CellRef>(accursed_cell_focus_);
    if (isCellLocked(focus)) {
      unlockCell(focus);
    }
    activateCell(focus);
    return true;
  }
  return false;
}

void ZigzagVisualizer::textTyped(const std::string &utf8) {
  if (commandBarVisible_) {
    commandBarInputText(utf8);
  } else if (paletteVisible_) {
    paletteInputText(utf8);
  }
}

std::optional<gleditor::InputArea> ZigzagVisualizer::textArea() const {
  if (commandBarVisible_) {
    return gleditor::InputArea{
        .x      = 20,
        .y      = 100,
        .width  = 700,
        .height = 76,
    };
  }
  if (paletteVisible_) {
    return gleditor::InputArea{
        .x      = 100,
        .y      = 100,
        .width  = 520,
        .height = 360,
    };
  }
  return std::nullopt;
}

bool ZigzagVisualizer::unlockCell(const CellRef cell) {
  if (!engine_) {
    return false;
  }
  if (!engine_->unlockTranscopyright(cell)) {
    return false;
  }
  ++revision_;
  rebuildActiveViewTopology();
  invalidateAccessibility();
  if (bridgeInvalidationCallback_) {
    bridgeInvalidationCallback_(revision_);
  }
  return true;
}

void ZigzagVisualizer::activateCell(const CellRef cell) {
  if (cellActivationCallback_) {
    cellActivationCallback_(cell);
  }
}

std::optional<xanadu::CellAnchor>
ZigzagVisualizer::cellAnchor(const CellRef cell) const {
  const auto it = visible_cells_.find(static_cast<CellID>(cell));
  if (it == visible_cells_.end()) {
    return std::nullopt;
  }
  const auto &c = it->second;
  if (c.current_alpha < 0.02F && c.target_alpha < 0.02F) {
    return std::nullopt;
  }
  const auto &layout =
      cellLayout(static_cast<CellID>(cell), c,
                 static_cast<CellID>(cell) == accursed_cell_focus_);

  const float xScale = glm::length(glm::vec3(presentation_transform_[0]));
  const float yScale = glm::length(glm::vec3(presentation_transform_[1]));
  return xanadu::CellAnchor{
      .position =
          glm::vec3(presentation_transform_ * glm::vec4(c.current_pos, 1.0F)),
      .width      = layout.width * xScale,
      .height     = layout.height * yScale,
      .lineHeight = layout.labelLineHeight * yScale,
      .normal     = glm::vec3(0.0F, 0.0F, 1.0F),
  };
}

void ZigzagVisualizer::setDimensionBundle(DimensionBundle bundle) {
  dimension_bundle_ = bundle;
  if (bundle != DimensionBundle::Custom) {
    current_view_ = dimensionBundleAxes(bundle);
    if (engine_) {
      for (const auto &dName :
           {current_view_.x_dimension, current_view_.y_dimension,
            current_view_.z_dimension}) {
        if (!dName.empty()) {
          static_cast<void>(engine_->dimensionFor(dName));
        }
      }
    }
    refreshCellLayouts();
    rebuildActiveViewTopology();
    invalidateAccessibility();
  }
}

void ZigzagVisualizer::cycleDimensionBundle(const bool forward) {
  auto current = static_cast<int>(dimension_bundle_);
  if (forward) {
    current = (current >= 5) ? 1 : current + 1;
  } else {
    current = (current <= 1) ? 5 : current - 1;
  }
  setDimensionBundle(static_cast<DimensionBundle>(current));
}

void ZigzagVisualizer::attachVortexHost(
    std::shared_ptr<vortex::VortexHost> host) {
  vortex_host_ = std::move(host);
}

std::shared_ptr<vortex::VortexHost> ZigzagVisualizer::vortexHost() noexcept {
  if (!vortex_host_) {
    ensureVortexHost();
  }
  return vortex_host_;
}

void ZigzagVisualizer::ensureVortexHost() {
  if (engine_) {
    vortex_host_ = std::make_shared<vortex::VortexHost>(&engine_->manifold());
    if (store_) {
      vortex_host_->bindStore(store_);
    } else {
      vortex_host_->bindStore(&engine_->store());
    }
  }
}

bool ZigzagVisualizer::dispatchAction(std::string_view actionName) {
  if (!vortex_host_) {
    ensureVortexHost();
  }
  if (vortex_host_ && accursed_cell_focus_ != 0) {
    if (vortex_host_->hasCustomAction(actionName)) {
      CellRef newFocus   = zigzag::noCell;
      const auto oldView = current_view_;
      if (vortex_host_->dispatchAction(
              actionName, static_cast<CellRef>(accursed_cell_focus_),
              current_view_, newFocus)) {
        if (engine_) {
          engine_->syncIncremental();
        }
        if (oldView != current_view_) {
          dimension_bundle_ = DimensionBundle::Custom;
          rebuildActiveViewTopology();
          invalidateAccessibility();
        }
        if (newFocus != zigzag::noCell &&
            newFocus != static_cast<CellRef>(accursed_cell_focus_)) {
          navigateFocusTo(static_cast<CellID>(newFocus));
        } else {
          rebuildActiveViewTopology();
          invalidateAccessibility();
        }
        return true;
      }
    }
  }

  // Navigation actions (leverage vortex stdlib when host is attached)
  if (actionName == "step-x-pos") {
    if (vortex_host_ && accursed_cell_focus_ != 0) {
      CellRef newFocus = zigzag::noCell;
      if (vortex_host_->dispatchAction(
              actionName, static_cast<CellRef>(accursed_cell_focus_),
              current_view_, newFocus)) {
        if (newFocus != zigzag::noCell) {
          navigateFocusTo(static_cast<CellID>(newFocus));
          return true;
        }
      }
    }
    navigateFocus(current_view_.x_dimension, DimVector::POS);
    return true;
  }
  if (actionName == "step-x-neg") {
    if (vortex_host_ && accursed_cell_focus_ != 0) {
      CellRef newFocus = zigzag::noCell;
      if (vortex_host_->dispatchAction(
              actionName, static_cast<CellRef>(accursed_cell_focus_),
              current_view_, newFocus)) {
        if (newFocus != zigzag::noCell) {
          navigateFocusTo(static_cast<CellID>(newFocus));
          return true;
        }
      }
    }
    navigateFocus(current_view_.x_dimension, DimVector::NEG);
    return true;
  }
  if (actionName == "step-y-pos") {
    if (vortex_host_ && accursed_cell_focus_ != 0) {
      CellRef newFocus = zigzag::noCell;
      if (vortex_host_->dispatchAction(
              actionName, static_cast<CellRef>(accursed_cell_focus_),
              current_view_, newFocus)) {
        if (newFocus != zigzag::noCell) {
          navigateFocusTo(static_cast<CellID>(newFocus));
          return true;
        }
      }
    }
    navigateFocus(current_view_.y_dimension, DimVector::POS);
    return true;
  }
  if (actionName == "step-y-neg") {
    if (vortex_host_ && accursed_cell_focus_ != 0) {
      CellRef newFocus = zigzag::noCell;
      if (vortex_host_->dispatchAction(
              actionName, static_cast<CellRef>(accursed_cell_focus_),
              current_view_, newFocus)) {
        if (newFocus != zigzag::noCell) {
          navigateFocusTo(static_cast<CellID>(newFocus));
          return true;
        }
      }
    }
    navigateFocus(current_view_.y_dimension, DimVector::NEG);
    return true;
  }
  if (actionName == "step-z-pos") {
    if (vortex_host_ && accursed_cell_focus_ != 0) {
      CellRef newFocus = zigzag::noCell;
      if (vortex_host_->dispatchAction(
              actionName, static_cast<CellRef>(accursed_cell_focus_),
              current_view_, newFocus)) {
        if (newFocus != zigzag::noCell) {
          navigateFocusTo(static_cast<CellID>(newFocus));
          return true;
        }
      }
    }
    navigateFocus(current_view_.z_dimension, DimVector::POS);
    return true;
  }
  if (actionName == "step-z-neg") {
    if (vortex_host_ && accursed_cell_focus_ != 0) {
      CellRef newFocus = zigzag::noCell;
      if (vortex_host_->dispatchAction(
              actionName, static_cast<CellRef>(accursed_cell_focus_),
              current_view_, newFocus)) {
        if (newFocus != zigzag::noCell) {
          navigateFocusTo(static_cast<CellID>(newFocus));
          return true;
        }
      }
    }
    navigateFocus(current_view_.z_dimension, DimVector::NEG);
    return true;
  }

  // Visualizer document editing actions
  if (actionName == "insert-cell-x-pos") {
    const bool ok = insertConnectedCell("New Cell", current_view_.x_dimension,
                                        DimVector::POS);
    if (ok && vortex_host_) {
      CellRef dummy = zigzag::noCell;
      vortex_host_->dispatchAction(actionName,
                                   static_cast<CellRef>(accursed_cell_focus_),
                                   current_view_, dummy);
    }
    return ok;
  }
  if (actionName == "insert-cell-x-neg") {
    const bool ok = insertConnectedCell("New Cell", current_view_.x_dimension,
                                        DimVector::NEG);
    if (ok && vortex_host_) {
      CellRef dummy = zigzag::noCell;
      vortex_host_->dispatchAction(actionName,
                                   static_cast<CellRef>(accursed_cell_focus_),
                                   current_view_, dummy);
    }
    return ok;
  }
  if (actionName == "insert-cell-y-pos") {
    const bool ok = insertConnectedCell("New Cell", current_view_.y_dimension,
                                        DimVector::POS);
    if (ok && vortex_host_) {
      CellRef dummy = zigzag::noCell;
      vortex_host_->dispatchAction(actionName,
                                   static_cast<CellRef>(accursed_cell_focus_),
                                   current_view_, dummy);
    }
    return ok;
  }
  if (actionName == "insert-cell-y-neg") {
    const bool ok = insertConnectedCell("New Cell", current_view_.y_dimension,
                                        DimVector::NEG);
    if (ok && vortex_host_) {
      CellRef dummy = zigzag::noCell;
      vortex_host_->dispatchAction(actionName,
                                   static_cast<CellRef>(accursed_cell_focus_),
                                   current_view_, dummy);
    }
    return ok;
  }
  if (actionName == "unlink-x-pos") {
    const bool ok = unlinkFocusAlong(current_view_.x_dimension, DimVector::POS);
    if (ok && vortex_host_) {
      CellRef dummy = zigzag::noCell;
      vortex_host_->dispatchAction(actionName,
                                   static_cast<CellRef>(accursed_cell_focus_),
                                   current_view_, dummy);
    }
    return ok;
  }
  if (actionName == "unlink-x-neg") {
    const bool ok = unlinkFocusAlong(current_view_.x_dimension, DimVector::NEG);
    if (ok && vortex_host_) {
      CellRef dummy = zigzag::noCell;
      vortex_host_->dispatchAction(actionName,
                                   static_cast<CellRef>(accursed_cell_focus_),
                                   current_view_, dummy);
    }
    return ok;
  }
  if (actionName == "delete-focus-cell") {
    const bool ok = deleteFocusCell();
    if (ok && vortex_host_) {
      CellRef dummy = zigzag::noCell;
      vortex_host_->dispatchAction(actionName,
                                   static_cast<CellRef>(accursed_cell_focus_),
                                   current_view_, dummy);
    }
    return ok;
  }
  if (actionName == "duplicate-focus-cell") {
    if (vortex_host_ && accursed_cell_focus_ != 0) {
      CellRef newFocus = zigzag::noCell;
      if (vortex_host_->dispatchAction(
              actionName, static_cast<CellRef>(accursed_cell_focus_),
              current_view_, newFocus)) {
        if (newFocus != zigzag::noCell) {
          navigateFocusTo(static_cast<CellID>(newFocus));
          return true;
        }
      }
    }
    return false;
  }
  if (actionName == "hop-head") {
    if (vortex_host_ && accursed_cell_focus_ != 0) {
      CellRef newFocus = zigzag::noCell;
      if (vortex_host_->dispatchAction(
              actionName, static_cast<CellRef>(accursed_cell_focus_),
              current_view_, newFocus)) {
        if (newFocus != zigzag::noCell) {
          navigateFocusTo(static_cast<CellID>(newFocus));
          return true;
        }
      }
    }
    return true;
  }
  if (actionName == "hop-tail") {
    if (vortex_host_ && accursed_cell_focus_ != 0) {
      CellRef newFocus = zigzag::noCell;
      if (vortex_host_->dispatchAction(
              actionName, static_cast<CellRef>(accursed_cell_focus_),
              current_view_, newFocus)) {
        if (newFocus != zigzag::noCell) {
          navigateFocusTo(static_cast<CellID>(newFocus));
          return true;
        }
      }
    }
    return true;
  }
  if (actionName == "jump-home") {
    if (vortex_host_) {
      CellRef newFocus = zigzag::noCell;
      if (vortex_host_->dispatchAction(
              actionName, static_cast<CellRef>(accursed_cell_focus_),
              current_view_, newFocus)) {
        if (newFocus != zigzag::noCell) {
          navigateFocusTo(static_cast<CellID>(newFocus));
          return true;
        }
      }
    }
    if (engine_) {
      navigateFocusTo(engine_->manifold().home());
      return true;
    }
    return false;
  }
  if (actionName == "swap-xy") {
    if (vortex_host_) {
      CellRef dummy = zigzag::noCell;
      vortex_host_->dispatchAction(actionName,
                                   static_cast<CellRef>(accursed_cell_focus_),
                                   current_view_, dummy);
      dimension_bundle_ = DimensionBundle::Custom;
      rebuildActiveViewTopology();
      invalidateAccessibility();
      return true;
    }
    swapDimensions(0, 1);
    return true;
  }
  if (actionName == "cycle-dims-forward") {
    if (vortex_host_) {
      CellRef dummy = zigzag::noCell;
      vortex_host_->dispatchAction(actionName,
                                   static_cast<CellRef>(accursed_cell_focus_),
                                   current_view_, dummy);
      dimension_bundle_ = DimensionBundle::Custom;
      rebuildActiveViewTopology();
      invalidateAccessibility();
      return true;
    }
    cycleDimensions(true);
    return true;
  }
  if (actionName == "cycle-dims-backward") {
    if (vortex_host_) {
      CellRef dummy = zigzag::noCell;
      vortex_host_->dispatchAction(actionName,
                                   static_cast<CellRef>(accursed_cell_focus_),
                                   current_view_, dummy);
      dimension_bundle_ = DimensionBundle::Custom;
      rebuildActiveViewTopology();
      invalidateAccessibility();
      return true;
    }
    cycleDimensions(false);
    return true;
  }
  return false;
}

void ZigzagVisualizer::togglePalette() { setPaletteVisible(!paletteVisible_); }

void ZigzagVisualizer::setPaletteVisible(const bool visible) {
  paletteVisible_ = visible;
  if (visible) {
    paletteSelectedIndex_ = 0;
  }
}

void ZigzagVisualizer::paletteNext() {
  const auto items = paletteItems();
  if (items.empty()) {
    paletteSelectedIndex_ = 0;
    return;
  }
  paletteSelectedIndex_ = (paletteSelectedIndex_ + 1) % items.size();
}

void ZigzagVisualizer::palettePrev() {
  const auto items = paletteItems();
  if (items.empty()) {
    paletteSelectedIndex_ = 0;
    return;
  }
  if (paletteSelectedIndex_ == 0) {
    paletteSelectedIndex_ = items.size() - 1;
  } else {
    --paletteSelectedIndex_;
  }
}

bool ZigzagVisualizer::paletteCloneSelectedToFocus() {
  const auto items = paletteItems();
  if (items.empty() || paletteSelectedIndex_ >= items.size()) {
    return false;
  }
  const auto &selectedItem = items[paletteSelectedIndex_];
  if (selectedItem.starts_with("VQL: ")) {
    return paletteTranslateVQL(selectedItem.substr(5));
  }
  if (vortex_host_ && accursed_cell_focus_ != 0) {
    vortex_host_->cloneSymbolToChain(
        selectedItem, static_cast<CellRef>(accursed_cell_focus_));
  }
  return insertConnectedCell(selectedItem, "d.step", DimVector::POS);
}

bool ZigzagVisualizer::paletteTranslateVQL(std::string_view query) {
  std::string_view vql = query;
  if (vql.empty()) {
    vql = paletteFilter_;
  }
  if (vql.starts_with("vql:") || vql.starts_with("VQL:")) {
    vql.remove_prefix(4);
    while (!vql.empty() && std::isspace(vql.front())) {
      vql.remove_prefix(1);
    }
  }
  if (vql.empty()) {
    return false;
  }
  return translateVQLAndAttachToFocus(vql, "d.spin", false);
}

bool ZigzagVisualizer::translateVQLAndAttachToFocus(std::string_view vqlQuery,
                                                    std::string_view attachDim,
                                                    bool spawnCursor) {
  if (vqlQuery.empty() || accursed_cell_focus_ == 0) {
    return false;
  }
  ensureVortexHost();
  if (!vortex_host_) {
    return false;
  }

  // Ensure standard Vortex dimensions exist in engine and store
  if (engine_) {
    for (const auto &dim :
         {std::string(attachDim), std::string("d.spin"), std::string("d.step"),
          std::string("d.grab"), std::string("d.vars"), std::string("d.values"),
          std::string("d.branch")}) {
      static_cast<void>(engine_->dimensionFor(dim));
    }
  }

  const auto focusRef = static_cast<CellRef>(accursed_cell_focus_);
  auto result = vortex_host_->compileAndAttachVQL(vqlQuery, focusRef, attachDim,
                                                  DimVector::POS, spawnCursor);
  if (!result.success || result.entryOpcode == zigzag::noCell) {
    return false;
  }

  // If visualizing a persistent document backed by store and engine, promote
  if (store_ && engine_) {
    auto promoted = vortex_host_->promoteAndAttachToStore(
        result.entryOpcode, focusRef, attachDim, DimVector::POS, *store_,
        engine_->head());
    if (promoted && !promoted->cells.empty()) {
      engine_->syncTo(promoted->version);
      accursed_cell_focus_ = promoted->cells.front();
      refreshCellLayouts();
      rebuildActiveViewTopology();
      invalidateAccessibility();
      return true;
    }
  }

  // Pure in-memory / arena mode
  accursed_cell_focus_ = result.entryOpcode;
  refreshCellLayouts();
  rebuildActiveViewTopology();
  invalidateAccessibility();
  return true;
}

xanadu::vql::CompilationResult
ZigzagVisualizer::compileVQL(std::string_view vqlQuery) const {
  if (!vortex_host_) {
    const_cast<ZigzagVisualizer *>(this)->ensureVortexHost();
  }
  if (!vortex_host_) {
    return xanadu::vql::CompilationResult{
        .success          = false,
        .entryOpcode      = zigzag::noCell,
        .errorMessage     = "VortexHost unavailable",
        .generatedOpcodes = {},
        .disassembly      = {},
    };
  }
  xanadu::vql::CompilationOptions options;
  options.targetLibrary = false;
  return vortex_host_->vqlCompiler().compile(vqlQuery, options);
}

void ZigzagVisualizer::setPaletteFilter(std::string filter) {
  paletteFilter_        = std::move(filter);
  paletteSelectedIndex_ = 0;
}

void ZigzagVisualizer::paletteInputText(const std::string_view text) {
  paletteFilter_.append(text);
  paletteSelectedIndex_ = 0;
}

void ZigzagVisualizer::paletteBackspace() {
  if (!paletteFilter_.empty()) {
    paletteFilter_.pop_back();
    paletteSelectedIndex_ = 0;
  }
}

std::vector<std::string> ZigzagVisualizer::paletteItems() const {
  std::vector<std::string> items = {
      "#LINK",
      "#VALUE",
      "#BIND",
      "#RESOLVE",
      "#CALL",
      "#RETURN",
      "#BRANCH",
      "std:zigzag/step",
      "std:zigzag/insert",
      "std:zigzag/unlink",
      "std:zigzag/link",
      "std:zigzag/delete",
      "std:zigzag/clone_to_chain",
      "std:gc/sweep",
  };
  if (vortex_host_) {
    for (const auto &mod : vortex_host_->availableModules()) {
      for (const auto &sym : vortex_host_->symbolsInModule(mod)) {
        std::string full = mod + "/" + sym;
        if (std::find(items.begin(), items.end(), full) == items.end()) {
          items.push_back(full);
        }
      }
    }
  }
  if (paletteFilter_.empty()) {
    return items;
  }

  if (paletteFilter_.starts_with("/") || paletteFilter_.starts_with("let ") ||
      paletteFilter_.starts_with("weave ") ||
      paletteFilter_.starts_with("vql:") ||
      paletteFilter_.starts_with("VQL:") || paletteFilter_.starts_with("##")) {
    std::string vqlQuery = paletteFilter_;
    if (vqlQuery.starts_with("vql:") || vqlQuery.starts_with("VQL:")) {
      vqlQuery = vqlQuery.substr(4);
      while (!vqlQuery.empty() && std::isspace(vqlQuery.front())) {
        vqlQuery.erase(vqlQuery.begin());
      }
    }
    return {"VQL: " + vqlQuery};
  }

  std::vector<std::string> filtered;
  for (const auto &item : items) {
    if (item.contains(paletteFilter_)) {
      filtered.push_back(item);
    }
  }
  if (filtered.empty() && !paletteFilter_.empty()) {
    filtered.push_back("VQL: " + paletteFilter_);
  }
  return filtered;
}

void ZigzagVisualizer::toggleCommandBar() {
  setCommandBarVisible(!commandBarVisible_);
}

void ZigzagVisualizer::setCommandBarVisible(const bool visible) {
  commandBarVisible_ = visible;
  if (commandBarVisible_) {
    commandBarFeedback_.clear();
    commandBarFeedbackIsError_ = false;
  }
}

void ZigzagVisualizer::commandBarInputChar(const char ch) {
  commandBarText_.push_back(ch);
}

void ZigzagVisualizer::commandBarInputText(const std::string_view text) {
  commandBarText_.append(text);
}

void ZigzagVisualizer::commandBarBackspace() {
  if (!commandBarText_.empty()) {
    commandBarText_.pop_back();
  }
}

void ZigzagVisualizer::commandBarClear() {
  commandBarText_.clear();
  commandBarFeedback_.clear();
  commandBarFeedbackIsError_ = false;
}

void ZigzagVisualizer::setCommandBarText(std::string text) {
  commandBarText_ = std::move(text);
}

bool ZigzagVisualizer::navigateVQL(const std::string_view pathExpr) {
  ensureVortexHost();
  if (!vortex_host_) {
    commandBarFeedback_        = "VortexHost unavailable";
    commandBarFeedbackIsError_ = true;
    return false;
  }
  const auto focusRef = static_cast<CellRef>(accursed_cell_focus_);
  auto target         = vortex_host_->navigatePath(pathExpr, focusRef);
  if (target.has_value() && *target != zigzag::noCell) {
    navigateFocusTo(static_cast<CellID>(*target));
    commandBarFeedback_        = std::format("Navigated to cell {}", *target);
    commandBarFeedbackIsError_ = false;
    return true;
  }
  commandBarFeedback_ = std::format("Path '{}' did not resolve", pathExpr);
  commandBarFeedbackIsError_ = true;
  return false;
}

vortex::VortexHost::ScriptResult
ZigzagVisualizer::executeVQLScript(const std::string_view script) {
  ensureVortexHost();
  if (!vortex_host_) {
    return {.success = false, .message = "VortexHost unavailable"};
  }
  const auto focusRef = static_cast<CellRef>(accursed_cell_focus_);
  auto res            = vortex_host_->executeScript(script, focusRef, store_);
  commandBarFeedback_ = res.message;
  commandBarFeedbackIsError_ = !res.success;
  if (res.success) {
    if (!res.affectedCells.empty()) {
      navigateFocusTo(static_cast<CellID>(res.affectedCells.back()));
    } else {
      refreshCellLayouts();
      rebuildActiveViewTopology();
      invalidateAccessibility();
    }
  }
  return res;
}

bool ZigzagVisualizer::defineMacro(const std::string_view name,
                                   const std::string_view vqlExpr,
                                   const std::string_view keyBinding) {
  ensureVortexHost();
  if (!vortex_host_) {
    commandBarFeedback_        = "VortexHost unavailable";
    commandBarFeedbackIsError_ = true;
    return false;
  }
  if (store_) {
    static_cast<void>(
        vortex_host_->saveMacroToStore(name, vqlExpr, keyBinding, *store_));
  } else {
    vortex_host_->defineMacro(name, vqlExpr, nullptr);
  }
  commandBarFeedback_ =
      std::format("Macro '{}' defined -> '{}'", name, vqlExpr);
  commandBarFeedbackIsError_ = false;
  return true;
}

bool ZigzagVisualizer::executeCommandBar() {
  std::string_view text = commandBarText_;
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  if (text.empty()) {
    commandBarFeedback_        = "Empty command";
    commandBarFeedbackIsError_ = true;
    return false;
  }

  // 1. View configuration (:view <dimX> [dimY] [dimZ])
  if (text.starts_with(":view ") || text.starts_with(":view\t")) {
    std::string_view rest = text.substr(6);
    std::vector<std::string> dims;
    std::istringstream iss{std::string(rest)};
    std::string d;
    while (iss >> d) {
      dims.push_back(d);
    }
    if (dims.empty()) {
      commandBarFeedback_        = "Usage: :view <dimX> [dimY] [dimZ]";
      commandBarFeedbackIsError_ = true;
      return false;
    }
    std::string dimX = !dims.empty() ? dims[0] : "";
    std::string dimY = dims.size() > 1 ? dims[1] : "";
    std::string dimZ = dims.size() > 2 ? dims[2] : "";
    if (vortex_host_) {
      vortex_host_->setView(current_view_, dimX, dimY, dimZ);
    } else {
      if (!dimX.empty()) current_view_.x_dimension = dimX;
      if (!dimY.empty()) current_view_.y_dimension = dimY;
      if (!dimZ.empty()) current_view_.z_dimension = dimZ;
    }
    dimension_bundle_ = DimensionBundle::Custom;
    rebuildActiveViewTopology();
    invalidateAccessibility();
    commandBarFeedback_ = "Set view dimensions: " + current_view_.x_dimension +
                          ", " + current_view_.y_dimension + ", " +
                          current_view_.z_dimension;
    commandBarFeedbackIsError_ = false;
    return true;
  }

  // 2. Direct routine invocation (:call <fn> [args...])
  if (text.starts_with(":call ") || text.starts_with(":call\t")) {
    std::string_view rest = text.substr(6);
    std::istringstream iss{std::string(rest)};
    std::string fnPath;
    if (!(iss >> fnPath)) {
      commandBarFeedback_        = "Usage: :call <module/symbol> [args...]";
      commandBarFeedbackIsError_ = true;
      return false;
    }
    std::vector<vortex::CellValue> args;
    std::string argStr;
    while (iss >> argStr) {
      try {
        if (argStr.contains('.')) {
          args.emplace_back(std::stod(argStr));
        } else {
          args.emplace_back(static_cast<std::int64_t>(std::stoll(argStr)));
        }
      } catch (...) {
        args.emplace_back(argStr);
      }
    }
    if (fnPath == "std:ui/view") {
      std::string dimX =
          !args.empty() && std::holds_alternative<std::string>(args[0])
              ? std::get<std::string>(args[0])
              : "";
      std::string dimY =
          args.size() > 1 && std::holds_alternative<std::string>(args[1])
              ? std::get<std::string>(args[1])
              : "";
      std::string dimZ =
          args.size() > 2 && std::holds_alternative<std::string>(args[2])
              ? std::get<std::string>(args[2])
              : "";
      if (vortex_host_) {
        vortex_host_->setView(current_view_, dimX, dimY, dimZ);
      } else {
        if (!dimX.empty()) current_view_.x_dimension = dimX;
        if (!dimY.empty()) current_view_.y_dimension = dimY;
        if (!dimZ.empty()) current_view_.z_dimension = dimZ;
      }
      dimension_bundle_ = DimensionBundle::Custom;
      rebuildActiveViewTopology();
      invalidateAccessibility();
      commandBarFeedback_        = "View updated via std:ui/view";
      commandBarFeedbackIsError_ = false;
      return true;
    }
    if (vortex_host_) {
      auto res = vortex_host_->stdlib().call(fnPath, args);
      commandBarFeedback_ =
          "Called " + fnPath + " -> " + std::to_string(res.size()) + " results";
      commandBarFeedbackIsError_ = false;
      return true;
    }
    commandBarFeedback_        = "No vortex host attached";
    commandBarFeedbackIsError_ = true;
    return false;
  }

  // 3. Sovereign Library export (:export-lib <module> <path>)
  if (text.starts_with(":export-lib ") || text.starts_with(":export-lib\t")) {
    std::string_view rest = text.substr(12);
    std::istringstream iss{std::string(rest)};
    std::string modName;
    std::string destPath;
    if (!(iss >> modName >> destPath)) {
      commandBarFeedback_        = "Usage: :export-lib <module> <path>";
      commandBarFeedbackIsError_ = true;
      return false;
    }
    if (vortex_host_ && vortex_host_->exportLibrary(modName, destPath)) {
      commandBarFeedback_ = "Exported library " + modName + " to " + destPath;
      commandBarFeedbackIsError_ = false;
      return true;
    }
    commandBarFeedback_        = "Failed to export library " + modName;
    commandBarFeedbackIsError_ = true;
    return false;
  }

  // 4. Sovereign Library import (:import-lib <path>)
  if (text.starts_with(":import-lib ") || text.starts_with(":import-lib\t")) {
    std::string_view rest = text.substr(12);
    std::string srcPath   = std::string(rest);
    while (!srcPath.empty() &&
           std::isspace(static_cast<unsigned char>(srcPath.front()))) {
      srcPath.erase(srcPath.begin());
    }
    while (!srcPath.empty() &&
           std::isspace(static_cast<unsigned char>(srcPath.back()))) {
      srcPath.pop_back();
    }
    if (srcPath.empty()) {
      commandBarFeedback_        = "Usage: :import-lib <path>";
      commandBarFeedbackIsError_ = true;
      return false;
    }
    if (vortex_host_ && vortex_host_->importLibrary(srcPath)) {
      commandBarFeedback_        = "Imported library from " + srcPath;
      commandBarFeedbackIsError_ = false;
      return true;
    }
    commandBarFeedback_        = "Failed to import library from " + srcPath;
    commandBarFeedbackIsError_ = true;
    return false;
  }

  // 5. Sovereign store save (:save [path])
  if (text == ":save" || text.starts_with(":save ") ||
      text.starts_with(":save\t")) {
    std::string path;
    if (text.size() > 5) {
      path = std::string(text.substr(6));
      while (!path.empty() &&
             std::isspace(static_cast<unsigned char>(path.front()))) {
        path.erase(path.begin());
      }
      while (!path.empty() &&
             std::isspace(static_cast<unsigned char>(path.back()))) {
        path.pop_back();
      }
    }
    if (saveStore(path)) {
      commandBarFeedback_ =
          "Saved store to " + (path.empty() ? current_slice_path_ : path);
      commandBarFeedbackIsError_ = false;
      return true;
    }
    commandBarFeedback_        = "Failed to save store";
    commandBarFeedbackIsError_ = true;
    return false;
  }

  // 6. Command mode (:macro ...)
  if (text.starts_with(":macro ") || text.starts_with(":macro\t")) {
    std::string_view rest = text.substr(7);
    while (!rest.empty() &&
           std::isspace(static_cast<unsigned char>(rest.front()))) {
      rest.remove_prefix(1);
    }
    auto nameEnd = rest.find_first_of(" \t");
    if (nameEnd == std::string_view::npos) {
      commandBarFeedback_        = "Usage: :macro <name> <vql> [binding]";
      commandBarFeedbackIsError_ = true;
      return false;
    }
    std::string_view macroName = rest.substr(0, nameEnd);
    rest                       = rest.substr(nameEnd + 1);
    while (!rest.empty() &&
           std::isspace(static_cast<unsigned char>(rest.front()))) {
      rest.remove_prefix(1);
    }
    if (rest.empty()) {
      commandBarFeedback_        = "Usage: :macro <name> <vql> [binding]";
      commandBarFeedbackIsError_ = true;
      return false;
    }

    std::string_view vqlExpr    = rest;
    std::string_view keyBinding = {};
    auto lastSpace              = rest.rfind(' ');
    if (lastSpace != std::string_view::npos) {
      std::string_view potentialKey = rest.substr(lastSpace + 1);
      if (potentialKey.starts_with("Ctrl+") ||
          potentialKey.starts_with("Alt+") ||
          potentialKey.starts_with("Shift+") || potentialKey.starts_with("F")) {
        vqlExpr    = rest.substr(0, lastSpace);
        keyBinding = potentialKey;
      }
    }
    return defineMacro(macroName, vqlExpr, keyBinding);
  }

  // 2. Navigation mode (starts with '/' or '##')
  if (text.starts_with("/") || text.starts_with("##")) {
    return navigateVQL(text);
  }

  // 3. Script / Weave mode
  auto res = executeVQLScript(text);
  return res.success;
}

} // namespace zigzag
