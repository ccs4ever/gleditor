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

#include "session.hpp"

namespace xudu {

namespace {

constexpr std::array<std::uint32_t, 6> kLineageHues = {
    0x00E5FFFF, // Cyan
    0xFFB74DFF, // Amber
    0xBA68C8FF, // Purple
    0x4CAF50FF, // Emerald
    0xFF80ABFF, // Pink
    0x42A5F5FF, // Blue
};

} // namespace

HypertimeGraph::HypertimeGraph(std::string aFontName, const Session &aSession)
    : fontName_(std::move(aFontName)), session_(aSession) {}

HypertimeGraph::~HypertimeGraph() = default;

void HypertimeGraph::deviceReady(render::RenderDevice &device,
                                 const render::PipelineDesc &documentPipeline) {
  canvas_ = std::make_unique<gleditor::Canvas>(&device, fontName_);
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
      const float y = static_cast<float>(yStep);
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
      const float y = static_cast<float>(yStep);
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
  if (node.alias.empty() || nullptr == canvas_) {
    return;
  }
  const auto metrics = canvas_->measureText(node.alias);
  const float bw     = std::max(28.0F, metrics.width + 12.0F);
  const float bh     = std::max(18.0F, metrics.height + 6.0F);

  // Candidate placements around node
  struct Candidate {
    float x;
    float y;
  };
  const std::array<Candidate, 6> candidates = {
      Candidate{node.x - bw * 0.5F, node.y + node.radius + 5.0F},      // Above
      Candidate{node.x + node.radius + 6.0F, node.y - bh * 0.5F},      // Right
      Candidate{node.x - bw * 0.5F, node.y - node.radius - bh - 5.0F}, // Below
      Candidate{node.x + node.radius * 0.7F + 4.0F,
                node.y + node.radius + 2.0F}, // Above-Right
      Candidate{node.x + node.radius * 0.7F + 4.0F,
                node.y - node.radius - bh}, // Below-Right
      Candidate{node.x - node.radius - bw - 6.0F, node.y - bh * 0.5F}, // Left
  };

  auto testBoxCollision = [&](const float bx, const float by) -> int {
    int collisions = 0;
    // Boundary check
    if (bx < panelX_ + 8.0F || bx + bw > panelX_ + panelW_ - 8.0F ||
        by < panelBottom + 32.0F || by + bh > panelTop - 36.0F) {
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
        if (sMaxX < bx || sMinX > bx + bw || sMaxY < by || sMinY > by + bh) {
          return false;
        }
        return true;
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

  node.aliasX = bestCandidate.x;
  node.aliasY = bestCandidate.y;
  node.aliasW = bw;
  node.aliasH = bh;
}

void HypertimeGraph::layout(RenderState &, const float screenW,
                            const float screenH) {
  if (builtAt == session_.generation() && !nodes_.empty()) {
    return;
  }
  nodes_.clear();
  edges_.clear();
  chronologicalOrder_.clear();
  builtAt = session_.generation();

  const auto &st = session_.store(0);
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
  chronologicalOrder_.push_back(MicroversionId{});

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
    const auto pIdx =
        parent.isZero()
            ? 0U
            : (nodeIndices.contains(parent) ? nodeIndices[parent] : 0U);
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

  panelW_ = std::min(screenW - 32.0F, 640.0F);
  panelH_ = std::min(screenH - 80.0F, 460.0F);
  panelX_ = 16.0F;
  panelY_ = 50.0F;

  const float topY       = panelY_ + panelH_;
  const float graphAreaW = panelW_ - 80.0F;
  const float graphAreaH = panelH_ - 140.0F;

  const float dx =
      maxDepth > 0
          ? std::clamp(graphAreaW / static_cast<float>(maxDepth), 36.0F, 65.0F)
          : 50.0F;
  const float dy =
      maxLanes > 1
          ? std::clamp(graphAreaH / static_cast<float>(maxLanes), 32.0F, 55.0F)
          : 45.0F;

  for (auto &node : nodes_) {
    node.x      = panelX_ + 36.0F + static_cast<float>(node.depth) * dx;
    node.y      = (topY - 85.0F) - static_cast<float>(node.lane) * dy;
    node.radius = 13.0F;
  }

  for (auto &edge : edges_) {
    if (edge.toIdx < nodes_.size()) {
      edge.lane = nodes_[edge.toIdx].lane;
    }
  }

  for (auto &node : nodes_) {
    if (!node.alias.empty()) {
      computeUnobstructedAliasPosition(node, edges_, topY, panelY_);
    }
  }

  scrubberTrackX_ = panelX_ + 20.0F;
  scrubberTrackY_ = panelY_ + 26.0F;
  scrubberTrackW_ = panelW_ - 40.0F;
}

void HypertimeGraph::drawFrame(gleditor::FrameContext &ctx) {
  if (!visible_ || nullptr == canvas_) {
    return;
  }
  layout(ctx.state, static_cast<float>(ctx.screenWidth),
         static_cast<float>(ctx.screenHeight));
  if (nodes_.empty()) {
    return;
  }

  const auto width  = static_cast<float>(ctx.screenWidth);
  const auto height = static_cast<float>(ctx.screenHeight);
  const auto ortho  = glm::ortho(0.0F, width, 0.0F, height, -1.0F, 1.0F);

  canvas_->clear();

  // 1. Panel backdrop
  canvas_->setTag(render::tagKindOverlay, 0);
  canvas_->addRect(panelX_, panelY_, panelW_, panelH_, 0x0F172AE6);
  canvas_->addLine(panelX_, panelY_, panelX_ + panelW_, panelY_, 1.0F,
                   0x334155FF);
  canvas_->addLine(panelX_ + panelW_, panelY_, panelX_ + panelW_,
                   panelY_ + panelH_, 1.0F, 0x334155FF);
  canvas_->addLine(panelX_ + panelW_, panelY_ + panelH_, panelX_,
                   panelY_ + panelH_, 1.0F, 0x334155FF);
  canvas_->addLine(panelX_, panelY_ + panelH_, panelX_, panelY_, 1.0F,
                   0x334155FF);

  const float topY = panelY_ + panelH_;
  canvas_->addText(ctx.state, panelX_ + 14.0F, topY - 24.0F,
                   "HYPERTIME BRANCHING DAG & N-WAY DIFF", 0x38BDF8FF, 0);
  canvas_->addText(ctx.state, panelX_ + 14.0F, topY - 40.0F,
                   "Click: jump | Shift+Click: compare | Scrub bar below",
                   0x94A3B8FF, 0);

  // 2. Directed branch edges
  for (const auto &edge : edges_) {
    if (edge.fromIdx < nodes_.size() && edge.toIdx < nodes_.size()) {
      const auto &n1     = nodes_[edge.fromIdx];
      const auto &n2     = nodes_[edge.toIdx];
      const auto edgeCol = kLineageHues[edge.lane % kLineageHues.size()];
      drawPolyline(*canvas_, n1.x, n1.y, n2.x, n2.y, 2.5F, edgeCol);
    }
  }

  // 3. Circular nodes
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    const auto &n    = nodes_[i];
    const bool isCur = (n.id == current_);
    const bool isComp =
        std::find(comparedVersions_.begin(), comparedVersions_.end(), n.id) !=
        comparedVersions_.end();

    canvas_->setTag(render::tagKindOverlay,
                    kTagNodeBase + static_cast<std::uint32_t>(i));

    if (isCur) {
      drawDisc(*canvas_, n.x, n.y, n.radius + 3.5F, 0x10B98144, 0x10B981FF,
               1.5F);
    }
    if (isComp) {
      drawDisc(*canvas_, n.x, n.y, n.radius + (isCur ? 6.0F : 3.5F), 0xFFD70044,
               0xFFD700FF, 1.5F);
    }

    const std::uint32_t fillCol =
        isCur ? 0x064E3BFF : (isComp ? 0x78350FFF : 0x1E293BFF);
    const std::uint32_t borderCol =
        isCur ? 0x10B981FF : (isComp ? 0xF59E0BFF : 0x64748BFF);
    drawDisc(*canvas_, n.x, n.y, n.radius, fillCol, borderCol, 1.5F);

    const std::string letterStr(1, n.opLetter);
    const auto m = canvas_->measureText(letterStr);
    const std::uint32_t letterCol =
        isCur ? 0x6EE7B7FF : (isComp ? 0xFDE68AFF : 0xF1F5F9FF);
    canvas_->addText(ctx.state, n.x - m.width * 0.5F,
                     n.y + m.height * 0.5F - 2.0F, letterStr, letterCol, 0);

    // Unobstructed alias badge
    if (!n.alias.empty()) {
      canvas_->setTag(render::tagKindOverlay,
                      kTagNodeBase + static_cast<std::uint32_t>(i));
      canvas_->addRect(n.aliasX, n.aliasY, n.aliasW, n.aliasH, 0x0F172AEE);
      canvas_->addLine(n.aliasX, n.aliasY, n.aliasX + n.aliasW, n.aliasY, 1.0F,
                       0x475569FF);
      canvas_->addLine(n.aliasX + n.aliasW, n.aliasY, n.aliasX + n.aliasW,
                       n.aliasY + n.aliasH, 1.0F, 0x475569FF);
      canvas_->addLine(n.aliasX + n.aliasW, n.aliasY + n.aliasH, n.aliasX,
                       n.aliasY + n.aliasH, 1.0F, 0x475569FF);
      canvas_->addLine(n.aliasX, n.aliasY + n.aliasH, n.aliasX, n.aliasY, 1.0F,
                       0x475569FF);

      canvas_->addText(ctx.state, n.aliasX + 5.0F, n.aliasY + n.aliasH - 4.0F,
                       n.alias, 0xFCD34DFF, 0);
    }
  }

  // 4. Continuous time scrubber
  canvas_->setTag(render::tagKindOverlay, kTagScrubberTrack);
  constexpr float trackH = 6.0F;
  canvas_->addRect(scrubberTrackX_, scrubberTrackY_, scrubberTrackW_, trackH,
                   0x1E293BFF);
  canvas_->addLine(scrubberTrackX_, scrubberTrackY_,
                   scrubberTrackX_ + scrubberTrackW_, scrubberTrackY_, 1.0F,
                   0x334155FF);
  canvas_->addLine(scrubberTrackX_, scrubberTrackY_ + trackH,
                   scrubberTrackX_ + scrubberTrackW_, scrubberTrackY_ + trackH,
                   1.0F, 0x334155FF);

  const std::size_t numVers = chronologicalOrder_.size();
  std::size_t currentIdx    = 0;
  for (std::size_t i = 0; i < numVers; ++i) {
    const float tx =
        scrubberTrackX_ + (numVers > 1 ? (static_cast<float>(i) /
                                          static_cast<float>(numVers - 1)) *
                                             scrubberTrackW_
                                       : scrubberTrackW_ * 0.5F);
    canvas_->addRect(tx - 0.5F, scrubberTrackY_ - 2.0F, 1.0F, trackH + 4.0F,
                     0x475569FF);
    if (chronologicalOrder_[i] == current_) {
      currentIdx = i;
    }
  }

  scrubberThumbX_ =
      scrubberTrackX_ + (numVers > 1 ? (static_cast<float>(currentIdx) /
                                        static_cast<float>(numVers - 1)) *
                                           scrubberTrackW_
                                     : scrubberTrackW_ * 0.5F);
  canvas_->setTag(render::tagKindOverlay, kTagScrubberThumb);
  drawDisc(*canvas_, scrubberThumbX_, scrubberTrackY_ + trackH * 0.5F, 8.0F,
           0x10B981FF, 0xFFFFFFFF, 1.5F);

  const std::string scrubLabel = "Time: " + current_.str() + " (" +
                                 std::to_string(currentIdx + 1) + "/" +
                                 std::to_string(numVers) + ")";
  canvas_->addText(ctx.state, scrubberTrackX_, scrubberTrackY_ + 16.0F,
                   scrubLabel, 0x94A3B8FF, 0);

  // 5. Unlimited N-way comparative diff panel
  if (comparedVersions_.size() >= 2) {
    if (diffNeedsUpdate_) {
      diffResult_      = session_.store(0).diffVersions(comparedVersions_);
      diffNeedsUpdate_ = false;
    }

    constexpr float diffW = 325.0F;
    constexpr float diffH = 145.0F;
    const float diffX     = panelX_ + panelW_ - diffW - 14.0F;
    const float diffY     = topY - diffH - 50.0F;

    canvas_->setTag(render::tagKindOverlay, 0);
    canvas_->addRect(diffX, diffY, diffW, diffH, 0x1E1E2EDD);
    canvas_->addLine(diffX, diffY, diffX + diffW, diffY, 1.0F, 0xF59E0BFF);
    canvas_->addLine(diffX + diffW, diffY, diffX + diffW, diffY + diffH, 1.0F,
                     0xF59E0BFF);
    canvas_->addLine(diffX + diffW, diffY + diffH, diffX, diffY + diffH, 1.0F,
                     0xF59E0BFF);
    canvas_->addLine(diffX, diffY + diffH, diffX, diffY, 1.0F, 0xF59E0BFF);

    const std::string diffTitle =
        "N-WAY DIFF: " + std::to_string(comparedVersions_.size()) + " VERSIONS";
    canvas_->addText(ctx.state, diffX + 8.0F, diffY + diffH - 16.0F, diffTitle,
                     0xF59E0BFF, 0);

    std::size_t uChars = 0;
    std::size_t sChars = 0;
    std::size_t qChars = 0;
    std::size_t dChars = 0;
    if (!diffResult_.versions.empty()) {
      uChars = diffResult_.versions[0].universalChars;
      sChars = diffResult_.versions[0].sharedChars;
      qChars = diffResult_.versions[0].uniqueChars;
      dChars = diffResult_.versions[0].deletedChars;
    }

    const std::string uStr =
        "Universal (Gold): " + std::to_string(uChars) + " chars";
    const std::string sStr =
        "Shared (Amber):    " + std::to_string(sChars) + " chars";
    const std::string qStr =
        "Unique (Mint):     " + std::to_string(qChars) + " chars";
    const std::string dStr =
        "Limbo (Crimson):   " + std::to_string(dChars) + " chars";

    canvas_->addText(ctx.state, diffX + 8.0F, diffY + diffH - 34.0F, uStr,
                     0xFFD700FF, 0);
    canvas_->addText(ctx.state, diffX + 8.0F, diffY + diffH - 50.0F, sStr,
                     0xF59E0BFF, 0);
    canvas_->addText(ctx.state, diffX + 8.0F, diffY + diffH - 66.0F, qStr,
                     0x10B981FF, 0);
    canvas_->addText(ctx.state, diffX + 8.0F, diffY + diffH - 82.0F, dStr,
                     0xEF4444FF, 0);

    // Buttons
    canvas_->setTag(render::tagKindOverlay, kTagQuoteButton);
    canvas_->addRect(diffX + 6.0F, diffY + 10.0F, 106.0F, 22.0F, 0x2563EBFF);
    canvas_->addText(ctx.state, diffX + 10.0F, diffY + 26.0F, "Quote into Head",
                     0xFFFFFFFF, 0);

    canvas_->setTag(render::tagKindOverlay, kTagOpen3DButton);
    canvas_->addRect(diffX + 116.0F, diffY + 10.0F, 72.0F, 22.0F, 0x059669FF);
    canvas_->addText(ctx.state, diffX + 120.0F, diffY + 26.0F, "Open in 3D",
                     0xFFFFFFFF, 0);

    canvas_->setTag(render::tagKindOverlay, kTagOnionSkinButton);
    canvas_->addRect(diffX + 192.0F, diffY + 10.0F, 82.0F, 22.0F, 0x7C3AEDFF);
    canvas_->addText(ctx.state, diffX + 196.0F, diffY + 26.0F, "Onion Skin",
                     0xFFFFFFFF, 0);

    canvas_->setTag(render::tagKindOverlay, kTagClearComp);
    canvas_->addRect(diffX + 278.0F, diffY + 10.0F, 42.0F, 22.0F, 0xDC2626FF);
    canvas_->addText(ctx.state, diffX + 282.0F, diffY + 26.0F, "Clear",
                     0xFFFFFFFF, 0);
  }

  canvas_->commit();
  canvas_->draw(ctx.state, ortho);
}

bool HypertimeGraph::picked(const render::PickingResult &pick, RenderState &) {
  if (!visible_ || pick.tag.kind != render::tagKindOverlay) {
    return false;
  }
  const auto tag = pick.tag.clusterIndex;

  if (tag == kTagScrubberThumb || tag == kTagScrubberTrack) {
    if (chronologicalOrder_.empty() || scrubberTrackW_ <= 0.0F) {
      return false;
    }
    const float relX = static_cast<float>(pick.x) - scrubberTrackX_;
    const float frac = std::clamp(relX / scrubberTrackW_, 0.0F, 1.0F);
    const auto targetIdx =
        std::min(chronologicalOrder_.size() - 1,
                 static_cast<std::size_t>(
                     frac * static_cast<float>(chronologicalOrder_.size())));
    current_ = chronologicalOrder_[targetIdx];
    if (scrubHandler_) {
      scrubHandler_(current_);
    } else if (goer_) {
      goer_(current_);
    }
    return true;
  }

  if (tag == kTagQuoteButton) {
    if (quoteHandler_ && !comparedVersions_.empty()) {
      const auto &srcVer =
          (comparedVersions_.size() > 1 && comparedVersions_[0] == current_)
              ? comparedVersions_[1]
              : comparedVersions_[0];
      const auto &st  = session_.store(0);
      const auto text = st.textOf(srcVer);
      if (!text.empty()) {
        quoteHandler_(srcVer, 0, static_cast<std::uint32_t>(text.size()));
      }
    }
    return true;
  }

  if (tag == kTagOpen3DButton) {
    if (compareHandler_ && !comparedVersions_.empty()) {
      compareHandler_(comparedVersions_);
    }
    return true;
  }

  if (tag == kTagOnionSkinButton) {
    if (onionSkinHandler_ && !comparedVersions_.empty()) {
      onionSkinHandler_(comparedVersions_);
    }
    return true;
  }

  if (tag == kTagClearComp) {
    clearComparison();
    return true;
  }

  if (tag >= kTagNodeBase) {
    const std::size_t nodeIdx = tag - kTagNodeBase;
    if (nodeIdx < nodes_.size()) {
      const auto &targetId   = nodes_[nodeIdx].id;
      const SDL_Keymod mods  = SDL_GetModState();
      const bool shiftOrCtrl = (mods & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL)) != 0;
      if (shiftOrCtrl) {
        toggleComparison(targetId);
      } else {
        current_ = targetId;
        if (goer_) {
          goer_(targetId);
        }
      }
      return true;
    }
  }

  return false;
}

void HypertimeGraph::toggleComparison(const MicroversionId &id) {
  const auto it =
      std::find(comparedVersions_.begin(), comparedVersions_.end(), id);
  if (it != comparedVersions_.end()) {
    comparedVersions_.erase(it);
  } else {
    comparedVersions_.push_back(id);
  }
  diffNeedsUpdate_ = true;
  revision_++;
}

void HypertimeGraph::clearComparison() {
  comparedVersions_.clear();
  diffNeedsUpdate_ = true;
  revision_++;
}

void HypertimeGraph::describe(gleditor::a11y::Builder &into) {
  if (!visible_ || nodes_.empty()) {
    return;
  }
  constexpr std::uint64_t mapId = 1;
  auto &mapNode                 = into.add(mapId, gleditor::a11y::Role::List);
  mapNode.label                 = "Hypertime Branching DAG";

  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    const auto &n     = nodes_[i];
    const auto nodeId = 1000U + i;
    auto &node        = into.add(nodeId, gleditor::a11y::Role::ListItem);
    std::string label =
        "Version " + n.id.str() + " [" + std::string(1, n.opLetter) + "]";
    if (!n.alias.empty()) {
      label += " (" + n.alias + ")";
    }
    node.label = label;
    node.value = (n.id == current_) ? "selected" : "";
  }

  auto &sliderNode = into.add(2000U, gleditor::a11y::Role::Group);
  sliderNode.label = "Time Scrubber";
  sliderNode.value = current_.str();

  if (comparedVersions_.size() >= 2) {
    auto &diffNode = into.add(3000U, gleditor::a11y::Role::Group);
    diffNode.label = "Comparative Diff (" +
                     std::to_string(comparedVersions_.size()) + " versions)";
  }
}

} // namespace xudu
