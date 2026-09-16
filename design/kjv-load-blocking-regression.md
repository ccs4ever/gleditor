# `kjv.txt` load regression: unbounded page-build batches block the main thread

**Status: Tier 1 (the plan below) is fully implemented, including its regression test.** The fix is
in `Doc::buildPendingPages()` (`src/doc.cpp`), `render::kPageBuildFrameBudget`
(`include/gleditor/render/constants.hpp`), and the updated contract comment on
`Doc::buildPendingPages()` (`include/gleditor/doc.hpp`). Verified against the exact reproduction
below, including the worst case (the entire 1261-page document already shaped before the first
call): every `buildPendingPages()` call now stays capped at roughly the configured budget regardless
of backlog size, `./tools/compare-backends.sh`'s determinism check still passes (`opengles` vs
`opengl` stays byte-identical), and the full `gleditor_test` suite shows no new failures (three
pre-existing, unrelated failures -- `MediaWidgetTest.DeviceReadyAndDrawFrame`,
`MediaWidgetSpeedTest.CyclePlaybackRatesByPicking`,
`MediaWidgetSpeedTest.AccessibilityPerformActionOnSpeedButton` -- reproduce identically on unpatched
`HEAD` and are unrelated to page building). `tests/lib/doc_page_budget_test.cpp` is the regression
test Step 3 called for; it was confirmed to actually fail against the unpatched logic before the fix
was restored (see Step 3, below).

**Tier 2 is implemented as viewport-aware catch-up, not as true lazy/sparse virtualization** — see
"Tier 2: viewport-aware catch-up (implemented)" below for why, and what the road not taken would
have cost. Its prerequisite, promoting the Layoutfilade from `apps/common/xanadu/enfilade/` into the
core library as `gleditor::enfilade::Layoutfilade` (`include/gleditor/enfilade/layoutfilade.hpp`,
`src/enfilade/layoutfilade.cpp`) with every incidental Xanadu/zigzag reference removed, is done and
is what `Doc` now actually depends on. `Doc::buildBudgetForThisCall()` (`src/doc.cpp`) and
`render::kPageBuildCatchUpMultiplier` (`include/gleditor/render/constants.hpp`) are the feature
itself, covered by `DocPageBudgetTest.CatchesUpFasterWhenTheCameraIsAheadOfBuildProgress`
(`tests/lib/doc_page_budget_test.cpp`), confirmed to fail with the multiplier neutralized to 1
before being restored to its real value.

## Summary

Opening `tests/samples/kjv.txt` (4.4 MB, ~31k lines, ~1261-1352 pages once paginated) used to show
its first page in well under a second and finish loading the rest of the document in the background
without the UI ever freezing. It now can take tens of seconds with the UI fully unresponsive for the
whole span.

**Root cause:** `Doc::buildPendingPages()` (`src/doc.cpp`), which the render loop calls once per
frame while a document is still loading, has no cap on how much work it does per call. It always
builds *every* page the background shaping thread has produced since the last call, synchronously,
on the render thread, with no opportunity to poll SDL events or present a frame in between. Under
normal conditions the background thread and the render loop happen to stay in lockstep (roughly one
page shaped per frame), so this is invisible. But the moment the render thread's own startup or a
slow frame lets the background thread get ahead — slower GPU/driver pipeline setup, a Vulkan
swapchain, cold shader compilation, disk I/O for the file, or simply a faster CPU that shapes pages
quicker than the render thread can keep up — the *entire* backlog gets built in one call. For a
document the size of `kjv.txt`, that backlog can be the whole 1200+-page document, which is exactly
the "tens of seconds, fully blocked" symptom.

This is not a one-line logic bug introduced by a single commit; it's a latent architectural gap that
has existed in some form since page-building was first split from shaping onto separate threads, and
it manifests probabilistically depending on the relative speed of the CPU shaping thread vs. the
render thread's early frames. See "Why this isn't a clean bisect" below.

## How this was verified

Reproduced directly, not just reasoned about, using the existing `--profile` flag and
`SDL_VIDEODRIVER=offscreen` (per this repo's headless-testing rule):

```sh
make -j$(nproc) gleditor
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  XDG_DATA_HOME=$PWD/build/xdg/data XDG_CONFIG_HOME=$PWD/build/xdg/config \
  ./build/gleditor --backend opengl --profile tests/samples/kjv.txt
```

1. **Instrumented `Doc::buildPendingPages()`** to print how many pages it built and how long each
   call took. Under normal conditions on this dev machine, batches were small and steady (9-10 pages
   every call, ~3ms each) — first page at ~340-450ms, full document settled at ~8.4s, spread evenly
   across ~900 calls. No blocking visible here.

1. **Injected an artificial startup delay** (an environment-variable-gated `sleep` placed right
   after the document's background shaping thread is kicked off, but before the render loop's first
   `buildPendingPages()` call — simulating a render thread that takes longer to become ready, the
   way a Vulkan swapchain/pipeline or a slower GPU driver would) and reran:

   ```
   GLEDITOR_DEBUG_STARTUP_DELAY_MS=2000 ./build/gleditor --backend opengl --profile tests/samples/kjv.txt
   ```

   Result: the **first** `buildPendingPages()` call built **341 pages in a single 121ms call**
   (followed by a second call building another 32), and "first page rendered" jumped from ~400ms to
   **2438ms** — confirming the mechanism: whatever backlog has accumulated by the time the render
   loop first asks gets built in one shot, with no yielding.

1. **Checked out the commit immediately before the last architectural change to this code path**
   (`ae924c0`, "enforce deterministic page building and glyph-atlas packing order across backends",
   which moved page-building from individual closures pushed onto the shared render queue to the
   current `Doc`-owned `pendingShapings` + `buildPendingPages()` scheme) in a worktree, applied the
   same instrumentation and the same artificial delay, and reproduced **the identical burst**: a
   single `renderQueue` drain built 371 items in 137ms, and "first page rendered" also jumped to
   ~2.4s. Under normal (no-delay) conditions, the old code's timings (449ms first page, 8.42s
   settle) were statistically indistinguishable from current `HEAD`'s.

**Conclusion of the reproduction:** the unbounded-batch hazard is architectural and predates
`ae924c0`; that commit changed its shape (single-threaded, ordered `buildPendingPages()` instead of
a shared, cross-thread render queue) but not its presence. It is real, it is directly reproducible,
and it is the mechanism that explains the reported symptom.

## Why this isn't a clean bisect

Under this repo's headless test rig (software Mesa / offscreen SDL, this container's CPU), the bug
is *latent*: the render thread's own startup is fast enough, and CPU shaping is slow enough relative
to it, that the background thread never gets more than about one frame's worth ahead. Both the
pre-`ae924c0` code and current `HEAD` behave nearly identically here (~400ms first page, ~8.4s
settle) — no regression is visible from timing alone on this machine.

What almost certainly changed is not this code path's logic but the **relative timing** around it:
anything that makes the render thread's own early-frame or startup work heavier (additional pipeline
state, a heavier backend like Vulkan with real swapchain/descriptor setup instead of this
container's fast integrated-driver path, a slower disk read of the 4.4 MB sample, general system
load) widens the window in which the background shaping thread can race ahead — and once that window
is wide enough to cover a meaningful fraction of a 1200+-page document, the burst this design always
risked becomes the whole document instead of an imperceptible handful of pages. That is consistent
with real hardware (a real Vulkan device, a real windowing system, possibly a slower disk) showing
this far worse than a headless software-render container does. A `git bisect` driven purely by
timing on this machine is unlikely to land cleanly on a single culprit commit; the fix belongs in
`buildPendingPages()` regardless of which commit first made the window wide enough to matter.

## Where the cost actually goes

Per-page work done inside `Doc::buildPendingPages()`'s loop (`src/doc.cpp`, `Page::Page`
constructor) is genuinely GPU-touching, not just bookkeeping — it is right that this must run on the
render thread, and right that `ae924c0` made it run in strict document order (see below):

- `state.glyphCache.put(...)` per glyph (`src/doc.cpp` inside the `Page` constructor) — inserts into
  the shared glyph atlas texture, and can trigger `GlyphCache::reallocate()`
  (`src/glyphcache/cache.cpp`), which **doubles the atlas texture size or layer count and copies the
  whole atlas** when a glyph doesn't fit. A burst that inserts a huge number of previously-unseen
  glyphs in one call can trigger several of these reallocations back to back, each one a real GPU
  stall — this is a multiplier on top of the raw per-glyph cost, not a separate bug, but worth
  knowing about when picking a per-call budget (see the plan).
- `pages.emplace_back(...)` (`Page::Page`) also builds the page's VBO row data and allocates from
  the document's `BufferPool`.

None of this is cheap enough to do 1200+ times in a single call without blocking for a long time; it
was never designed to be — it was designed to be done a few pages at a time, once per frame.

## Doesn't frustum culling, or the Layoutfilade, already prevent this?

Both exist in this codebase and both are relevant prior art, but neither reaches the code path
responsible for the burst.

**Frustum culling only decides what gets *drawn*, not what gets *built*.** `Page::collect()`
(`src/doc.cpp`, called every frame from `Doc::collect()`) checks `outsideFrustum()`
(`include/gleditor/draw_budget.hpp`) and skips adding an off-screen page's draw call to that frame's
batch list. That check runs on a `Page` object that already exists — GPU buffer allocated, glyphs
already inserted into the atlas via `state.glyphCache.put()`, VBO rows already built. It has no way
to see a page that hasn't been constructed yet, and no say over whether `buildPendingPages()`
constructs it. The burst happens entirely upstream of any culling decision: every page in the
document gets shaped and GPU-uploaded regardless of whether the viewport will ever show it. Culling
here is strictly a per-frame draw-call optimization, not a load-time one.

**The Layoutfilade is real, and it was built to solve exactly this class of problem** —
`design/enfilade/layoutfilade-virtualized-scrolling.md` opens by naming "Quadratic Full-Document
Shaping" on documents like a million-line file as problem #1, and gives true O(log N) virtualized
scrolling: screen-coordinate ↔ (line, byte) mapping over a 2D B-enfilade, so only the visible window
(plus margin) needs to be shaped at all. That is the right shape of fix for this class of bug. At
the time this report was first written it lived in
`apps/common/xanadu/enfilade/layoutfilade.{hpp,cpp}`, which `AGENTS.md`'s project-structure notes
describe as part of the xanalogical engine shared by `apps/xudu` and `apps/zigzag`, with **no
connection to `apps/gleditor`** — confirmed by grepping `src/doc.cpp` and `include/gleditor/doc.hpp`
for `layoutfilade`/`enfilade`/`xanadu` and finding nothing. That specific gap is now closed: the
Layoutfilade (and the generic `CrumNode` enfilade primitives it's built on) has since been promoted
into the core library as `include/gleditor/enfilade/layoutfilade.hpp` /
`src/enfilade/layoutfilade.cpp`, namespace `gleditor::enfilade`, with every incidental reference to
`xanadu`/`xudu`/`zigzag` removed from the code itself (it always only operated on generic text lines
and `gleditor::LayoutBox`, so nothing about its actual logic needed to change) —
`apps/xudu/core/layoutfilade.hpp` is now a one-line forwarding header into the promoted library
location, the same pattern this repo already uses for `xudu`/`zigzag`'s other core shims. **So,
concretely: plain `gleditor` still eagerly shapes and GPU-uploads the entire document up front,
regardless of viewport, every time.** For `kjv.txt`'s ~1200 pages, that means building all ~1200
pages is *always* the amount of work `buildPendingPages()` eventually has to do. Culling only ever
hid this by not drawing the results, and the "viewport-aware catch-up" section below only changes
the *order* that work happens in, not the total amount of it — see that section for why.

This changes what "the fix" should mean, and splits it into two tiers worth keeping distinct:

- **Tier 1 (this report's original plan, below): stop the block.** Cap `buildPendingPages()`'s
  per-call work so the eager, whole-document build can never again freeze the main thread for
  seconds at a time. Small, safe, bounded in scope, fixes the reported symptom (UI responsiveness)
  regardless of whether the underlying eagerness is ever addressed.
- **Tier 2: make `buildPendingPages()` aware of where the camera actually is**, so a document loads
  toward wherever the user scrolled rather than strictly top-to-bottom. Two designs were on the
  table for this — see "Tier 2: viewport-aware catch-up (implemented)" below for both and why the
  smaller one was built.

Both are implemented; see the status note at the top of this report.

## Tier 2: viewport-aware catch-up (implemented)

Two designs were considered for making `buildPendingPages()` aware of where the camera is, and only
the smaller one was built. Both start from the same finding, which is worth stating precisely
because it is what ruled the bigger one out for now.

**The finding**: `Doc::pages` is a strictly ordered, append-only `std::vector<Page>`. A page's
build-order, its index into that vector, and its true position in the document are always identical
today. Two other things depend on that being true:

- **Picking** (`src/renderer.cpp`, `Doc::offsetForPick()`) bakes a page's index into its GPU picking
  tag at construction time and resolves clicks back through it — click-to-caret-position is wrong
  the moment a page's baked-in index stops matching its true document position.
- **Reflow** (`Doc::reflowFrom()`, `src/doc.cpp`) walks forward from the edited page and assumes
  every page before it already exists, re-stacking `pages[firstPage..]` in place.

Both are fine as long as pages arrive in order, which is exactly what makes them fragile to a design
that does not.

**Option A — true lazy/sparse virtualization.** Never build pages outside the viewport (plus a
margin); jump-build wherever the camera looks regardless of document position; evict and rebuild as
the user scrolls elsewhere. This is what "viewport-driven" suggested at first read, and it is a
materially bigger undertaking than it sounds: `pages` has to become gap-tolerant (a
`std::map<std::uint32_t, Page>` rather than a vector, since the total page count of a document isn't
known without walking all of it — see `design/enfilade/layoutfilade-virtualized-scrolling.md`'s own
"Heuristic Byte Windows" problem statement), a page's picking tag has to carry its *true* index
rather than its build-order position, `reflowFrom()` needs a story for editing a page that has not
been built yet (force-build the pages before it, most plausibly, since there is no way to click into
an unbuilt page to begin editing it in the first place — but that is a real design decision, not a
detail), and accessibility's page walk (`src/a11y/documents.cpp`), which already tolerates trailing
pages not existing yet, would need to keep tolerating *interior* gaps too. None of this is
impossible, but it is a genuinely separate feature with real correctness surface area in
accessibility and click-to-place behavior, not a variation on the fix above.

**Option B — viewport-aware catch-up (what was built).** Pages still build strictly in document
order — `pages` is untouched, picking and reflow are untouched, nothing above applies — but
`Doc::buildPendingPages()` (`src/doc.cpp`) can now tell when the camera is looking at a page well
past what has been built so far, and spends more of its per-call time budget catching up toward it
when that's true, via `render::kPageBuildCatchUpMultiplier`
(`include/gleditor/render/constants.hpp`). Concretely:

- `Doc::makePages()` now also appends each page's height to `pageHeightsPx` as it shapes it
  (`src/doc.cpp`) — independent of `pendingShapings`, which `buildPendingPages()` drains, so this
  keeps growing regardless of GPU-build progress.
- `Doc::buildBudgetForThisCall()` (`src/doc.cpp`) rebuilds a `gleditor::enfilade::Layoutfilade`
  (`pageIndexFilade`) from `pageHeightsPx` whenever it has grown, giving an O(log N) "which page
  index is at this Y" query over every page shaped so far — this is the actual use of the promoted
  Layoutfilade the rest of this report set up.
- It reads the camera's world position (`AppState::view.pos`, the one shared free-moving 3D camera —
  there is no per-document scroll offset in this renderer) and projects it into this document's own
  stacking coordinate (relative to `Doc::currentPosition()`, in the same `pixelsToWorld`-scaled
  units `buildPendingPages()` already stacks pages in). Camera x/z and view direction are
  deliberately not considered — a document the camera isn't actually looking at just catches up for
  no visual benefit, which costs nothing else this function's own per-call budget doesn't already
  bound.
- If the resulting target page index is past `pages.size()`, the call gets
  `render::kPageBuildFrameBudget * render::kPageBuildCatchUpMultiplier` instead of the plain budget
  — still one bounded, yielding call per frame, just a bigger one while there is somewhere specific
  to catch up to.

This does not reduce total work (every page still gets built eventually, same as Tier 1 alone) and
it does not skip building pages nobody will ever scroll to — both real limitations relative to
Option A. What it does fix, without touching picking, reflow, or accessibility at all: a document
that is still loading top-to-bottom no longer makes the user wait behind every page before the one
they actually scrolled to.

**Test**: `DocPageBudgetTest.CatchesUpFasterWhenTheCameraIsAheadOfBuildProgress`
(`tests/lib/doc_page_budget_test.cpp`) establishes a plain-budget baseline on one call, moves the
camera deep into the document's stacking direction, and asserts the very next call on the *same*
document builds noticeably more pages than that baseline — same document, same warmed-up glyph cache
and font lookups both times, isolating the catch-up multiplier as the only variable between the two
counts (an earlier version of this test compared two *separate* documents and passed even with the
multiplier neutralized to 1, because the second document's build was benefiting from the first one's
already-warm shared glyph cache rather than from catch-up at all — worth knowing if this test is
ever touched again). Confirmed to fail with `kPageBuildCatchUpMultiplier` temporarily set to `1`
before being restored to its real value.

## Why the fix must not just revert `ae924c0`

`ae924c0`'s stated purpose — building pages in a single, deterministic order on the render thread
instead of via `renderer->run()` closures raced across multiple documents' background threads onto a
shared queue — is a real fix for a real bug (nondeterministic glyph-atlas packing order across runs
and backends, which `tools/compare-backends.sh`'s atlas-growth check exists to catch). **Any fix
here must keep page-building single-threaded, sequential, and doc-ordered.** The fix is to bound how
much of that sequential work happens per call, not to go back to firing it off piecemeal from
multiple threads.

## Implementation plan

Target file: `src/doc.cpp` (`Doc::buildPendingPages()`, roughly lines 1192-1235 as of this report).
Nothing else needs to change — `include/gleditor/doc.hpp`'s `pendingShapings`/`shapingMutex`/
`shapingComplete` members, `src/renderer.cpp`'s per-frame call site (`doc->buildPendingPages(state)`
inside the `for (auto &doc : state.docs)` loop), and the `isFullyLoaded()`/`docsLoading`/`settled`
logic downstream of it are all already written to tolerate `buildPendingPages()` taking more than
one frame to finish — that's the whole point of it returning `bool` and being called every frame
until it returns `true`. Capping the batch size does not change any of that contract.

### Step 1 — cap how much `buildPendingPages()` builds per call

Replace the unconditional `toBuild.swap(pendingShapings)` (which takes *everything* pending) with a
bounded take. A **time budget** is the right primitive here, not a fixed page count: per-page cost
varies a lot (glyph count on the page, whether this call happens to trigger an atlas
`reallocate()`), so a fixed count of, say, 16 pages could still blow well past a frame budget on a
page that triggers a reallocation, or be needlessly conservative on plain short pages. Sketch:

```cpp
bool Doc::buildPendingPages(RenderState &state) {
  if (fullyLoaded) {
    return true;
  }

  // A page or two of headroom below a 60Hz frame (~16.7ms), leaving room for
  // everything else the render loop does the same frame (drawing, event
  // pump, present). Always build at least one page even if it alone blows
  // the budget, so a single expensive page (e.g. one that grows the glyph
  // atlas) can't stall progress forever.
  constexpr auto kFrameBudget = std::chrono::milliseconds(8);

  std::vector<PendingShaping> toBuild;
  {
    std::lock_guard lock(shapingMutex);
    toBuild.swap(pendingShapings);
  }

  constexpr float pageGapPx = 32.0F;
  float currentTopY = /* ...unchanged... */;

  const auto callStart = std::chrono::steady_clock::now();
  std::size_t built = 0;
  for (auto &[shaping, textOffset] : toBuild) {
    // ...unchanged per-page body...
    ++built;
    if (built >= 1 &&
        std::chrono::steady_clock::now() - callStart >= kFrameBudget) {
      break;
    }
  }

  // Anything left over goes back to the front of the queue, ahead of
  // whatever the background thread has appended in the meantime, so page
  // order stays exactly the order layoutFrom() produced it in.
  if (built < toBuild.size()) {
    std::lock_guard lock(shapingMutex);
    pendingShapings.insert(
        pendingShapings.begin(),
        std::make_move_iterator(toBuild.begin() + static_cast<std::ptrdiff_t>(built)),
        std::make_move_iterator(toBuild.end()));
    return false;
  }

  if (shapingComplete.load(std::memory_order_acquire)) {
    std::lock_guard lock(shapingMutex);
    if (pendingShapings.empty()) {
      pool->trim();
      fullyLoaded = true;
      return true;
    }
  }
  return false;
}
```

Notes for whoever implements this:

- **Order matters.** `pendingShapings` must stay in document order end-to-end, because
  `buildPendingPages()`'s vertical-stacking math (`currentTopY` computed from `pages.back()`) and
  the glyph-atlas-packing determinism `ae924c0` was written to guarantee both depend on it. The
  put-the-leftover-back-at-the-front step above is what preserves that — don't append it to the
  back, and don't let the background thread's
  `std::lock_guard lock(shapingMutex); pendingShapings.push_back(...)` in `Doc::makePages()` race
  ahead of leftovers being reinserted (the mutex already serializes this; just don't reorder the two
  operations).
- **Always build at least one page per call**, even if that single page's cost alone exceeds the
  budget (the `built >= 1 &&` guard above). Otherwise a pathological single page (e.g. one that
  triggers `GlyphCache::reallocate()`) could make the loop check the clock before doing any work and
  bail with zero progress, which would spin the render loop doing nothing every frame.
- **8ms is a starting point, not a measured constant** — this codebase's convention
  (`.agents/rules/architectural_governance.md`) is that magic numbers need a rationale and, ideally,
  data from a probe tool. Whoever implements this should sanity-check the budget against
  `tools/layout-latency-probe.cpp` or a similar quick measurement rather than trusting the number
  above outright; the reasoning is "leave enough of a 16.7ms/60Hz frame for everything else the loop
  does," but the actual number should come from measuring one representative page build on hardware
  believed to be representative, not asserted from theory.

### Step 2 — verify the fix closes the reproduction

Rerun the exact repro from "How this was verified" above (the `GLEDITOR_DEBUG_STARTUP_DELAY_MS`
env-var-gated sleep is not meant to ship — add it temporarily the same way, in `src/renderer.cpp`
right after the render loop's initial `renderQueue` drain, verify batches now stay capped even after
a large artificial head start, then remove it before committing). Confirm:

- No single `buildPendingPages()` call builds more than a small, budget-sized batch, even with
  `GLEDITOR_DEBUG_STARTUP_DELAY_MS=2000` (or larger) set.
- "First page rendered" in `--profile` output stays fast (comparable to the ~340-450ms baseline
  measured above) regardless of the artificial delay, since the first call now only builds a bounded
  batch rather than however much has backlogged.
- `./tools/compare-backends.sh` still passes, in particular its atlas-growth check — this is the
  regression test for the thing `ae924c0` fixed, and the leftover-goes-back-to-the-front reinsertion
  above must not reintroduce out-of-order glyph insertion.

### Step 3 — add a regression test (done)

`tests/lib/doc_page_budget_test.cpp` covers exactly the property the reproduction above demonstrated
by hand. `DocPageBudgetTest` builds a `Doc` (over a `MemoryTextSource`, mocked `RenderDevice`, and a
real but window-less `Renderer`/`RenderState` — the same mocking pattern `tests/lib/glyph_cache.cpp`
and `tests/lib/media_widget_test.cpp` already use for `GlyphCache`/`BufferPool`-touching code) large
enough to paginate into several hundred pages, then calls `makePages()` straight through
synchronously *before* ever calling `buildPendingPages()` once — deterministically reproducing the
race the bug depends on, rather than depending on real thread timing. It then asserts:

- The first `buildPendingPages()` call returns `false` (not fully loaded in one call) and takes
  nowhere near what building the whole backlog in one call would cost (a generous 20x-budget
  ceiling, since real per-page cost varies and the call always builds at least one page even if it
  alone exceeds the budget).
- Repeated calls make forward progress and eventually reach `isFullyLoaded() == true`, taking more
  than one call to get there — proving the backlog was genuinely spread across frames, not just fast
  enough by coincidence.

Verified the test actually catches the regression it's named for: temporarily disabling the budget
check in `buildPendingPages()` (`if (false && ...)`) makes it fail exactly as expected (whole
100-page backlog built in the first call, zero further progress after) before restoring the real
fix, which makes it pass again. `pages` is a private member with no accessor for external order
verification, so this test doesn't re-check per-page ordering directly — that property is covered
end-to-end by `tools/compare-backends.sh`'s existing `opengles` vs `opengl` byte-identical
comparison, which is sensitive to exactly this (see "Why the fix must not just revert `ae924c0`"
above).

### Out of scope for this fix

- Throttling the **background CPU shaping thread** (`Doc::makePages()`) itself is not needed. It
  racing ahead of the render thread isn't the problem — it's cheap (a `PageShaping` per page,
  appended to a vector under a mutex) and the "Height-Budgeted Slicing" design already bounds its
  per-page cost (see `AGENTS.md`'s Text Architecture section and `src/text/layout.cpp`'s
  `layoutPage()`). The only thing that needs bounding is how much of the *accumulated* backlog
  `buildPendingPages()` is allowed to drain in one render-thread call.
- This report doesn't attempt to identify a single "regressing" commit, because the reproduction
  above shows the hazard predates the most recent architectural change to this code (`ae924c0`) and
  reproduces identically on both sides of it under the same synthetic conditions. Time spent trying
  to bisect to one commit on this machine is unlikely to find one; the fix in `buildPendingPages()`
  is the actionable next step regardless.
