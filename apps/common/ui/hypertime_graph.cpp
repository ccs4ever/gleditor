/**
 * @file hypertime_graph.cpp
 * @brief Interactive 2D Hypertime Branching DAG and Unlimited N-Way Visual Diff
 * Engine.
 */
#include "hypertime_graph.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <gleditor/canvas.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/sdl_compat.hpp>

namespace xanadu::ui {
namespace ui = gleditor::ui;

namespace {

// Keep the established gesture response while presentation dimensions follow
// the live theme. One wheel notch pans about one touch target.
constexpr float kPanStepLogicalPx = 48.F;
constexpr float kZoomNotchFactor  = 1.12F;
constexpr float kMinimumZoom      = .4F;
constexpr float kMaximumZoom      = 3.F;

constexpr std::array<std::uint32_t, 6> kLineageHues = {
    0x00E5FFFF, // Cyan
    0xFFB74DFF, // Amber
    0xBA68C8FF, // Purple
    0x4CAF50FF, // Emerald
    0xFF80ABFF, // Pink
    0x42A5F5FF, // Blue
};

std::string passagePreview(const SingleVersionDiff &version, DiffKind kind) {
  const auto found =
      std::ranges::find_if(version.spans, [kind](const auto &span) {
        return span.kind == kind && span.length > 0;
      });
  if (found == version.spans.end() || found->offset >= version.text.size())
    return "none";
  auto text = version.text.substr(found->offset, found->length);
  std::ranges::replace(text, '\n', ' ');
  std::ranges::replace(text, '\r', ' ');
  return text;
}

} // namespace

HypertimeGraph::HypertimeGraph(
    std::string aFontName, std::function<const Store &(std::size_t)> storeAt,
    std::function<std::uint64_t()> generation)
    : fontName_(std::move(aFontName)), storeAt_(std::move(storeAt)),
      generation_(std::move(generation)),
      overlay_({.id = 1, .model = gleditor::ui::PositionedPanel{}}) {
  overlay_.setVisible(false);
  overlay_.setActionHandler([this](const auto &action) { queue(action); });
}

HypertimeGraph::~HypertimeGraph() = default;

void HypertimeGraph::scroll(const float horizontal, const float vertical,
                            const bool zoom, const bool shift,
                            const float pointerX, const float pointerY) {
  const std::scoped_lock lock(guard_);
  if (!visible_) return;
  if (zoom) {
    const float next = std::clamp(zoom_ * std::pow(kZoomNotchFactor, vertical),
                                  kMinimumZoom, kMaximumZoom);
    const float factor   = next / zoom_;
    const float diameter = nodes_.empty() ? 0 : nodes_.front().radius * 2;
    const float originX  = graphBounds_.left + diameter;
    const float originY  = graphBounds_.bottom + graphBounds_.height - diameter;
    panX_ = pointerX - originX - (pointerX - originX - panX_) * factor;
    panY_ = pointerY - originY - (pointerY - originY - panY_) * factor;
    zoom_ = next;
  } else {
    panX_ += (-horizontal + (shift ? vertical : 0.0F)) *
             metrics_.px(kPanStepLogicalPx);
    panY_ -= (shift ? 0.0F : vertical) * metrics_.px(kPanStepLogicalPx);
  }
  builtAt = std::numeric_limits<std::uint64_t>::max();
  changed();
}

void HypertimeGraph::deviceReady(render::RenderDevice &device,
                                 const render::PipelineDesc &documentPipeline) {
  const std::scoped_lock lock(guard_);
  overlay_.deviceReady(device, documentPipeline);
  canvas_ = std::make_unique<gleditor::Canvas>(
      &device, gleditor::ui::scaledFontDescription(
                   fontName_, gleditor::ui::FontRole::Body, {},
                   gleditor::ui::defaultTheme()));
  canvas_->createPipeline(documentPipeline, false);
}

char HypertimeGraph::opKindLetter(const std::optional<OpKind> kind) noexcept {
  if (!kind.has_value()) {
    return 'G'; // Genesis / root
  }
  switch (*kind) {
  case OpKind::Insert:
    return 'I';
  case OpKind::Delete:
    return 'D';
  case OpKind::Rearrange:
    return 'R';
  case OpKind::Transclude:
    return 'T';
  case OpKind::Link:
    return 'L';
  case OpKind::PageBreak:
    return 'P';
  case OpKind::Structure:
    return 'S';
  default:
    return 'M';
  }
}

void HypertimeGraph::drawDisc(gleditor::Canvas &canvas, const float cX,
                              const float cY, const float radius,
                              const std::uint32_t fillCol,
                              const std::uint32_t borderCol,
                              const float borderWidth,
                              [[maybe_unused]] const std::size_t slices) {
  if (radius <= 0.0F) {
    return;
  }

  // Draw outer border disc if specified
  if (borderWidth > 0.0F && borderCol != 0) {
    const float rOut     = radius;
    const auto nStepsOut = static_cast<int>(std::ceil(rOut));
    for (int yStep = -nStepsOut; yStep <= nStepsOut; ++yStep) {
      const auto y = static_cast<float>(yStep);
      if (std::abs(y) > rOut) {
        continue;
      }
      const float dx = std::sqrt(std::max(0.0F, rOut * rOut - y * y));
      canvas.addRect(cX - dx, cY + y, 2.0F * dx, 1.0F, borderCol);
    }
  }

  // Draw inner fill disc
  const float rIn = (borderWidth > 0.0F && borderCol != 0)
                        ? std::max(0.0F, radius - borderWidth)
                        : radius;
  if (rIn > 0.0F) {
    const auto nStepsIn = static_cast<int>(std::ceil(rIn));
    for (int yStep = -nStepsIn; yStep <= nStepsIn; ++yStep) {
      const auto y = static_cast<float>(yStep);
      if (std::abs(y) > rIn) {
        continue;
      }
      const float dx = std::sqrt(std::max(0.0F, rIn * rIn - y * y));
      canvas.addRect(cX - dx, cY + y, 2.0F * dx, 1.0F, fillCol);
    }
  }
}

void HypertimeGraph::drawPolyline(gleditor::Canvas &canvas, const float fromX,
                                  const float fromY, const float toX,
                                  const float toY, const float thickness,
                                  const std::uint32_t colour) {
  if (std::abs(fromY - toY) < 0.5F || std::abs(fromX - toX) < 0.5F) {
    canvas.addLine(fromX, fromY, toX, toY, thickness, colour);
    return;
  }
  // Orthogonal elbow routing (horizontal -> vertical -> horizontal)
  const float midX = (fromX + toX) * 0.5F;
  canvas.addLine(fromX, fromY, midX, fromY, thickness, colour);
  canvas.addLine(midX, fromY, midX, toY, thickness, colour);
  canvas.addLine(midX, toY, toX, toY, thickness, colour);
}

void HypertimeGraph::computeUnobstructedAliasPosition(
    GraphNode &node, const std::vector<GraphEdge> &allEdges,
    const float panelTop, const float panelBottom) {
  if (node.alias.empty()) {
    return;
  }
  const auto font = gleditor::text::FontManager::instance().getFont(
      metrics_.fontDescription(ui::FontRole::Label, theme_));
  const auto measured = measurements_.fitted(node.alias, font, {});
  const float bw =
      std::min(graphBounds_.width,
               std::max(node.radius * 2,
                        std::min(measured.widthPx + 4, node.radius * 4)));
  const float bh = std::ceil(font->metrics().lineHeight) + 4;

  // Candidate placements around node
  struct Candidate {
    float x;
    float y;
  };
  const std::array<Candidate, 6> candidates = {
      Candidate{.x = node.x - bw * 0.5F,
                .y = node.y + node.radius + 5.0F}, // Above
      Candidate{.x = node.x + node.radius + 6.0F,
                .y = node.y - bh * 0.5F}, // Right
      Candidate{.x = node.x - bw * 0.5F,
                .y = node.y - node.radius - bh - 5.0F}, // Below
      Candidate{.x = node.x + node.radius * 0.7F + 4.0F,
                .y = node.y + node.radius + 2.0F}, // Above-Right
      Candidate{.x = node.x + node.radius * 0.7F + 4.0F,
                .y = node.y - node.radius - bh}, // Below-Right
      Candidate{.x = node.x - node.radius - bw - 6.0F,
                .y = node.y - bh * 0.5F}, // Left
  };

  auto testBoxCollision = [&](const float bx, const float by) -> int {
    int collisions = 0;
    // Boundary check
    if (bx < graphBounds_.left ||
        bx + bw > graphBounds_.left + graphBounds_.width || by < panelBottom ||
        by + bh > panelTop) {
      collisions += 100;
    }

    // Node circles check
    for (const auto &other : nodes_) {
      if (other.id == node.id) {
        continue;
      }
      const float cx = std::clamp(other.x, bx, bx + bw);
      const float cy = std::clamp(other.y, by, by + bh);
      const float dSq =
          (other.x - cx) * (other.x - cx) + (other.y - cy) * (other.y - cy);
      const float minDist = other.radius + 4.0F;
      if (dSq < minDist * minDist) {
        collisions += 10;
      }
    }

    // Other placed alias badges check
    for (const auto &other : nodes_) {
      if (other.id == node.id || other.alias.empty() || other.aliasW <= 0.0F) {
        continue;
      }
      const bool xOverlap = (bx < other.aliasX + other.aliasW + 4.0F) &&
                            (bx + bw + 4.0F > other.aliasX);
      const bool yOverlap = (by < other.aliasY + other.aliasH + 4.0F) &&
                            (by + bh + 4.0F > other.aliasY);
      if (xOverlap && yOverlap) {
        collisions += 50;
      }
    }

    // Edge segments check
    for (const auto &e : allEdges) {
      if (e.fromIdx >= nodes_.size() || e.toIdx >= nodes_.size()) {
        continue;
      }
      const auto &n1 = nodes_[e.fromIdx];
      const auto &n2 = nodes_[e.toIdx];

      auto segIntersectsBox = [&](const float x1, const float y1,
                                  const float x2, const float y2) -> bool {
        if (std::abs(y1 - y2) < 0.5F) {
          return (by <= y1 && y1 <= by + bh) &&
                 (std::max(x1, x2) >= bx && std::min(x1, x2) <= bx + bw);
        }
        if (std::abs(x1 - x2) < 0.5F) {
          return (bx <= x1 && x1 <= bx + bw) &&
                 (std::max(y1, y2) >= by && std::min(y1, y2) <= by + bh);
        }
        const float sMinX = std::min(x1, x2);
        const float sMaxX = std::max(x1, x2);
        const float sMinY = std::min(y1, y2);
        const float sMaxY = std::max(y1, y2);
        return !(sMaxX < bx || sMinX > bx + bw || sMaxY < by ||
                 sMinY > by + bh);
      };

      if (std::abs(n1.y - n2.y) < 0.5F) {
        if (segIntersectsBox(n1.x, n1.y, n2.x, n2.y)) {
          collisions += 5;
        }
      } else {
        const float midX = (n1.x + n2.x) * 0.5F;
        if (segIntersectsBox(n1.x, n1.y, midX, n1.y) ||
            segIntersectsBox(midX, n1.y, midX, n2.y) ||
            segIntersectsBox(midX, n2.y, n2.x, n2.y)) {
          collisions += 5;
        }
      }
    }
    return collisions;
  };

  int bestScore           = 999999;
  Candidate bestCandidate = candidates[0];
  for (const auto &cand : candidates) {
    const int score = testBoxCollision(cand.x, cand.y);
    if (score < bestScore) {
      bestScore     = score;
      bestCandidate = cand;
      if (score == 0) {
        break;
      }
    }
  }

  if (bestScore >= 100) {
    node.aliasW = 0;
    return;
  }
  node.aliasX = bestCandidate.x;
  node.aliasY = bestCandidate.y;
  node.aliasW = bw;
  node.aliasH = bh;
}

void HypertimeGraph::layout(const float screenW, const float screenH) {
  if (builtAt == generation_() && !nodes_.empty() && laidOutWidth_ == screenW &&
      laidOutHeight_ == screenH) {
    return;
  }
  nodes_.clear();
  edges_.clear();
  chronologicalOrder_.clear();
  builtAt        = generation_();
  laidOutWidth_  = screenW;
  laidOutHeight_ = screenH;
  revision_++;

  const auto &st = storeAt_(storeIndex_);
  const auto all = st.allVersions();

  // 1. Explicit Genesis root node (opLetter 'G', depth 0, lane 0)
  GraphNode genesisNode;
  genesisNode.id       = MicroversionId{};
  genesisNode.opLetter = 'G';
  genesisNode.radius   = 13.0F;
  const auto genAnn    = st.versionAnnotation(genesisNode.id);
  if (genAnn.has_value() && !genAnn->alias.empty()) {
    genesisNode.alias = genAnn->alias;
  } else {
    genesisNode.alias = "genesis";
  }
  nodes_.push_back(std::move(genesisNode));
  chronologicalOrder_.emplace_back();

  std::map<MicroversionId, std::size_t> nodeIndices;
  nodeIndices[MicroversionId{}] = 0;

  for (const auto &ver : all) {
    GraphNode node;
    node.id       = ver;
    const auto op = st.getOp(node.id);
    node.opLetter =
        opKindLetter(op.has_value() ? std::optional(op->kind) : std::nullopt);
    const auto ann = st.versionAnnotation(node.id);
    if (ann.has_value() && !ann->alias.empty()) {
      node.alias = ann->alias;
    }
    nodeIndices[node.id] = nodes_.size();
    nodes_.push_back(std::move(node));
    chronologicalOrder_.push_back(ver);
  }

  std::map<MicroversionId, std::vector<std::size_t>> childrenMap;

  // Build edges: versions with parent.isZero() connect from Genesis (index 0).
  for (std::size_t i = 1; i < nodes_.size(); ++i) {
    const auto &parent = nodes_[i].id.parent();
    std::size_t pIdx   = 0U;
    if (!parent.isZero() && nodeIndices.contains(parent)) {
      pIdx = nodeIndices[parent];
    }
    childrenMap[nodes_[pIdx].id].push_back(i);

    GraphEdge edge;
    edge.fromIdx     = pIdx;
    edge.toIdx       = i;
    const auto &segs = nodes_[i].id.segments();
    if (pIdx == 0) {
      edge.isBranch =
          !segs.empty() && segs[0].branch != MicroversionId::noBranch;
    } else {
      edge.isBranch = segs.size() > 1 && segs.back().number == 1;
    }
    edges_.push_back(edge);
  }

  // Genesis is depth 0, lane 0.
  nodes_[0].depth      = 0;
  nodes_[0].lane       = 0;
  std::size_t maxDepth = 0;
  std::size_t nextLane = 1;

  // Propagate depth and lanes topologically through childrenMap starting from
  // Genesis
  const auto &genChildren = childrenMap[MicroversionId{}];
  for (std::size_t c = 0; c < genChildren.size(); ++c) {
    const auto childIdx    = genChildren[c];
    nodes_[childIdx].depth = 1;
    maxDepth               = std::max(maxDepth, nodes_[childIdx].depth);
    if (c == 0) {
      nodes_[childIdx].lane = 0; // Main trunk continues in lane 0
    } else {
      nodes_[childIdx].lane = nextLane++;
    }
  }

  for (std::size_t i = 1; i < nodes_.size(); ++i) {
    const auto &curId    = nodes_[i].id;
    const auto &children = childrenMap[curId];
    for (std::size_t c = 0; c < children.size(); ++c) {
      const auto childIdx    = children[c];
      nodes_[childIdx].depth = nodes_[i].depth + 1;
      maxDepth               = std::max(maxDepth, nodes_[childIdx].depth);
      if (c == 0) {
        nodes_[childIdx].lane = nodes_[i].lane;
      } else {
        nodes_[childIdx].lane = nextLane++;
      }
    }
  }
  const std::size_t maxLanes = std::max<std::size_t>(1, nextLane);

  const float graphAreaW = graphBounds_.width;
  const float graphAreaH = graphBounds_.height;
  const auto font        = gleditor::text::FontManager::instance().getFont(
      metrics_.fontDescription(gleditor::ui::FontRole::Label, theme_));
  const float diameter = std::ceil(std::max(metrics_.px(theme_.type.minTouchPx),
                                            font->metrics().lineHeight + 4));
  const float dx       = std::max(
      diameter * 2.2F, maxDepth ? graphAreaW / static_cast<float>(maxDepth + 1)
                                : diameter * 2.2F);
  const float dy =
      std::max(diameter * 1.4F, graphAreaH / static_cast<float>(maxLanes + 1));
  const float originX = graphBounds_.left + diameter;
  const float originY = graphBounds_.bottom + graphAreaH - diameter;
  panX_ =
      std::clamp(panX_,
                 std::min(0.F, graphAreaW - diameter * 2 -
                                   static_cast<float>(maxDepth) * dx * zoom_),
                 0.F);
  panY_ =
      std::clamp(panY_, 0.F,
                 std::max(0.F, static_cast<float>(maxLanes - 1) * dy * zoom_ -
                                   graphAreaH + diameter * 2));
  for (auto &node : nodes_) {
    node.x      = originX + static_cast<float>(node.depth) * dx * zoom_ + panX_;
    node.y      = originY - static_cast<float>(node.lane) * dy * zoom_ + panY_;
    node.radius = diameter * .5F;
  }

  for (auto &node : nodes_)
    computeUnobstructedAliasPosition(node, edges_,
                                     graphBounds_.bottom + graphBounds_.height,
                                     graphBounds_.bottom);

  for (auto &edge : edges_) {
    if (edge.toIdx < nodes_.size()) {
      edge.lane = nodes_[edge.toIdx].lane;
    }
  }
}

void HypertimeGraph::changed(bool retire) {
  dirty_ = decorationDirty_ = true;
  ++revision_;
  if (retire) {
    ++epoch_;
    actions_.clear();
    nodeIds_.clear();
    aliasIds_.clear();
    controlIds_.clear();
    overlay_.setVisible(false);
  }
}
HypertimeGraph *HypertimeGraph::setVisible(bool show) {
  const std::scoped_lock lock(guard_);
  if (visible_ == show) return this;
  visible_ = show;
  changed(true);
  if (show)
    activate();
  else
    deactivate();
  return this;
}
HypertimeGraph *HypertimeGraph::setCurrent(const MicroversionId &id) {
  const std::scoped_lock lock(guard_);
  if (current_ != id) {
    current_ = id;
    changed();
  }
  return this;
}
HypertimeGraph *HypertimeGraph::invalidate() {
  const std::scoped_lock lock(guard_);
  nodes_.clear();
  diffNeedsUpdate_ = true;
  changed(true);
  return this;
}
HypertimeGraph *HypertimeGraph::setStoreIndex(std::size_t index) {
  const std::scoped_lock lock(guard_);
  if (index == storeIndex_) return this;
  storeIndex_ = index;
  nodes_.clear();
  comparedVersions_.clear();
  selectedOperation_.reset();
  diffNeedsUpdate_ = true;
  changed(true);
  return this;
}
HypertimeGraph *
HypertimeGraph::setConfig(const ModalPresentationConfig &config) {
  const std::scoped_lock lock(guard_);
  if (config_ != config) {
    config_ = config;
    changed();
  }
  return this;
}
HypertimeGraph *HypertimeGraph::toggleComparison(const MicroversionId &id) {
  const std::scoped_lock lock(guard_);
  const auto found = std::ranges::find(comparedVersions_, id);
  if (found == comparedVersions_.end())
    comparedVersions_.push_back(id);
  else
    comparedVersions_.erase(found);
  diffNeedsUpdate_ = true;
  changed(true);
  return this;
}
HypertimeGraph *HypertimeGraph::clearComparison() {
  const std::scoped_lock lock(guard_);
  comparedVersions_.clear();
  comparisonPage_  = false;
  diffNeedsUpdate_ = true;
  changed(true);
  return this;
}

std::shared_ptr<const ui::WidgetScene>
HypertimeGraph::prepare(const ui::UiMetrics &metrics, const ui::Theme &source) {
  const std::scoped_lock lock(guard_);
  if (!visible_) return {};
  const auto generation = generation_();
  if (!dirty_ && builtAt == generation && metrics_ == metrics &&
      sourceTheme_ == source)
    return overlay_.snapshot();
  if (modelGeneration_ != generation && !nodes_.empty()) changed(true);
  modelGeneration_ = generation;
  metrics_         = metrics;
  sourceTheme_     = source;
  theme_ = ui::withFontOverride(source, ui::FontRole::Label, fontName_);
  // Graph coordinates carry their own spacing; nested chrome has no padding.
  theme_.paddingEm = 0;
  theme_.gapEm     = 0;
  const auto font  = gleditor::text::FontManager::instance().getFont(
      metrics.fontDescription(ui::FontRole::Label, theme_));
  const float line = std::ceil(font->metrics().lineHeight) + 2;
  const float touch =
      std::ceil(std::max(metrics.px(theme_.type.minTouchPx), line + 4));
  const auto safe    = metrics.pixelSafeArea();
  const auto bounded = [](float value, float fallback, float minimum,
                          float maximum) {
    return std::isfinite(value) ? std::clamp(value, minimum, maximum)
                                : fallback;
  };
  panelW_ = std::floor(
      std::min(metrics.px(bounded(config_.widthPx, 640, 1, 10000)),
               safe.width * bounded(config_.maxWidthShare, .95F, .1F, 1)));
  panelH_ = std::floor(
      std::min(std::max(metrics.px(bounded(config_.heightPx, 460, 1, 10000)),
                        line + touch * 4),
               safe.height * bounded(config_.maxHeightShare, .95F, .1F, 1)));
  panelX_ = std::round(safe.left);
  panelY_ = std::round(safe.bottom + (safe.height - panelH_) * .5F);

  graphBounds_ = {panelX_ + 2, panelY_ + touch + 4, std::max(0.F, panelW_ - 4),
                  std::max(0.F, panelH_ - line - touch * 2 - 10)};

  // Every rebuild can change typography or positioning; warm frames exit above.
  builtAt = std::numeric_limits<std::uint64_t>::max();
  layout(static_cast<float>(metrics.screenWidth),
         static_cast<float>(metrics.screenHeight));
  ui::Widget model{
      .id        = 1,
      .model     = ui::PositionedPanel{},
      .preferred = {metrics.logical(panelW_), metrics.logical(panelH_)}};
  auto add = [&](ui::Widget widget, ui::Rect bounds) {
    auto &positioned = std::get<ui::PositionedPanel>(model.model);
    positioned.childBounds.push_back({metrics.logical(bounds.left - panelX_),
                                      metrics.logical(bounds.bottom - panelY_),
                                      metrics.logical(bounds.width),
                                      metrics.logical(bounds.height)});
    widget.preferred = {metrics.logical(bounds.width),
                        metrics.logical(bounds.height)};
    model.children.push_back(std::move(widget));
  };
  actions_.clear();
  auto actionButton = [&](std::string label, std::string kind, ui::Rect rect,
                          std::optional<MicroversionId> version = {}) {
    const auto key           = kind + (version ? version->str() : "");
    auto [identity, created] = controlIds_.try_emplace(key, nextId_);
    if (created) ++nextId_;
    const auto id = identity->second;
    actions_.emplace(id, Action{kind, version, comparedVersions_, epoch_});
    add({.id = id, .model = ui::Button{std::move(label), kind}, .maxLines = 1},
        rect);
  };
  add({.id       = 2,
       .model    = ui::Label{"Hypertime Branching DAG & N-Way Diff"},
       .maxLines = 1},
      {panelX_ + 2, panelY_ + panelH_ - line - 2, panelW_ - 4, line});
  const float headerY = panelY_ + panelH_ - line - touch - 4;
  const std::size_t headerCount =
      3 + (selectedOperation_ ? 1 : 0) +
      (selectedOperation_ && annotateHandler_ ? 1 : 0);
  const float buttonWidth =
      std::floor((panelW_ - 4) / static_cast<float>(headerCount));
  std::size_t column = 0;
  auto header        = [&](std::string label, std::string kind,
                           std::optional<MicroversionId> version = {}) {
    actionButton(std::move(label), std::move(kind),
                 {panelX_ + 2 + static_cast<float>(column++) * buttonWidth,
                  headerY, buttonWidth, touch},
                 version);
  };
  header("Close hypertime", "close");
  header("Graph", "graph");
  header("Comparison", "comparison");
  if (selectedOperation_) {
    const bool included =
        std::ranges::find(comparedVersions_, *selectedOperation_) !=
        comparedVersions_.end();
    header((included ? "Remove " : "Add ") + selectedOperation_->str() +
               (included ? " from comparison" : " to comparison"),
           "toggle", selectedOperation_);
    if (annotateHandler_)
      header("Annotate operation " + selectedOperation_->str() +
                 " and place handle on d.1",
             "annotate", selectedOperation_);
  }
  if (comparisonPage_) {
    if (diffNeedsUpdate_ && comparedVersions_.size() >= 2) {
      diffResult_      = storeAt_(storeIndex_).diffVersions(comparedVersions_);
      diffNeedsUpdate_ = false;
    }
    ui::List rows;
    rowHeight_       = line + 4;
    rows.rowHeightPx = rowHeight_;

    auto row = [&](std::string label) {
      rows.rows.push_back({nextId_++, std::move(label), {}, false});
    };
    row("Comparative Diff (" + std::to_string(comparedVersions_.size()) +
        " versions)");
    if (comparedVersions_.size() < 2)
      row("Select versions, then add them to comparison.");
    else {
      if (comparedVersions_.size() == 3) {
        auto ancestor = comparedVersions_[1];
        while (ancestor != comparedVersions_[2] &&
               !ancestor.isAncestorOf(comparedVersions_[2]) &&
               !ancestor.isZero())
          ancestor = ancestor.parent();
        const bool before = comparedVersions_[0] != ancestor &&
                            comparedVersions_[0].isAncestorOf(ancestor);
        row("Ancestor " + ancestor.str() +
            (before ? " | base before" : " | check base"));
      }
      for (const auto &version : diffResult_.versions) {
        row("Version " + version.version.str() + ": " +
            std::to_string(version.uniqueChars) + " unique characters, " +
            std::to_string(version.changedCells.size()) + " changed cells");
        row("Universal (Gold): " + std::to_string(version.universalChars) +
            " chars | " + passagePreview(version, DiffKind::Universal));
        row("Shared (Amber): " + std::to_string(version.sharedChars) +
            " chars | " + passagePreview(version, DiffKind::Shared));
        row("Unique (Mint): " + std::to_string(version.uniqueChars) +
            " chars | " + passagePreview(version, DiffKind::Unique));
        row("Limbo (Crimson): " + std::to_string(version.deletedChars) +
            " chars");
      }
    }
    comparisonMaxScroll_ =
        std::max(0.F, static_cast<float>(rows.rows.size()) * rowHeight_ -
                          std::max(0.F, graphBounds_.height - touch - 2));
    comparisonScroll_ =
        std::clamp(comparisonScroll_, 0.F, comparisonMaxScroll_);
    rows.scrollPx = comparisonScroll_;
    add({.id = 3, .model = std::move(rows), .maxLines = 1},
        {graphBounds_.left, graphBounds_.bottom + touch + 2, graphBounds_.width,
         std::max(0.F, graphBounds_.height - touch - 2)});
    const float width = std::floor(graphBounds_.width / 4);
    const std::array<std::pair<const char *, const char *>, 4> buttons{
        {{"Quote into Head", "quote"},
         {"Open in 3D", "open3d"},
         {"Onion Skin", "onion"},
         {"Clear comparison", "clear"}}};
    for (std::size_t i = 0; i < buttons.size(); ++i)
      actionButton(buttons[i].first, buttons[i].second,
                   {graphBounds_.left + static_cast<float>(i) * width,
                    graphBounds_.bottom, width, touch});
  } else {
    for (const auto &node : nodes_) {
      const ui::Rect bounds{node.x - node.radius, node.y - node.radius,
                            node.radius * 2, node.radius * 2};
      if (bounds.left < graphBounds_.left ||
          bounds.bottom < graphBounds_.bottom ||
          bounds.left + bounds.width > graphBounds_.left + graphBounds_.width ||
          bounds.bottom + bounds.height >
              graphBounds_.bottom + graphBounds_.height)
        continue;
      auto [entry, created] = nodeIds_.try_emplace(node.id, nextId_);
      if (created) ++nextId_;
      const auto label = std::string(1, node.opLetter) + " · Version " +
                         node.id.str() +
                         (node.alias.empty() ? "" : " (" + node.alias + ")");
      actions_.emplace(entry->second, Action{"node", node.id, {}, epoch_});
      add({.id = entry->second,
           .model =
               ui::Button{std::string(1, node.opLetter), "node", true, label},
           .maxLines = 1},
          bounds);
      if (!node.alias.empty() && node.aliasW > 0) {
        auto [alias, newAlias] = aliasIds_.try_emplace(node.id, nextId_);
        if (newAlias) ++nextId_;
        actions_.emplace(alias->second, Action{"node", node.id, {}, epoch_});
        add({.id       = alias->second,
             .model    = ui::Button{node.alias, "node", true,
                                    "Alias " + node.alias + " for version " +
                                        node.id.str()},
             .maxLines = 1},
            {node.aliasX, node.aliasY, node.aliasW, node.aliasH});
      }
    }
  }
  const auto current = std::ranges::find(chronologicalOrder_, current_);
  const auto index = current == chronologicalOrder_.end()
                         ? 0
                         : std::distance(chronologicalOrder_.begin(), current);
  auto [time, createdTime] = controlIds_.try_emplace("time", nextId_);
  if (createdTime) ++nextId_;
  const auto timeId = time->second;
  actions_.emplace(timeId, Action{"time", {}, chronologicalOrder_, epoch_});
  add({.id = timeId,
       .model =
           ui::Scrubber{"Time: " + current_.str() + " (" +
                            std::to_string(index + 1) + "/" +
                            std::to_string(chronologicalOrder_.size()) + ")",
                        "time", static_cast<double>(index), 0,
                        static_cast<double>(chronologicalOrder_.size() - 1),
                        1.0 / static_cast<double>(std::max<std::size_t>(
                                  1, chronologicalOrder_.size() - 1))},
       .maxLines = 1},
      {panelX_ + 2, panelY_ + 2, panelW_ - 4, touch});
  overlay_.setModel(std::move(model));
  overlay_.setBounds(ui::Rect{panelX_, panelY_, panelW_, panelH_});
  overlay_.setVisible(true);
  dirty_           = false;
  decorationDirty_ = true;
  return overlay_.prepare(metrics_, theme_);
}
void HypertimeGraph::queue(const ui::WidgetAction &widget) {
  const std::scoped_lock lock(guard_);
  if (!visible_ || dirty_ || modelGeneration_ != generation_()) return;
  const auto found = actions_.find(widget.id);
  if (found == actions_.end()) return;
  auto action = found->second;
  if (action.kind == "node") {
    const auto mods = SDL_GetModState();
    action.modified = (mods & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL)) != 0;
  }
  if (action.kind == "time") {
    try {
      const auto value = std::stod(widget.value);
      if (!std::isfinite(value) || action.compared.empty()) return;
      const auto index = static_cast<std::size_t>(std::clamp(
          value, 0., static_cast<double>(action.compared.size() - 1)));
      action.version   = action.compared[index];
    } catch (const std::exception &) {
      return;
    }
  }
  pending_.push_back(std::move(action));
}
void HypertimeGraph::drain() {
  auto pending = std::move(pending_);
  pending_.clear();
  for (const auto &action : pending)
    if (visible_ && action.epoch == epoch_ && modelGeneration_ == generation_())
      dispatch(action);
}
void HypertimeGraph::dispatch(const Action &action) {
  const auto &kind = action.kind;
  if (kind == "close") {
    setVisible(false);
    return;
  }
  if (kind == "graph" || kind == "comparison") {
    comparisonPage_ = kind == "comparison";
    changed(true);
  } else if (kind == "toggle" && action.version)
    toggleComparison(*action.version);
  else if (kind == "clear")
    clearComparison();
  else if (kind == "annotate" && action.version && annotateHandler_)
    annotateHandler_(*action.version);
  else if (kind == "open3d" && compareHandler_ && !action.compared.empty())
    compareHandler_(action.compared);
  else if (kind == "onion" && onionSkinHandler_ && !action.compared.empty())
    onionSkinHandler_(action.compared);
  else if (kind == "quote" && quoteHandler_ && !action.compared.empty()) {
    const auto source =
        action.compared.size() > 1 && action.compared[0] == current_
            ? action.compared[1]
            : action.compared[0];
    const auto text = storeAt_(storeIndex_).textOf(source);
    if (!text.empty())
      quoteHandler_(source, 0, static_cast<std::uint32_t>(text.size()));
  } else if (kind == "node" && action.version) {
    if (action.modified)
      toggleComparison(*action.version);
    else {
      if (!action.version->isZero()) selectedOperation_ = action.version;
      current_ = *action.version;
      changed();
      if (goer_) goer_(current_);
    }
  } else if (kind == "time" && action.version) {
    current_ = *action.version;
    changed();
    if (scrubHandler_)
      scrubHandler_(current_);
    else if (goer_)
      goer_(current_);
  }
}
void HypertimeGraph::rebuildDecoration() {
  if (!canvas_) return;
  canvas_->clear();
  canvas_->setTag(render::tagKindOverlay, 0);
  canvas_->addRect(panelX_, panelY_, panelW_, panelH_,
                   ui::rgba(theme_.colours.surface));
  if (!comparisonPage_) {
    const auto inside = [&](const GraphNode &node) {
      return node.x - node.radius >= graphBounds_.left &&
             node.x + node.radius <= graphBounds_.left + graphBounds_.width &&
             node.y - node.radius >= graphBounds_.bottom &&
             node.y + node.radius <= graphBounds_.bottom + graphBounds_.height;
    };
    for (const auto &edge : edges_) {
      if (edge.fromIdx >= nodes_.size() || edge.toIdx >= nodes_.size())
        continue;
      const auto &from = nodes_[edge.fromIdx];
      const auto &to   = nodes_[edge.toIdx];
      if (inside(from) && inside(to))
        drawPolyline(*canvas_, from.x, from.y, to.x, to.y, metrics_.px(2),
                     kLineageHues[edge.lane % kLineageHues.size()]);
    }
    for (const auto &node : nodes_) {
      if (!inside(node)) continue;
      const bool selected = node.id == current_;
      const bool compared = std::ranges::find(comparedVersions_, node.id) !=
                            comparedVersions_.end();
      if (selected || compared)
        drawDisc(*canvas_, node.x, node.y, node.radius + 2,
                 selected ? 0x10B981FF : 0xFFD700FF);
    }
  }
  canvas_->commit();
  decorationDirty_ = false;
}
void HypertimeGraph::drawFrame(gleditor::FrameContext &ctx) {
  const std::scoped_lock lock(guard_);
  drain();
  if (!visible_ || !canvas_) return;
  auto metrics         = ctx.metrics;
  metrics.screenWidth  = ctx.screenWidth;
  metrics.screenHeight = ctx.screenHeight;
  metrics.chrome       = ctx.chrome;
  std::ignore          = prepare(metrics, ctx.theme);
  if (decorationDirty_) rebuildDecoration();
  const auto ortho =
      glm::ortho(0.F, static_cast<float>(ctx.screenWidth), 0.F,
                 static_cast<float>(ctx.screenHeight), -1.F, 1.F);
  canvas_->draw(ctx.state, ortho);
  gleditor::FrameContext draw{
      ctx.state,    ctx.viewProjection, ctx.screenWidth,   ctx.screenHeight,
      ctx.timeline, ctx.chrome,         ctx.settledChrome, metrics,
      theme_};
  overlay_.drawFrame(draw);
}
bool HypertimeGraph::busy() const {
  const std::scoped_lock lock(guard_);
  return !pending_.empty();
}
bool HypertimeGraph::picked(const render::PickingResult &pick,
                            RenderState &state) {
  const std::scoped_lock lock(guard_);
  if (!visible_) return false;
  if (pick.requestId || pick.tag.docIndex || pick.tag.pageIndex) {
    if (dirty_ || modelGeneration_ != generation_())
      return pick.tag.kind == render::tagKindOverlay;
    const auto handled = overlay_.picked(pick, state);
    if (handled && pick.overlayWidgetId) requestFocus(*pick.overlayWidgetId);
    return handled;
  }
  // Existing synthetic domain tests use the historical tags. Real picks resolve
  // the retained scene's immutable identity instead of these array indices.
  if (pick.tag.kind != render::tagKindOverlay) return false;
  const auto tag = pick.tag.clusterIndex;
  Action action{.epoch = epoch_};
  action.compared = comparedVersions_;
  if (tag >= kTagNodeBase && tag - kTagNodeBase < nodes_.size()) {
    action.kind    = "node";
    action.version = nodes_[tag - kTagNodeBase].id;
    action.modified =
        (SDL_GetModState() & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL)) != 0;
  } else if (tag == kTagCompareSelection) {
    action.kind    = "toggle";
    action.version = selectedOperation_;
  } else if (tag == kTagAnnotateButton) {
    action.kind    = "annotate";
    action.version = selectedOperation_;
  } else if (tag == kTagQuoteButton)
    action.kind = "quote";
  else if (tag == kTagOpen3DButton)
    action.kind = "open3d";
  else if (tag == kTagOnionSkinButton)
    action.kind = "onion";
  else if (tag == kTagClearComp)
    action.kind = "clear";
  else if (tag == kTagScrubberTrack || tag == kTagScrubberThumb) {
    if (chronologicalOrder_.empty() || panelW_ <= 0) return false;
    action.kind = "time";
    const auto fraction =
        std::clamp((static_cast<float>(pick.x) - panelX_) / panelW_, 0.F, 1.F);
    const auto index = std::min(
        chronologicalOrder_.size() - 1,
        static_cast<std::size_t>(
            fraction * static_cast<float>(chronologicalOrder_.size())));
    action.version = chronologicalOrder_[index];
  } else
    return false;
  dispatch(action);
  return true;
}
void HypertimeGraph::describe(gleditor::a11y::Builder &builder) {
  const std::scoped_lock lock(guard_);
  if (!visible_ || dirty_) return;
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder presentation(tree, builder.owner());
  overlay_.describe(presentation);
  for (auto &node : tree.nodes) {
    const auto local  = static_cast<ui::WidgetId>(node.id);
    const auto action = actions_.find(local);
    if (action != actions_.end() && action->second.kind == "node" &&
        action->second.version) {
      const auto &version = *action->second.version;
      auto state          = [&](std::string_view value) {
        if (!node.value.empty()) node.value += ", ";
        node.value += value;
      };
      if (version == current_) state("current view");
      if (selectedOperation_ == version) state("selected operation");
      if (std::ranges::find(comparedVersions_, version) !=
          comparedVersions_.end())
        state("in comparison");
    }
    builder.add(local, node.role) = std::move(node);
  }
  for (const auto root : presentation.roots()) builder.contribute(root);
  if (tree.focus) builder.takeFocus(tree.focus);
}
bool HypertimeGraph::performAction(std::uint64_t id,
                                   gleditor::a11y::Action action,
                                   std::string_view value) {
  const std::scoped_lock lock(guard_);
  return visible_ && !dirty_ && modelGeneration_ == generation_() &&
         overlay_.performAction(id, action, value);
}
std::shared_ptr<const ui::LayoutResult> HypertimeGraph::focusLayout() const {
  const std::scoped_lock lock(guard_);
  // A pending geometry refresh keeps the previous immutable focus identity.
  // Activation remains guarded until the replacement scene is published.
  return visible_ ? overlay_.focusLayout() : nullptr;
}
void HypertimeGraph::focusedNodeChanged(std::uint32_t id) {
  overlay_.focusedNodeChanged(id);
}
bool HypertimeGraph::activateNode(std::uint32_t id) {
  const std::scoped_lock lock(guard_);
  return visible_ && !dirty_ && modelGeneration_ == generation_() &&
         overlay_.activateNode(id);
}
void HypertimeGraph::focusChanged(bool focused) {
  overlay_.focusChanged(focused);
}
std::optional<gleditor::InputArea> HypertimeGraph::pointerArea() const {
  const std::scoped_lock lock(guard_);
  return visible_ && !dirty_ ? overlay_.pointerArea() : std::nullopt;
}
bool HypertimeGraph::keyPressed(gleditor::Key key, gleditor::KeyMods mods) {
  const std::scoped_lock lock(guard_);
  if (!visible_) return false;
  if (key == gleditor::Key::Escape) {
    setVisible(false);
    return true;
  }
  if (comparisonPage_ &&
      (key == gleditor::Key::PageUp || key == gleditor::Key::PageDown)) {
    const float direction = key == gleditor::Key::PageDown ? 1.F : -1.F;
    comparisonScroll_ =
        std::clamp(comparisonScroll_ + direction * graphBounds_.height, 0.F,
                   comparisonMaxScroll_);
    changed();
    return true;
  }
  return !dirty_ && overlay_.keyPressed({key, mods});
}
bool HypertimeGraph::pointerEvent(const ui::PointerEvent &event) {
  const std::scoped_lock lock(guard_);
  if (!visible_ || dirty_) return false;
  if (event.phase == ui::PointerPhase::Wheel) {
    if (comparisonPage_) {
      comparisonScroll_ =
          std::clamp(comparisonScroll_ - event.deltaY * rowHeight_, 0.F,
                     comparisonMaxScroll_);
      changed();
    } else
      scroll(event.deltaX, event.deltaY,
             (SDL_GetModState() & SDL_KMOD_CTRL) != 0,
             (SDL_GetModState() & SDL_KMOD_SHIFT) != 0, event.x,
             static_cast<float>(metrics_.screenHeight) - event.y);
    return true;
  }
  return overlay_.pointerEvent(event);
}
} // namespace xanadu::ui
