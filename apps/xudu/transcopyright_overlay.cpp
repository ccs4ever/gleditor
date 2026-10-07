/**
 * @file transcopyright_overlay.cpp
 * @brief Retained Transcopyright badge presentation.
 */
#include "transcopyright_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <tuple>

#include <gleditor/render_state.hpp>
#include <gleditor/text/fit.hpp>
#include <gleditor/text/font.hpp>
#include <gleditor/ui/metrics.hpp>
#include <glm/ext/matrix_clip_space.hpp>

#include "session.hpp"

namespace xudu {
namespace {
bool sameBox(const std::optional<gleditor::ui::Rect> &a,
             const std::optional<gleditor::ui::Rect> &b) {
  if (a.has_value() != b.has_value()) return false;
  return !a || std::tie(a->left, a->bottom, a->width, a->height) ==
                   std::tie(b->left, b->bottom, b->width, b->height);
}
} // namespace

TranscopyrightOverlay::TranscopyrightOverlay(Session &session, RendererRef,
                                             std::string fontName)
    : TranscopyrightOverlay(
          [&session](const RenderState &state) {
            std::vector<xanadu::HoleSpanInfo> holes;
            for (const auto &doc : state.docs) {
              if (!doc || doc->isClosing()) continue;
              auto next = session.holesForView(doc->documentIndex());
              holes.insert(holes.end(), std::make_move_iterator(next.begin()),
                           std::make_move_iterator(next.end()));
            }
            return holes;
          },
          [&session] { return session.generation(); }, {},
          std::move(fontName)) {
  lifetime_ = std::make_shared<TranscopyrightOverlay *>(this);
  session.setTranscopyrightUnlockedHandler(
      [alive = std::weak_ptr(lifetime_)](std::size_t doc,
                                         const xanadu::PrimediaSpan &span,
                                         std::uint64_t cost) {
        if (const auto owner = alive.lock())
          (*owner)->notifyUnlocked(doc, span, cost);
      });
}

TranscopyrightOverlay::TranscopyrightOverlay(HoleSource source,
                                             HoleRevision revision,
                                             Projector projector,
                                             std::string fontName)
    : source_(std::move(source)), sourceRevision_(std::move(revision)),
      projector_(std::move(projector)), fontName_(std::move(fontName)) {}

TranscopyrightOverlay::~TranscopyrightOverlay() = default;

void TranscopyrightOverlay::deviceReady(render::RenderDevice &device,
                                        const render::PipelineDesc &pipeline) {
  device_   = &device;
  pipeline_ = pipeline;
  canvas_.reset();
  animationCanvas_.reset();
  built_     = false;
  pickState_ = nullptr;
}

bool TranscopyrightOverlay::busy() const {
  const std::scoped_lock lock(actionMutex_);
  return animating_.load(std::memory_order_relaxed) || !pendingActions_.empty();
}

void TranscopyrightOverlay::notifyUnlocked(std::size_t docIndex,
                                           const xanadu::PrimediaSpan &span,
                                           std::uint64_t) {
  holesDirty_ = true;
  for (const auto &badge : activeBadges_) {
    if (badge.docIndex == docIndex && badge.span == span) {
      animating_.store(true, std::memory_order_relaxed);
      uncurlingAnims_.push_back(
          {.box = {badge.screenX, badge.screenY, badge.width, badge.height}});
      break;
    }
  }
}

void TranscopyrightOverlay::releaseSnapshots() {
  const auto removed = std::erase_if(snapshots_, [&](const auto &snapshot) {
    return snapshot != pickTargets_ && snapshot.use_count() == 1;
  });
  if (!removed) return;
  std::erase_if(targets_, [&](const auto &binding) {
    return std::ranges::none_of(snapshots_, [&](const auto &snapshot) {
      return std::ranges::find(*snapshot, binding.first) != snapshot->end();
    });
  });
}

void TranscopyrightOverlay::refreshHoles(const RenderState &state) {
  const auto revision = sourceRevision_ ? sourceRevision_() : 0;
  if (!holesDirty_ && holesRevision_ == revision) return;
  auto holes = source_ ? source_(state) : std::vector<xanadu::HoleSpanInfo>{};
  std::vector<Entry> next;
  next.reserve(holes.size());
  for (auto &hole : holes) {
    const auto old = std::ranges::find_if(entries_, [&](const auto &entry) {
      return entry.hole.docIndex == hole.docIndex &&
             entry.hole.storeIndex == hole.storeIndex &&
             entry.hole.span == hole.span;
    });
    std::uint32_t id{};
    if (old != entries_.end())
      id = old->id;
    else {
      if (nextId_ == std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("Transcopyright badge identities exhausted");
      id = nextId_++;
    }
    auto label = hole.badgeText();
    next.push_back(
        {.hole = std::move(hole), .label = std::move(label), .id = id});
  }
  entries_       = std::move(next);
  holesDirty_    = false;
  holesRevision_ = revision;
  built_         = false;
}

std::optional<gleditor::ui::Rect>
TranscopyrightOverlay::project(Entry &entry,
                               const gleditor::FrameContext &ctx) {
  if (projector_) return projector_(entry.hole, ctx);
  const auto index = entry.hole.docIndex;
  if (index >= ctx.state.docs.size()) return {};
  const auto &doc = ctx.state.docs[index];
  if (!doc || doc->isClosing()) return {};
  const auto edits = doc->editGeneration();
  const auto pages = doc->numPages(), built = doc->builtPageCount();
  if (entry.document != doc.get() || entry.edits != edits ||
      entry.pages != pages || entry.builtPages != built) {
    entry.document   = doc.get();
    entry.edits      = edits;
    entry.pages      = pages;
    entry.builtPages = built;
    entry.first      = doc->anchorFor(entry.hole.charStart);
    entry.last =
        doc->anchorFor(entry.hole.charEnd > 0 ? entry.hole.charEnd - 1 : 0);
  }
  if (!entry.first) return {};
  const auto first = doc->worldPoint(*entry.first);
  if (!first) return {};
  const auto screen = [&](glm::vec3 point) -> std::optional<glm::vec2> {
    const auto clip = ctx.viewProjection * glm::vec4(point, 1);
    if (clip.w <= .001F) return {};
    const auto ndc = glm::vec3(clip) / clip.w;
    return glm::vec2((ndc.x * .5F + .5F) * ctx.screenWidth,
                     (ndc.y * .5F + .5F) * ctx.screenHeight);
  };
  const auto start = screen(*first);
  if (!start) return {};
  auto endX = start->x;
  if (entry.last) {
    if (const auto world = doc->worldPoint(*entry.last))
      if (const auto end = screen(*world)) endX = end->x;
  }
  if (start->x < 0 || start->x > ctx.screenWidth || start->y < 0 ||
      start->y > ctx.screenHeight)
    return {};
  return gleditor::ui::Rect{std::min(start->x, endX), start->y,
                            std::abs(start->x - endX), 0};
}

void TranscopyrightOverlay::rebuild(gleditor::FrameContext &ctx) {
  canvas_->clear();
  activeBadges_.clear();
  const auto safe = metrics_.pixelSafeArea();
  canvas_->pushClip(safe);
  auto targets        = std::make_shared<std::vector<std::uint32_t>>();
  auto accessible     = std::make_shared<std::vector<AccessibleBadge>>();
  const float line    = font_->metrics().lineHeight;
  const float padding = std::max(0.0F, theme_.paddingEm * line);
  for (const auto &entry : entries_) {
    if (!entry.box) continue;
    const auto box        = *entry.box;
    const auto &hole      = entry.hole;
    const auto locked     = hole.isLocked();
    const auto background = locked ? xanadu::kTranscopyrightColour : hole.color;
    const auto foreground = locked ? gleditor::ui::rgba(theme_.colours.text)
                                   : gleditor::ui::rgba(theme_.colours.muted);
    const Target target{hole.docIndex, hole.storeIndex, hole.span};
    if (locked) {
      targets->push_back(entry.id);
      targets_.insert_or_assign(entry.id, target);
      canvas_->setTag(render::tagKindOverlay,
                      static_cast<std::uint32_t>(targets->size()));
      activeBadges_.push_back({.tagId      = entry.id,
                               .docIndex   = hole.docIndex,
                               .storeIndex = hole.storeIndex,
                               .span       = hole.span,
                               .screenX    = box.left,
                               .screenY    = box.bottom,
                               .width      = box.width,
                               .height     = box.height,
                               .text       = entry.label,
                               .isLocked   = true,
                               .color      = hole.color});
    } else
      canvas_->setTag(render::tagKindOverlay, 0);
    canvas_->addRect(box.left, box.bottom, box.width, box.height, background);
    const auto pad = std::min(padding, std::min(box.width, box.height) * .25F);
    const gleditor::ui::Rect textBox{box.left + pad, box.bottom + pad,
                                     std::max(0.0F, box.width - 2 * pad),
                                     std::max(0.0F, box.height - 2 * pad)};
    if (textBox.width > 0 && textBox.height >= line) {
      const auto &fitted = shaping_.fitted(
          entry.label, font_,
          {.maxWidthPx = textBox.width, .maxHeightPx = textBox.height});
      auto placed = textBox;
      placed.bottom += (placed.height - fitted.heightPx) * .5F;
      placed.height = fitted.heightPx;
      canvas_->addText(ctx.state, placed, fitted, foreground, background);
    }
    accessible->push_back(
        {.id     = entry.id,
         .target = target,
         .label  = entry.label,
         .bounds = {box.left, metrics_.screenHeight - box.bottom - box.height,
                    box.left + box.width, metrics_.screenHeight - box.bottom},
         .locked = locked});
  }
  canvas_->popClip();
  canvas_->commit();
  ctx.state.glyphCache.flush();
  pickTargets_ = std::move(targets);
  snapshots_.push_back(pickTargets_);
  accessible_.store(std::move(accessible));
  revision_.fetch_add(1, std::memory_order_relaxed);
  built_ = true;
}

void TranscopyrightOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!device_) return;
  refreshHoles(ctx.state);
  {
    std::vector<PendingAction> pending;
    {
      const std::scoped_lock lock(actionMutex_);
      pending.swap(pendingActions_);
    }
    if (onUnlock_)
      for (const auto &action : pending) {
        const auto current =
            std::ranges::find_if(entries_, [&](const auto &entry) {
              return entry.id == action.id && entry.hole.isLocked() &&
                     Target{entry.hole.docIndex, entry.hole.storeIndex,
                            entry.hole.span} == action.target;
            });
        if (current != entries_.end())
          onUnlock_(action.target.storeIndex, action.target.span);
      }
  }
  // A synchronous unlock callback can change the source while dispatching.
  refreshHoles(ctx.state);
  auto metrics                   = ctx.metrics;
  metrics.screenWidth            = ctx.screenWidth;
  metrics.screenHeight           = ctx.screenHeight;
  metrics.chrome                 = ctx.chrome;
  const bool presentationChanged = metrics != metrics_ || ctx.theme != theme_;
  bool changed                   = !built_ || presentationChanged;
  if (changed) {
    const auto description = gleditor::ui::scaledFontDescription(
        fontName_, gleditor::ui::FontRole::Caption, metrics, ctx.theme);
    if (!canvas_ || drawnFont_ != description) {
      drawnFont_ = description;
      font_      = gleditor::text::FontManager::instance().getFont(description);
      canvas_    = std::make_unique<gleditor::Canvas>(device_, description);
      animationCanvas_ =
          std::make_unique<gleditor::Canvas>(device_, description);
      canvas_->createPipeline(pipeline_, false);
      animationCanvas_->createPipeline(pipeline_, false);
    }
    metrics_ = metrics;
    theme_   = ctx.theme;
  }
  if (pickState_ != &ctx.state) {
    pickState_ = &ctx.state;
    pickScope_ = ctx.state.allocatePersistentOverlayPickScope();
  }
  canvas_->setIdentity(pickScope_, 0);
  animationCanvas_->setIdentity(pickScope_, 0);
  const auto safe = metrics_.pixelSafeArea();
  const auto line = font_->metrics().lineHeight;
  const auto pad  = std::max(0.0F, line * theme_.paddingEm);
  const auto height =
      std::max(metrics_.px(theme_.type.minTouchPx), line + 2 * pad);
  for (auto &entry : entries_) {
    if (presentationChanged || entry.intrinsicWidth == 0)
      entry.intrinsicWidth =
          shaping_.fitted(entry.label, font_, {}).widthPx + 2 * pad;
    auto anchor = project(entry, ctx);
    std::optional<gleditor::ui::Rect> box;
    if (anchor && safe.width > 0 && safe.height > 0 &&
        std::isfinite(anchor->left) && std::isfinite(anchor->bottom) &&
        std::isfinite(anchor->width) && std::isfinite(anchor->height)) {
      box = gleditor::ui::placeNear(
          *anchor,
          std::max({height, entry.intrinsicWidth,
                    std::max(0.0F, anchor->width) + 2 * pad}),
          height, safe);
    }
    changed |= !sameBox(entry.box, box);
    entry.box = box;
  }
  if (changed) rebuild(ctx);
  releaseSnapshots();
  if (pickTargets_)
    ctx.state.bindOverlayWidgets(
        render::packTagIdentity(render::tagKindOverlay, pickScope_, 0),
        pickTargets_);
  const auto ortho =
      glm::ortho(0.0F, static_cast<float>(ctx.screenWidth), 0.0F,
                 static_cast<float>(ctx.screenHeight), -1.0F, 1.0F);
  canvas_->draw(ctx.state, ortho);
  if (!uncurlingAnims_.empty()) {
    animationCanvas_->clear();
    animationCanvas_->pushClip(safe);
    animationCanvas_->setTag(render::tagKindOverlay, 0);
    for (auto &animation : uncurlingAnims_) {
      animation.progress = std::min(1.0F, animation.progress + .05F);
      const auto alpha =
          static_cast<std::uint32_t>((1 - animation.progress) * 220);
      const auto grow = animation.progress * height * .5F;
      const auto &box = animation.box;
      animationCanvas_->addRect(box.left - grow, box.bottom - grow,
                                box.width + 2 * grow, box.height + 2 * grow,
                                (xanadu::kTranscopyrightColour & 0xFFFFFF00U) |
                                    alpha);
    }
    animationCanvas_->popClip();
    animationCanvas_->commit();
    animationCanvas_->draw(ctx.state, ortho);
    std::erase_if(uncurlingAnims_, [](const auto &animation) {
      return animation.progress >= 1;
    });
    animating_.store(!uncurlingAnims_.empty(), std::memory_order_relaxed);
  }
}

bool TranscopyrightOverlay::picked(const render::PickingResult &pick,
                                   RenderState &) {
  if (pick.tag.kind != render::tagKindOverlay) return false;
  std::uint32_t id{};
  if (pick.tag.docIndex == pickScope_ && pick.tag.pageIndex == 0 &&
      pickScope_ != 0) {
    if (pick.requestId != 0) {
      if (!pick.overlayWidgetId) return false;
      id = *pick.overlayWidgetId;
    } else {
      if (!pickTargets_ || pick.tag.clusterIndex == 0 ||
          pick.tag.clusterIndex > pickTargets_->size())
        return false;
      id = (*pickTargets_)[pick.tag.clusterIndex - 1];
    }
  } else {
    if (pick.requestId != 0 || pick.tag.docIndex != 0 ||
        pick.tag.pageIndex != 0)
      return false;
    id = pick.tag.clusterIndex;
    if (std::ranges::none_of(activeBadges_, [id](const auto &badge) {
          return badge.tagId == id;
        }))
      return false;
  }
  const auto target = targets_.find(id);
  if (target == targets_.end()) return false;
  const auto meaning = target->second;
  if (std::ranges::none_of(entries_, [&](const auto &entry) {
        return entry.id == id && entry.hole.isLocked() &&
               Target{entry.hole.docIndex, entry.hole.storeIndex,
                      entry.hole.span} == meaning;
      }))
    return false;
  if (onUnlock_) onUnlock_(meaning.storeIndex, meaning.span);
  return true;
}

void TranscopyrightOverlay::describe(gleditor::a11y::Builder &into) {
  const auto snapshot = accessible_.load();
  if (!snapshot) return;
  for (const auto &badge : *snapshot) {
    auto &node  = into.add(badge.id, badge.locked ? gleditor::a11y::Role::Button
                                                  : gleditor::a11y::Role::Label);
    node.label  = badge.label;
    node.bounds = badge.bounds;
    node.focusable = badge.locked;
    node.actions =
        badge.locked ? gleditor::a11y::bit(gleditor::a11y::Action::Click) : 0;
    into.contribute(into.id(badge.id));
  }
}
std::uint64_t TranscopyrightOverlay::accessibilityRevision() const {
  return revision_.load(std::memory_order_relaxed);
}
bool TranscopyrightOverlay::performAction(std::uint64_t node,
                                          gleditor::a11y::Action action,
                                          std::string_view) {
  if (action != gleditor::a11y::Action::Click) return false;
  const auto snapshot = accessible_.load();
  if (!snapshot) return false;
  const auto local = gleditor::a11y::Ids::localOf(node);
  const auto found = std::ranges::find(*snapshot, local, &AccessibleBadge::id);
  if (found == snapshot->end() || !found->locked) return false;
  const std::scoped_lock lock(actionMutex_);
  pendingActions_.push_back({found->id, found->target});
  return true;
}
} // namespace xudu
