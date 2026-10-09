#include "common/ui/view/view_animation.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <tuple>
#include <utility>

#include <choreograph/Easing.h>
#include <glm/common.hpp>
#include <glm/gtc/quaternion.hpp>

namespace xanadu::view {

namespace {

[[nodiscard]] auto tied(const SubjectId &id) noexcept {
  return std::tie(id.kind, id.slot, id.value, id.epoch);
}

[[nodiscard]] bool subjectLess(const SubjectId &a,
                               const SubjectId &b) noexcept {
  return tied(a) < tied(b);
}

/// Anything a view derives rather than a real cell, page or document: what
/// a toss discards and brings, and what a held key holds back.
[[nodiscard]] bool derived(const SubjectId &id) noexcept {
  return id.kind != SubjectKind::Cell && id.kind != SubjectKind::Page &&
         id.kind != SubjectKind::Document;
}
[[nodiscard]] bool derived(const PlacedItem &item) noexcept {
  return derived(item.id) || (item.flags & itemViewOnly) != 0U;
}
[[nodiscard]] bool derived(const PlacedFrame &frame) noexcept {
  return derived(frame.id);
}
[[nodiscard]] bool derived(const PlacedEdge &edge) noexcept {
  return derived(edge.from) || derived(edge.to);
}

struct ItemLess {
  bool operator()(const PlacedItem &a, const PlacedItem &b) const noexcept {
    return subjectLess(a.id, b.id);
  }
};
struct FrameLess {
  bool operator()(const PlacedFrame &a, const PlacedFrame &b) const noexcept {
    return subjectLess(a.id, b.id);
  }
};
/// An edge is the relation between its two ends, whatever its label or
/// bundle are numbered this time.
struct EdgeLess {
  bool operator()(const PlacedEdge &a, const PlacedEdge &b) const noexcept {
    return std::tuple_cat(tied(a.from), tied(a.to),
                          std::tie(a.kind, a.relation)) <
           std::tuple_cat(tied(b.from), tied(b.to),
                          std::tie(b.kind, b.relation));
  }
};

/// The subjects whose MotionHint times a record: an edge moves with the end
/// it leads to, then the one it leaves.
[[nodiscard]] std::array<SubjectId, 1> hinted(const PlacedItem &item) noexcept {
  return {item.id};
}
[[nodiscard]] std::array<SubjectId, 1>
hinted(const PlacedFrame &frame) noexcept {
  return {frame.id};
}
[[nodiscard]] std::array<SubjectId, 2> hinted(const PlacedEdge &edge) noexcept {
  return {edge.to, edge.from};
}

[[nodiscard]] float ease(const MotionEase curve, const float u) noexcept {
  namespace ch = choreograph;
  switch (curve) {
  case MotionEase::Linear:
    return ch::easeNone(u);
  case MotionEase::InQuad:
    return ch::easeInQuad(u);
  case MotionEase::OutQuad:
    return ch::easeOutQuad(u);
  case MotionEase::InOutQuad:
    return ch::easeInOutQuad(u);
  case MotionEase::InCubic:
    return ch::easeInCubic(u);
  case MotionEase::OutCubic:
    return ch::easeOutCubic(u);
  case MotionEase::InOutCubic:
    return ch::easeInOutCubic(u);
  }
  return u;
}

[[nodiscard]] PlacedItem mix(const PlacedItem &a, PlacedItem b,
                             const float e) noexcept {
  b.centre      = glm::mix(a.centre, b.centre, e);
  b.orientation = glm::slerp(a.orientation, b.orientation, e);
  b.width       = glm::mix(a.width, b.width, e);
  b.height      = glm::mix(a.height, b.height, e);
  b.opacity     = glm::mix(a.opacity, b.opacity, e);
  if (a.window && b.window) {
    b.window->top    = glm::mix(a.window->top, b.window->top, e);
    b.window->bottom = glm::mix(a.window->bottom, b.window->bottom, e);
  }
  return b;
}
[[nodiscard]] PlacedFrame mix(const PlacedFrame &a, PlacedFrame b,
                              const float e) noexcept {
  b.centre      = glm::mix(a.centre, b.centre, e);
  b.orientation = glm::slerp(a.orientation, b.orientation, e);
  b.width       = glm::mix(a.width, b.width, e);
  b.height      = glm::mix(a.height, b.height, e);
  b.opacity     = glm::mix(a.opacity, b.opacity, e);
  return b;
}
[[nodiscard]] PlacedEdge mix(const PlacedEdge &a, PlacedEdge b,
                             const float e) noexcept {
  b.a         = glm::mix(a.a, b.a, e);
  b.b         = glm::mix(a.b, b.b, e);
  b.gather[0] = glm::mix(a.gather[0], b.gather[0], e);
  b.gather[1] = glm::mix(a.gather[1], b.gather[1], e);
  b.opacity   = glm::mix(a.opacity, b.opacity, e);
  return b;
}

/// Whether two records would be drawn alike, so a relayout that moved
/// nothing starts no motion and the frame can settle.
[[nodiscard]] bool alike(const PlacedItem &a, const PlacedItem &b) noexcept {
  return a.centre == b.centre && a.orientation == b.orientation &&
         a.width == b.width && a.height == b.height && a.opacity == b.opacity;
}
[[nodiscard]] bool alike(const PlacedFrame &a, const PlacedFrame &b) noexcept {
  return a.centre == b.centre && a.orientation == b.orientation &&
         a.width == b.width && a.height == b.height && a.opacity == b.opacity;
}
[[nodiscard]] bool alike(const PlacedEdge &a, const PlacedEdge &b) noexcept {
  return a.a == b.a && a.b == b.b && a.gather == b.gather &&
         a.opacity == b.opacity;
}

/// New derived cells grow into place as they fade in.
void scale(PlacedItem &item, const float by) noexcept {
  item.width *= by;
  item.height *= by;
}
void scale(PlacedFrame &frame, const float by) noexcept {
  frame.width *= by;
  frame.height *= by;
}
void scale(PlacedEdge & /*edge*/, float /*by*/) noexcept {}

/// A ghost's position is never animated (plan §5.3): sliding one would say
/// something is arriving. It only fades.
void holdGhost(PlacedItem &from, const PlacedItem &to) noexcept {
  if ((to.flags & itemGhost) != 0U) {
    from.centre      = to.centre;
    from.orientation = to.orientation;
    from.width       = to.width;
    from.height      = to.height;
  }
}
void holdGhost(PlacedFrame & /*from*/, const PlacedFrame & /*to*/) noexcept {}
void holdGhost(PlacedEdge & /*from*/, const PlacedEdge & /*to*/) noexcept {}

} // namespace

struct ViewAnimation::Change {
  MotionCause cause{};
  bool toss{};
  /// A step that came before the last one's motion had ended: a held key.
  bool held{};
  /// Since the step before: one repeat interval.
  double repeatMs{};
  std::span<const MotionHint> hints; // sorted by subject
};

ViewAnimation::ViewAnimation(const MotionConfig &config) : config_(config) {}

ViewAnimation *ViewAnimation::configure(const MotionConfig &config) {
  config_ = config;
  if (config_.reduced) {
    cut();
  }
  return this;
}

void ViewAnimation::cut() {
  // Leaving tracks are always after the rest (retargetTracks() appends
  // them), so dropping them keeps the indices edges hold into items_.
  const auto settle = [this](auto &tracks) {
    std::erase_if(tracks, [](const auto &track) { return track.leaving; });
    for (auto &track : tracks) {
      track.from   = track.to;
      track.timing = {.startMs = nowMs_, .durationMs = 0.0};
    }
  };
  settle(items_);
  settle(frames_);
  settle(edges_);
}

double ViewAnimation::progress(const Timing &timing) const noexcept {
  if (nowMs_ < timing.startMs) {
    return 0.0;
  }
  if (timing.durationMs <= 0.0) {
    return 1.0;
  }
  return std::min(1.0, (nowMs_ - timing.startMs) / timing.durationMs);
}

bool ViewAnimation::running(const Timing &timing) const noexcept {
  return nowMs_ < timing.startMs + timing.durationMs;
}

template <typename Record>
Record ViewAnimation::sample(const Track<Record> &track) const noexcept {
  return mix(
      track.from, track.to,
      ease(track.timing.ease, static_cast<float>(progress(track.timing))));
}

template <typename Record, typename KeyLess>
void ViewAnimation::retargetTracks(std::vector<Track<Record>> &tracks,
                                   std::vector<Track<Record>> &next,
                                   const std::vector<Record> &targets,
                                   const Change &change, const KeyLess less) {
  const auto causeSpec = [&]() -> const MotionSpec & {
    switch (change.cause) {
    case MotionCause::Move:
    case MotionCause::Rebind:
      return config_.step;
    case MotionCause::ViewSwitch:
      return config_.viewSwitch;
    case MotionCause::SubViewSwitch:
      return config_.subViewSwitch;
    }
    return config_.step;
  }();
  const auto hintFor = [&](const Record &record) -> const MotionHint * {
    for (const auto &id : hinted(record)) {
      const auto found = std::ranges::lower_bound(change.hints, id, subjectLess,
                                                  &MotionHint::id);
      if (found != change.hints.end() && found->id == id) {
        return &*found;
      }
    }
    return nullptr;
  };

  index_.clear();
  for (std::uint32_t i = 0; i < tracks.size(); ++i) {
    index_.push_back({.track = i, .leaving = tracks[i].leaving});
  }
  // What is still shown is matched before a copy of it that is fading out,
  // as after a view switch both can carry one id.
  std::ranges::sort(index_, [&](const IndexEntry &a, const IndexEntry &b) {
    const auto &ra = tracks[a.track].to;
    const auto &rb = tracks[b.track].to;
    if (less(ra, rb)) {
      return true;
    }
    if (less(rb, ra)) {
      return false;
    }
    return !a.leaving && b.leaving;
  });
  matched_.assign(tracks.size(), false);

  next.clear();
  for (const auto &target : targets) {
    std::optional<std::uint32_t> old;
    if (change.cause != MotionCause::ViewSwitch) {
      auto at = std::ranges::lower_bound(
          index_, target, less, [&](const IndexEntry &e) -> const Record & {
            return tracks[e.track].to;
          });
      for (; at != index_.end() && !less(target, tracks[at->track].to); ++at) {
        if (!matched_[at->track]) {
          old                 = at->track;
          matched_[at->track] = true;
          break;
        }
      }
    }
    const auto *hint = hintFor(target);
    // A view switch cross-fades everything alike; otherwise what a toss
    // brings is told apart from what merely came into view.
    const bool tossed = change.cause != MotionCause::ViewSwitch &&
                        change.toss && derived(target);
    Track<Record> track{.to = target};
    if (old) {
      track.from = sample(tracks[*old]);
      holdGhost(track.from, target);
      track.timing = {.startMs    = nowMs_,
                      .durationMs = causeSpec.durationMs,
                      .ease       = causeSpec.ease};
    } else {
      const auto &spec   = tossed ? config_.tossIn : causeSpec;
      track.from         = target;
      track.from.opacity = 0.0F;
      if (tossed) {
        scale(track.from, config_.tossInScale);
      }
      track.timing = {
          .startMs = nowMs_, .durationMs = spec.durationMs, .ease = spec.ease};
      // Under a held key a derived cell waits one repeat before it fades
      // in, so one gone by the next repeat is never seen (plan §5.3).
      if (change.held && derived(target)) {
        track.timing.startMs += change.repeatMs;
      }
    }
    if (hint != nullptr) {
      track.timing.startMs += hint->delayMs;
      track.timing.durationMs = hint->durationMs;
      track.timing.ease       = config_.hint;
    }
    if (config_.reduced || (old && alike(track.from, target))) {
      track.from   = target;
      track.timing = {.startMs = nowMs_, .durationMs = 0.0};
    }
    next.push_back(track);
  }

  for (std::uint32_t i = 0; i < tracks.size(); ++i) {
    if (matched_[i] || config_.reduced) {
      continue;
    }
    const auto &old = tracks[i];
    if (old.leaving) {
      // Already fading on its own clock; starting again would lengthen it.
      if (!finished(old)) {
        next.push_back(old);
        next.back().labelTrack.reset();
      }
      continue;
    }
    Track<Record> track{.from = sample(old), .leaving = true};
    if (track.from.opacity <= 0.0F) {
      continue; // never seen, so it does not fade
    }
    const auto &spec = change.cause != MotionCause::ViewSwitch && change.toss &&
                               derived(old.to)
                           ? config_.tossOut
                           : causeSpec;
    if (spec.durationMs <= 0.0F) {
      continue;
    }
    // In place, from the copy last drawn: nothing is looked up.
    track.to         = track.from;
    track.to.opacity = 0.0F;
    track.timing     = {
            .startMs = nowMs_, .durationMs = spec.durationMs, .ease = spec.ease};
    next.push_back(track);
  }
  std::swap(tracks, next);
}

ViewAnimation *ViewAnimation::retarget(const AnimationTarget &target) {
  const auto &sink = target.layout;

  targetItems_.clear();
  trackOfSinkItem_.clear();
  for (const auto &item : sink.items()) {
    const auto pose = placedPose(sink, item);
    if (!pose) {
      trackOfSinkItem_.emplace_back();
      continue;
    }
    trackOfSinkItem_.emplace_back(
        static_cast<std::uint32_t>(targetItems_.size()));
    auto &flat       = targetItems_.emplace_back(item);
    flat.centre      = pose->centre;
    flat.orientation = pose->orientation;
    flat.frame.reset();
  }
  targetFrames_.clear();
  for (const auto &frame : sink.frames()) {
    if (const auto pose = placedPose(sink, frame)) {
      auto &flat       = targetFrames_.emplace_back(frame);
      flat.centre      = pose->centre;
      flat.orientation = pose->orientation;
      flat.parent.reset();
    }
  }
  targetEdges_.clear();
  targetLabels_.clear();
  for (const auto &edge : sink.edges()) {
    targetEdges_.push_back(edge);
    targetLabels_.push_back(edge.label && *edge.label < trackOfSinkItem_.size()
                                ? trackOfSinkItem_[*edge.label]
                                : std::nullopt);
  }
  hints_.assign(sink.hints().begin(), sink.hints().end());
  // Not stable_sort, which takes a buffer from the heap on every call.
  std::ranges::sort(hints_, subjectLess, &MotionHint::id);

  Change change{.cause = target.cause, .hints = hints_};
  change.toss = epoch_.has_value() && *epoch_ != target.epoch;
  if (target.cause == MotionCause::Move) {
    if (lastMoveMs_) {
      change.repeatMs = nowMs_ - *lastMoveMs_;
      change.held     = change.repeatMs < config_.step.durationMs;
    }
    lastMoveMs_ = nowMs_;
  } else {
    lastMoveMs_.reset(); // the next step starts a press, not a repeat
  }
  epoch_ = target.epoch;

  retargetTracks(items_, nextItems_, targetItems_, change, ItemLess{});
  retargetTracks(frames_, nextFrames_, targetFrames_, change, FrameLess{});
  retargetTracks(edges_, nextEdges_, targetEdges_, change, EdgeLess{});
  // The first tracks are the targets, in order (retargetTracks()).
  for (std::size_t i = 0; i < targetLabels_.size(); ++i) {
    edges_[i].labelTrack = targetLabels_[i];
  }
  dropTargets_.assign(sink.dropTargets().begin(), sink.dropTargets().end());
  return this;
}

ViewAnimation *ViewAnimation::advance(const Millis dt, LayoutSink &out) {
  nowMs_ += dt.count();
  out.clear();
  for (auto &track : frames_) {
    if (!finished(track)) {
      out.push(sample(track));
    }
  }
  for (auto &track : items_) {
    track.written.reset();
    if (!finished(track)) {
      track.written = out.push(sample(track));
    }
  }
  for (const auto &track : edges_) {
    if (finished(track)) {
      continue;
    }
    auto edge = sample(track);
    edge.label =
        track.labelTrack ? items_[*track.labelTrack].written : std::nullopt;
    out.push(edge);
  }
  for (const auto &target : dropTargets_) {
    out.push(target);
  }
  return this;
}

bool ViewAnimation::animating() const noexcept {
  const auto any = [this](const auto &tracks) {
    return std::ranges::any_of(
        tracks, [this](const auto &track) { return running(track.timing); });
  };
  return any(items_) || any(frames_) || any(edges_);
}

} // namespace xanadu::view
