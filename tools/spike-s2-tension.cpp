// Spike S2 (throwaway, not for merge): does a fresh TensionLayoutEngine,
// cold-started and stepped a fixed 25 times, reproduce what LinkBeams does
// today?  Reproduces the engine calls of LinkBeams::alignPair
// (apps/common/ui/xanadoc/beams.cpp, the docSlots loop and the block from
// tensionEngine_.clear() to the docSlots read-back) over a device-free model of
// RenderState::docs, and compares it with pure functions of the catalog.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <glm/geometric.hpp>

#include "common/xanadu/tension_layout.hpp"

using namespace xanadu;

namespace {

// Values taken from the code, not chosen: Doc::pixelsToWorld = 1/18,
// letterPage = 139.7 dpi * 8.5in + 48 by 11in + 48, kDefaultDocumentGap = 24,
// alreadyAligned = 1, dt = 0.016 and 25 steps in alignPair.
constexpr float pxToWorld  = 1.0F / 18.0F;
constexpr float letterW    = ((139.70F * 8.5F) + 48.0F) * pxToWorld;
constexpr float letterH    = ((139.70F * 11.0F) + 48.0F) * pxToWorld;
constexpr float docGap     = 24.0F;
constexpr float aligned    = 1.0F;
constexpr float dt         = 0.016F;
constexpr int todaySteps   = 25;
constexpr float tolPx      = 2.0F; // page.base.levelTolerance (spec §12)
constexpr int settleCap    = 20000;

struct Doc {
  glm::vec3 dest; // getModel()[3]: the destination animateMoveTo() recorded
  float halfW{letterW * 0.5F};
  float height{letterH};
};

struct Link {
  std::size_t from, to;
  float nearAnchorY, farAnchorY; // anchor centroid Y relative to doc origin
};

struct Result {
  std::vector<float> x; // docSlots after the engine
  int stepsToSettle{-1};
  float maxSpeedAt25{0};
};

// The docSlots loop of alignPair, beams.cpp:932-957.
std::vector<float> rowSlots(const std::vector<Doc> &docs, std::size_t far) {
  const auto bg = [&](std::size_t d) { return d != far && docs[d].dest.z < 0; };
  std::vector<float> slots(docs.size(), 0.0F);
  float currX  = 0.0F;
  bool anyYet  = false;
  for (std::size_t d = 0; d < docs.size(); ++d) {
    if (bg(d)) {
      slots[d] = docs[d].dest.x;
      continue;
    }
    if (anyYet) {
      float prevHalfW = letterW * 0.5F;
      for (std::size_t prev = d; prev-- > 0;) {
        if (bg(prev)) continue;
        prevHalfW = docs[prev].halfW;
        break;
      }
      currX += prevHalfW + docs[d].halfW + docGap;
    }
    anyYet   = true;
    slots[d] = currX;
  }
  return slots;
}

// beams.cpp:959-1005, generalised to several constraints for the one-shot case.
// `steps` < 0 means "until settled or settleCap".
Result solve(const std::vector<Doc> &docs, const std::vector<Link> &links,
             std::size_t far, int steps, TensionParams params = {}) {
  const auto bg = [&](std::size_t d) { return d != far && docs[d].dest.z < 0; };
  TensionLayoutEngine engine(params);
  engine.clear();
  for (std::size_t d = 0; d < docs.size(); ++d) {
    TensionBody b;
    b.docIndex        = d;
    b.position        = docs[d].dest;
    b.restingPosition = docs[d].dest;
    b.width           = docs[d].halfW * 2.0F;
    b.height          = docs[d].height;
    b.isForeground    = !bg(d);
    b.isFlying        = (d == far && docs[far].dest.z < -5.0F);
    b.pinned          = (d == links.front().from);
    engine.setBody(b);
  }
  for (const auto &l : links) {
    TensionConstraint c;
    c.fromDoc     = l.from;
    c.toDoc       = l.to;
    c.nearAnchorY = l.nearAnchorY;
    c.farAnchorY  = l.farAnchorY;
    c.targetGap   = docGap;
    c.prominence  = 1.0F;
    c.active      = true;
    engine.addConstraint(c);
  }
  Result r;
  auto slots = rowSlots(docs, far);
  // Settled = the step after the last one at which any body still moved
  // faster than the threshold (a first dip below it can be a zero crossing).
  const int total = steps < 0 ? settleCap : std::max(steps, todaySteps);
  int lastMoving  = -1;
  std::vector<glm::vec3> keep;
  for (int n = 0; n < total; ++n) {
    engine.step(dt);
    if (!engine.isSettled()) lastMoving = n;
    if (n + 1 == todaySteps)
      for (const auto &b : engine.bodies())
        r.maxSpeedAt25 = std::max(r.maxSpeedAt25, glm::length(b.velocity));
    if (n + 1 == steps)
      for (const auto &b : engine.bodies()) keep.push_back(b.position);
  }
  r.stepsToSettle = lastMoving + 1;
  if (steps >= 0)
    for (std::size_t i = 0; i < keep.size(); ++i)
      engine.bodies()[i].position = keep[i];
  for (std::size_t d = 0; d < docs.size(); ++d)
    if (!bg(d)) slots[d] = engine.findBody(d)->position.x;
  r.x = slots;
  return r;
}

// One alignPair: solve, then commit destinations the way beams.cpp:1009-1056
// does (far gets x, levelled y, z=0; the rest keep y,z and take x).
void commit(std::vector<Doc> &docs, const Link &l, const std::vector<float> &x) {
  const auto bg = [&](std::size_t d) { return d != l.to && docs[d].dest.z < 0; };
  const float deltaY = l.nearAnchorY + docs[l.from].dest.y -
                       (l.farAnchorY + docs[l.to].dest.y);
  const glm::vec3 target{x[l.to], docs[l.to].dest.y + deltaY, 0.0F};
  if (glm::distance(target, docs[l.to].dest) >= aligned) docs[l.to].dest = target;
  for (std::size_t d = 0; d < docs.size(); ++d) {
    if (d == l.to || bg(d)) continue;
    if (std::abs(docs[d].dest.x - x[d]) >= aligned) docs[d].dest.x = x[d];
  }
}

std::vector<Doc> homeRow(std::size_t n, std::vector<std::size_t> background = {},
                         std::vector<float> halfWs = {}) {
  // Renderer::openDoc: first foreground at 0, each next at lastRight + gap +
  // default half width; background docs at (0,0,depth).
  std::vector<Doc> docs(n);
  float lastRight = 0;
  bool any        = false;
  for (std::size_t i = 0; i < n; ++i) {
    if (!halfWs.empty()) docs[i].halfW = halfWs[i];
    if (std::ranges::contains(background, i)) {
      docs[i].dest = {0, 0, -40};
      continue;
    }
    // openDoc places with the *default* half width; the page's real width
    // arrives later, which is the first source of drift checked below.
    docs[i].dest = {any ? lastRight + docGap + letterW * 0.5F : 0.0F, 0, 0};
    lastRight    = docs[i].dest.x + docs[i].halfW;
    any          = true;
  }
  return docs;
}

float maxDevPx(const std::vector<float> &a, const std::vector<float> &b) {
  float m = 0;
  for (std::size_t i = 0; i < a.size(); ++i)
    m = std::max(m, std::abs(a[i] - b[i]));
  return m / pxToWorld;
}

std::string fmt(const std::vector<float> &v) {
  std::string s;
  for (float f : v) s += std::to_string(f).substr(0, 7) + " ";
  return s;
}

void report(const char *name, float devPx, const char *what) {
  std::printf("  %-44s %10.3f px  %s  (%s)\n", name, devPx,
              devPx <= tolPx ? "PASS" : "FAIL", what);
}

// --- Part 1: the scenarios of tests/xudu/tension_layout_test.cpp ----------
void testScenarios() {
  std::puts("== tension_layout_test scenarios: 25 steps vs the test's count ==");
  struct S {
    const char *name;
    TensionParams p;
    std::vector<TensionBody> bodies;
    std::vector<TensionConstraint> cons;
    int testSteps;
  };
  std::vector<S> ss;
  {
    TensionParams p;
    p.kDamping = 12; p.kPlane = 25; p.settleVelocityThreshold = 0.01F;
    TensionBody b; b.docIndex = 0; b.position = {0, 0, 20};
    b.velocity = {10, -5, 15};
    ss.push_back({"RK4DampedConvergence", p, {b}, {}, 250});
  }
  {
    TensionParams p; p.kRepel = 6000; p.kDamping = 10; p.defaultGap = 8;
    TensionBody b1; b1.docIndex = 1; b1.width = 40; b1.height = 60;
    TensionBody b2 = b1; b2.docIndex = 2; b2.position = {5, 0, 0};
    ss.push_back({"CoulombRepulsionPreventsOverlap", p, {b1, b2}, {}, 250});
  }
  {
    TensionParams p; p.kAlign = 35; p.kDamping = 12; p.kPlane = 20;
    p.defaultGap = 8;
    TensionBody n; n.docIndex = 10; n.width = 50; n.height = 70; n.pinned = true;
    TensionBody f; f.docIndex = 20; f.position = {120, 50, -40}; f.width = 50;
    f.height = 70; f.isFlying = true;
    TensionConstraint c; c.fromDoc = 10; c.toDoc = 20; c.nearAnchorY = 10;
    c.farAnchorY = -10; c.targetGap = 8; c.active = true;
    ss.push_back({"CollinearAlignmentSpring", p, {n, f}, {c}, 300});
  }
  for (auto &s : ss) {
    const auto run = [&](int steps, int *settled) {
      TensionLayoutEngine e(s.p);
      for (auto &b : s.bodies) e.setBody(b);
      for (auto &c : s.cons) e.addConstraint(c);
      int k = 0;
      for (; k < steps; ++k) {
        e.step(dt);
        if (settled && !e.isSettled()) *settled = k + 1;
      }
      std::vector<glm::vec3> pos;
      for (auto &b : e.bodies()) pos.push_back(b.position);
      return pos;
    };
    int settled = 0;
    const auto at25   = run(todaySteps, nullptr);
    const auto atTest = run(s.testSteps, nullptr);
    run(settleCap, &settled);
    // Re-run cold to show it is a pure function: identical bits.
    const auto again = run(todaySteps, nullptr);
    float dev = 0;
    bool same = true;
    for (std::size_t i = 0; i < at25.size(); ++i) {
      dev = std::max(dev, glm::length(at25[i] - atTest[i]));
      same = same && at25[i] == again[i];
    }
    std::printf("  %-34s settle@%5d steps (v<%.3g)  |p25-p%d|=%8.3f world "
                "= %9.2f px  cold rerun bit-identical=%s\n",
                s.name, settled, s.p.settleVelocityThreshold, s.testSteps, dev,
                dev / pxToWorld, same ? "yes" : "NO");
  }
}

// --- Part 2: alignPair, today vs pure --------------------------------------
struct Case {
  const char *name;
  std::vector<Doc> docs;
  std::vector<Link> sequence; // activations/aligns in order
};

void runCase(const Case &c) {
  std::printf("\n== %s ==\n", c.name);
  // Today: each alignPair seeds the engine from where the previous ones left
  // the destinations.
  auto today = c.docs;
  std::vector<float> lastToday;
  for (const auto &l : c.sequence) {
    auto r    = solve(today, {l}, l.to, todaySteps);
    lastToday = r.x;
    commit(today, l, r.x);
  }
  std::vector<float> todayX;
  for (auto &d : today) todayX.push_back(d.dest.x);

  // Physics off (the default; xuzz --physics turns it on): docSlots alone.
  auto off = c.docs;
  for (const auto &l : c.sequence) commit(off, l, rowSlots(off, l.to));
  std::vector<float> offX;
  for (auto &d : off) offX.push_back(d.dest.x);

  // Pure, per activation: the *last* activation solved cold from the home
  // row, ignoring history -- what P2's coalesce would be for one tie.
  const auto &last = c.sequence.back();
  auto pureDocs    = c.docs;
  auto pure25      = solve(pureDocs, {last}, last.to, todaySteps);
  commit(pureDocs, last, pure25.x);
  std::vector<float> pureX;
  for (auto &d : pureDocs) pureX.push_back(d.dest.x);

  auto settledDocs = c.docs;
  auto pureSettled = solve(settledDocs, {last}, last.to, -1);
  commit(settledDocs, last, pureSettled.x);
  std::vector<float> settledX;
  for (auto &d : settledDocs) settledX.push_back(d.dest.x);

  // Re-run of the pure function: bit-identical?
  auto again = c.docs;
  auto r2    = solve(again, {last}, last.to, todaySteps);
  const bool bitSame = r2.x == pure25.x;

  std::printf("  home x      : %s\n", [&] {
    std::vector<float> v; for (auto &d : c.docs) v.push_back(d.dest.x);
    return fmt(v); }().c_str());
  std::printf("  today(phys) : %s\n", fmt(todayX).c_str());
  std::printf("  today(off)  : %s\n", fmt(offX).c_str());
  std::printf("  pure 25     : %s\n", fmt(pureX).c_str());
  std::printf("  pure settled: %s  (settled after %d steps; max |v| at "
              "step 25 = %.3f world/s = %.0f px/s)\n",
              fmt(settledX).c_str(), pureSettled.stepsToSettle,
              pure25.maxSpeedAt25, pure25.maxSpeedAt25 / pxToWorld);
  // Steps until the cold solve stays within the tolerance of its rest.
  {
    int within = -1;
    for (int k = settleCap / 40; k >= 1; --k) {
      auto r = solve(c.docs, {last}, last.to, k);
      if (maxDevPx(r.x, pureSettled.x) > tolPx) { within = k + 1; break; }
    }
    std::printf("  steps until x stays within %.0f px of rest: %d\n", tolPx,
                within);
  }
  report("pure25 vs today(physics on)", maxDevPx(pureX, todayX),
         "the S2 question");
  report("pure25 vs today(physics off, default)", maxDevPx(pureX, offX),
         "parity with what a user sees by default");
  report("pure25 vs pure settled", maxDevPx(pureX, settledX),
         "how far from rest 25 steps leaves it");
  std::printf("  pure25 cold rerun bit-identical: %s\n", bitSame ? "yes" : "NO");
  // Overlap check (spec §10.3.4: when all fit whole, no two overlap).
  const auto overlap = [](const std::vector<Doc> &ds) {
    float worst = 0;
    for (std::size_t i = 0; i < ds.size(); ++i)
      for (std::size_t j = i + 1; j < ds.size(); ++j) {
        if (std::abs(ds[i].dest.z - ds[j].dest.z) > 1) continue;
        worst = std::max(worst, ds[i].halfW + ds[j].halfW -
                                    std::abs(ds[i].dest.x - ds[j].dest.x));
      }
    return worst / pxToWorld;
  };
  std::printf("  worst overlap of two foreground docs: pure25 %.1f px, "
              "settled %.1f px, today(phys) %.1f px\n",
              overlap(pureDocs), overlap(settledDocs), overlap(today));
}

// One-shot multi-constraint solve (what the base view's coalesce does for a
// many-ended link) against today's one-pair-per-frame chain.
void manyEndsOneShot() {
  std::puts("\n== many ends: one cold solve with all ties vs today's chain ==");
  auto docs                    = homeRow(6);
  const std::vector<Link> ties = {{0, 2, 10, 5}, {0, 4, 30, 2}, {0, 5, 50, 8}};
  auto chain                   = docs;
  for (const auto &l : ties) {
    auto r = solve(chain, {l}, l.to, todaySteps);
    commit(chain, l, r.x);
  }
  auto oneShot = solve(docs, ties, ties.front().to, todaySteps);
  auto settled = solve(docs, ties, ties.front().to, -1);
  std::vector<float> chainX;
  for (auto &d : chain) chainX.push_back(d.dest.x);
  std::printf("  today chain : %s\n", fmt(chainX).c_str());
  std::printf("  one-shot 25 : %s\n", fmt(oneShot.x).c_str());
  std::printf("  one-shot set: %s (settled after %d steps)\n",
              fmt(settled.x).c_str(), settled.stepsToSettle);
  report("one-shot25 vs today chain", maxDevPx(oneShot.x, chainX), "");
  report("one-shot25 vs one-shot settled", maxDevPx(oneShot.x, settled.x), "");
}

} // namespace

int main() {
  testScenarios();
  const float h = letterW * 0.5F;
  runCase({"two docs, link 0->1 (first activation from home)", homeRow(2),
           {{0, 1, 20, 40}}});
  runCase({"two docs, link 1->0 (far is left of near)", homeRow(2),
           {{1, 0, 20, 40}}});
  runCase({"three docs, link 0->2 (far not adjacent)", homeRow(3),
           {{0, 2, 20, 40}}});
  runCase({"three docs, link 0->1 (far adjacent)", homeRow(3),
           {{0, 1, 20, 40}}});
  runCase({"three docs, link 0->1 then 0->2 (history)", homeRow(3),
           {{0, 1, 20, 40}, {0, 2, 10, 60}}});
  runCase({"six docs, link 0->5 (many docs)", homeRow(6), {{0, 5, 10, 30}}});
  runCase({"six docs, links 0->2, 0->4, 0->5 chained (many ends)", homeRow(6),
           {{0, 2, 10, 5}, {0, 4, 30, 2}, {0, 5, 50, 8}}});
  runCase({"six docs, link 0->3 then 2->5 (history, separate links)",
           homeRow(6), {{0, 3, 10, 20}, {2, 5, 15, 25}}});
  runCase({"three docs + background corpus, link 0->3 (flies forward)",
           homeRow(4, {3}), {{0, 3, 10, 20}}});
  runCase({"three docs, mixed widths (A4-ish narrow, wide), link 0->2",
           homeRow(3, {}, {h, h * 0.7F, h * 1.4F}), {{0, 2, 20, 40}}});
  manyEndsOneShot();
}
