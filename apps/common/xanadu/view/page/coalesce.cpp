#include "common/xanadu/view/page/coalesce.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

#include <gleditor/logging.hpp>

namespace xanadu::view {
namespace {

constexpr const char *kLayoutCategory = "view.layout";

[[nodiscard]] bool usable(const CoalesceTie &tie, const std::size_t bodies) {
  return tie.from < bodies && tie.to < bodies && tie.from != tie.to;
}

/// How far a passage's centre stands above its page's centre: heights are
/// measured down from the top, and y runs up.
[[nodiscard]] float aboveCentre(const CoalesceBody &body, const float height) {
  return (0.5F * body.height) - height;
}

/// Sets every tied passage level with the one it is tied to, outward from
/// the pinned pages, so that a chain of ties is levelled from the anchor
/// rather than from wherever a spring happened to stop. A page reached by
/// two ties takes the first; a group with no pinned page is levelled from
/// its first page, which keeps its home height.
void levelHeights(std::span<CoalesceBody> bodies,
                  std::span<const CoalesceTie> ties) {
  const auto n = bodies.size();
  std::vector<std::uint8_t> levelled(n, 0U);
  std::vector<std::size_t> queue;
  queue.reserve(n);
  std::size_t head = 0;

  const auto drain = [&] {
    for (; head < queue.size(); ++head) {
      const auto at = queue[head];
      for (const auto &tie : ties) {
        if (!usable(tie, n)) {
          continue;
        }
        const bool outward = tie.from == at && levelled[tie.to] == 0U;
        const bool inward  = tie.to == at && levelled[tie.from] == 0U;
        if (!outward && !inward) {
          continue;
        }
        const auto next         = outward ? tie.to : tie.from;
        const float atHeight    = outward ? tie.fromHeight : tie.toHeight;
        const float nextHeight  = outward ? tie.toHeight : tie.fromHeight;
        bodies[next].position.y = bodies[at].position.y +
                                  aboveCentre(bodies[at], atHeight) -
                                  aboveCentre(bodies[next], nextHeight);
        levelled[next] = 1U;
        queue.push_back(next);
      }
    }
  };

  for (std::size_t i = 0; i < n; ++i) {
    if (bodies[i].pinned) {
      levelled[i] = 1U;
      queue.push_back(i);
    }
  }
  drain();
  for (std::size_t i = 0; i < n; ++i) {
    if (levelled[i] == 0U) {
      levelled[i] = 1U;
      queue.push_back(i);
      drain();
    }
  }
}

/// Horizontal positions from a fresh engine, in the engine's own units. Every
/// body sits at height zero there: the heights are already decided, and a
/// spring left to pull on them would only keep the bodies from settling.
void settle(std::span<CoalesceBody> bodies, std::span<const CoalesceTie> ties,
            const TensionParams &physics, const PageBaseConfig &page) {
  const float unit = page.physicsUnitPx;
  if (!(unit > 0.0F) || !(physics.timeStep > 0.0F)) {
    GLEDITOR_LOG_DEBUG(kLayoutCategory,
                       "coalesce leaves pages in their row: unit {} px, "
                       "time step {} s",
                       unit, physics.timeStep);
    return;
  }

  TensionLayoutEngine engine(physics);
  for (std::size_t i = 0; i < bodies.size(); ++i) {
    TensionBody body;
    body.docIndex        = i;
    body.position        = glm::vec3(bodies[i].home.x / unit, 0.0F, 0.0F);
    body.restingPosition = body.position;
    body.width           = bodies[i].width / unit;
    body.height          = bodies[i].height / unit;
    body.pinned          = bodies[i].pinned;
    engine.setBody(body);
  }
  for (const auto &tie : ties) {
    if (!usable(tie, bodies.size())) {
      continue;
    }
    TensionConstraint constraint;
    constraint.fromDoc   = tie.from;
    constraint.toDoc     = tie.to;
    constraint.targetGap = tie.gap / unit;
    constraint.active    = true;
    constraint.side      = bodies[tie.to].home.x < bodies[tie.from].home.x
                               ? AlignSide::Left
                               : AlignSide::Right;
    engine.addConstraint(constraint);
  }

  // A body at rest reads as settled before it has been stepped at all, so
  // the test follows each step rather than preceding it.
  std::uint32_t steps = 0;
  bool settled        = false;
  while (!settled && steps < page.coalesceStepCap) {
    engine.step(physics.timeStep);
    ++steps;
    settled = engine.isSettled();
  }
  GLEDITOR_LOG_DEBUG(kLayoutCategory,
                     "coalesce of {} pages and {} ties {} after {} steps",
                     bodies.size(), ties.size(),
                     settled ? "settled" : "reached the cap", steps);

  const auto &solved = engine.bodies();
  for (std::size_t i = 0; i < bodies.size(); ++i) {
    bodies[i].position.x = solved[i].position.x * unit;
  }
}

[[nodiscard]] bool shareHeight(const CoalesceBody &a, const CoalesceBody &b) {
  return std::abs(a.position.y - b.position.y) < 0.5F * (a.height + b.height);
}

/// Pushes each unpinned page away from the anchor, in the order the row had
/// them, until it is @p gap clear of every page already placed beside it.
/// Row order rather than solved order, so pages do not swap places and a
/// page's ghost and tether do not cross another's.
void separate(std::span<CoalesceBody> bodies, const float gap) {
  const auto n = bodies.size();
  const auto anchor =
      std::ranges::find_if(bodies, [](const auto &b) { return b.pinned; });
  const float pivot =
      anchor != bodies.end() ? anchor->home.x : bodies[0].home.x;

  std::vector<std::size_t> placed;
  std::vector<std::size_t> left;
  std::vector<std::size_t> right;
  placed.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (bodies[i].pinned) {
      placed.push_back(i);
    } else if (bodies[i].home.x < pivot) {
      left.push_back(i);
    } else {
      right.push_back(i);
    }
  }
  // Nearest the anchor first on each side; stable, so equal homes keep
  // their order in the input.
  std::ranges::stable_sort(left, [&](const auto a, const auto b) {
    return bodies[a].home.x > bodies[b].home.x;
  });
  std::ranges::stable_sort(right, [&](const auto a, const auto b) {
    return bodies[a].home.x < bodies[b].home.x;
  });

  const auto place = [&](const std::size_t i, const bool leftwards) {
    auto &body = bodies[i];
    float x    = body.position.x;
    // Each push carries the page past one placed page's far bound, which it
    // never meets again, so this ends within one push per placed page. The
    // bound is compared exactly as it was assigned, so rounding cannot leave
    // a page forever just inside it.
    for (;;) {
      std::optional<float> next;
      for (const auto p : placed) {
        const auto &other = bodies[p];
        if (!shareHeight(body, other)) {
          continue;
        }
        const float reach = (0.5F * (body.width + other.width)) + gap;
        const float low   = other.position.x - reach;
        const float high  = other.position.x + reach;
        if (!(low < x && x < high)) {
          continue;
        }
        const float to = leftwards ? low : high;
        next           = !next ? to
                               : (leftwards ? std::max(*next, to) : std::min(*next, to));
      }
      if (!next) {
        break;
      }
      x = *next;
    }
    body.position.x = x;
    placed.push_back(i);
  };
  for (const auto i : left) {
    place(i, true);
  }
  for (const auto i : right) {
    place(i, false);
  }
}

} // namespace

TensionCoalesce::TensionCoalesce(TensionParams physics,
                                 PageBaseConfig page) noexcept
    : physics_(physics), page_(page) {}

void TensionCoalesce::solve(std::span<CoalesceBody> bodies,
                            std::span<const CoalesceTie> ties) const noexcept {
  for (auto &body : bodies) {
    body.position = body.home;
  }
  if (bodies.empty()) {
    return;
  }
  try {
    levelHeights(bodies, ties);
    settle(bodies, ties, physics_, page_);
    separate(bodies, page_.coalesceGap);
  } catch (const std::exception &error) {
    // Only an allocation can fail here; the row is a layout that is wrong
    // only in leaving the link apart.
    GLEDITOR_LOG_WARN(kLayoutCategory, "coalesce left pages home: {}",
                      error.what());
    for (auto &body : bodies) {
      body.position = body.home;
    }
  }
}

} // namespace xanadu::view
