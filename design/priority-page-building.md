# Priority page building: viewport first, then what beams reach

Implementation plan. The groundwork it stands on (a per-page coordinate index, and a per-call build
budget) landed with the Tier 1/Tier 2 work in
[`kjv-load-blocking-regression.md`](kjv-load-blocking-regression.md), which is worth reading first —
this note assumes its vocabulary (`Doc::buildPendingPages()`, `pageIndexFilade`,
`render::kPageBuildFrameBudget`).

**Status: Stages 0-5 are all done** — see their sections below, each marked "(done)".

## Goal

Build a document's pages in priority order rather than document order:

1. **P0 — pages at or near the viewport.** What the reader is looking at should exist first.
1. **P1 — pages a beam touches, when that beam could cross the viewport.** Both ends: the page the
   beam is anchored on *and* the page it targets. A beam is a long ribbon between two points that
   may be hundreds of pages apart; its middle can sweep through the viewport while neither end is in
   it, so an anchor being off-screen says nothing about whether the beam is visible.
1. **P2 — everything else**, in document order, exactly as today.

## Why none of this works today

Four facts, each of which the plan has to move:

- **A beam cannot be drawn until both of its endpoint pages are built.** `Doc::anchorFor()`
  (`src/doc.cpp:503`) linearly scans *built* pages and asks each `Page::caretGeometry()`; an offset
  on an unbuilt page returns `std::nullopt`, which its own header comment
  (`include/gleditor/doc.hpp:801`) describes as "the answer while a document is still being built".
  `LinkBeams::resolveAnchors()` (`apps/xudu/beams.cpp:194`) is built on it.
- **So beams currently wait for *every* open document to finish.** `LinkBeams::drawFrame()`
  (`apps/xudu/beams.cpp:1169-1179`) returns early, with `unsettled = true`, unless every doc in
  `state.docs` reports `isFullyLoaded()`. **This is the gate that makes the whole feature invisible
  until it moves**: priority-building the pages a beam needs achieves nothing a user can see while
  beams refuse to draw until the last page of the last document lands.
- **Pages can only be built in document order.** `Doc::buildPendingPages()` (`src/doc.cpp:1254`)
  chains each page's Y off `pages.back()`, and passes `pages.size()` as the page's own index
  (`src/doc.cpp:1284,1292`) — so build order, vector position, and true page number are the same
  number by construction. `Doc::page(index)` (`include/gleditor/doc.hpp:746`) is
  `index < pages.size() ? &pages[index] : nullptr`, which cannot express a gap.
- **Two things depend on that.** Picking resolves a click through the page index baked into the GPU
  tag at construction, so a page whose baked index is its build position rather than its true page
  number sends the caret to the wrong offset. `Doc::reflowFrom()` (`src/doc.cpp:963`) re-stacks
  `pages[firstPage..]` and assumes every page before the edited one exists.

## The enabler that is already there

`Doc::pageIndexFilade` — a `gleditor::enfilade::Layoutfilade` rebuilt in
`Doc::buildBudgetForThisCall()` from `pageHeightsPx` — already answers **"where is page N"** in
O(log N) for every page that has been *shaped*, whether or not it has been built. That is what
breaks the circular dependency this feature would otherwise have: deciding whether to build a page
needs to know where it is, and knowing where it is currently needs it built.

It needs one extension (Stage 1): record each page's byte length in the entry alongside its height,
so `Layoutfilade::findEntryAtByte()` answers **"which page holds byte B"** as well. Both fields
already exist on `LayoutEntry`; only `heightPx` is populated today.

## Design

### The oracle

Two new `Doc` queries, both answered from the filade, both valid for any *shaped* page:

- `pageIndexForOffset(std::uint32_t offset) -> std::optional<std::uint32_t>` — via
  `findEntryAtByte()`.
- `approximateAnchorFor(std::uint32_t offset) -> std::optional<Anchor>` — page index plus that
  page's Y, with no within-page x/y and no shaping. Deliberately *not* `anchorFor()`: it is accurate
  to about a page height, which is all a "could this ribbon cross the viewport" test needs, and it
  costs a tree descent rather than a reshape.

### Getting the beam signal across the layering boundary

`.agents/rules/architectural_governance.md` §1 forbids `src/` and `include/gleditor/` from depending
on anything in `apps/`, and beams-as-links live in `apps/xudu/beams.cpp`. The library therefore
cannot ask "which pages have beams"; the application has to push it, the same way `FrameContributor`
already inverts this dependency.

So: the library offers a channel that knows nothing about beams.

```cpp
/// Byte offsets this document should build the pages for ahead of the rest,
/// after the viewport's own pages. Replaced wholesale each time it is set;
/// an empty span means "no opinion", which is what every caller that has
/// none (apps/gleditor) leaves it at. Render thread only.
void setPriorityOffsets(std::span<const std::uint32_t> offsets);
```

`LinkBeams` (xudu) is what fills it, because only it knows what a beam is: for each strand, take
both ends' offsets, resolve each to an approximate world point via `approximateAnchorFor()` +
`Doc::worldPoint()`, frustum-test the resulting ribbon (inflated by a page height, since the
endpoints are approximate), and push the offsets of the ribbons that survive. `apps/gleditor` pushes
nothing and behaves exactly as it does today.

### Out-of-order building

This is the part that was deliberately deferred in `kjv-load-blocking-regression.md` (see its
"Option A" discussion). Each of the four blockers above gets an answer:

| Blocker                                   | Answer                                                                                                                                                                                                                                                                                                                                                              |
| ----------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `pages` cannot express a gap              | `std::map<std::uint32_t, Page>` keyed by true page index. `Page` is movable (`src/doc.cpp:1045` moves one), so `std::vector<std::optional<Page>>` also works — but the map needs no total-page-count estimate to size itself, and keeps ordered iteration for `Doc::collect()` and a11y.                                                                            |
| Page Y chains off `pages.back()`          | Take it from `pageIndexFilade.findEntryByIndex(n)->startYPx` instead — a prefix sum over shaped heights, which is the same number the chain would have reached, available without the predecessor being built.                                                                                                                                                      |
| Baked page index is the build position    | Pass the true page index when constructing. One-line change once pages are built by index rather than by "next".                                                                                                                                                                                                                                                    |
| `reflowFrom()` assumes 0..firstPage exist | Guard at entry: if any page before the edited one is unbuilt, build them first (synchronously — an edit is rare and already re-shapes), then proceed. Practically an edit reaches a page by being clicked into, which requires it drawn; automation scripts (`--type` at an arbitrary offset) can reach one that is not, so the guard is correctness, not tidiness. |

`numPages()` (`include/gleditor/doc.hpp:863`) also has to split, because "how many pages exist" and
"how many are built" stop being the same number. Only three production callers, all needing the
first sense: `src/a11y/documents.cpp:58` (loop bound — already null-checks each `page(index)`, so it
tolerates interior gaps unchanged), `src/renderer.cpp:984` and `src/renderer.cpp:1045`. Keep
`numPages()` as the total and add `builtPageCount()` for progress, rather than silently changing
what the existing name means.

`pendingShapings` stops being a FIFO drained in order and becomes a pool keyed by page index, since
selection is now by priority rather than by position.

### Determinism is a hard constraint, not a nice-to-have

`ae924c0` made page building deterministic across backends on purpose, and
`tools/compare-backends.sh` checks it (`opengles` vs `opengl` must stay byte-identical). Priority
order must therefore be **a pure function of (camera, shaped-page set, pushed offsets)** with no
dependence on wall-clock or thread timing — which frame a page lands in may vary, but the *sequence*
must not.

The reassuring case is the one CI actually exercises: with a fixed camera and no beams, P0 resolves
to the pages around the default camera position and P1 is empty, so the sequence degenerates to
document order — exactly today's behaviour. That degeneration is worth an explicit test (Stage 3),
not just an argument.

## Cell endpoints, and why they are a different problem

Xanalinks and transclusion beams between a xanadoc and a cell, and between two cells, are **real,
converged, and wired in production** — not hypothetical. The convergence delivered the address:
`UniversalLinkEnd` (`apps/common/xanadu/universal_link_endpoint.hpp`) is one 16-byte endpoint that
names *either* a document byte range *or* a `CellRef` plus an intra-cell span, discriminated by
`LinkTargetKind`. And the path from a beam to a placed cell is connected end to end in the xuzz
build: `apps/xudu/main.cpp:2671-2674` (under `XUZZ_BUILD`) constructs a `BridgeCoordinator` and
attaches the zigzag presentation, which sets `links_.setCellAnchorResolver(...)`
(`apps/xudu/bridge_coordinator.cpp:81`) pointing at `ZigzagVisualizer::cellAnchor()`
(`apps/zigzag/zigzag_visualizer.cpp:1571`).

So cross-domain strands **are** in this plan's scope, in the part that matters here: a strand with
one document end and one cell end still needs its document end's page built, and Stage 0's
per-strand resolution has to treat the two ends symmetrically rather than assuming both are pages.

What this plan **cannot** do is make an unresolvable cell end resolve, and the reason is worth being
precise about, because it is not the reason a page end fails:

- A **page** end fails because the page **is not built yet**. It is in a queue; it will be built;
  priority only changes when. Waiting works, and reordering the queue is exactly what Stages 1-3 do.
- A **cell** end fails because the cell **is not in `visible_cells_`** — a neighbourhood grown
  outward from `accursed_cell_focus_` and bounded by `scene_.neighborhood_radius`
  (`apps/zigzag/zigzag_visualizer.cpp:922-951`). A cell outside that radius is never materialised,
  so `cellAnchor()` returns `std::nullopt` **permanently**, not "yet". There is no queue to reorder
  and no amount of budget that helps.

That is why it is separate work rather than another priority tier: making a beam reach an
out-of-neighbourhood cell means **materialising a cell because a link needs it**, not scheduling one
sooner.

**The shape that separate work should take** (recorded here so it has a starting point, not planned
in this note): a cell linked from a xanadoc spawns a **temporary view cursor** on that cell, which
grows its own visible neighbourhood by exactly the same radius walk the main cursor does. Not a new
kind of placement — the same kind, from a second origin. The temporary cursor lives only as long as
the link that justified it: it goes when the link changes, whether by editing or by the user moving
to a different link.

That fits what is already there better than a pinned-cell exception would. `visible_cells_` is keyed
by `CellID` (`apps/zigzag/zigzag_visualizer.cpp:804`), so two cursors whose neighbourhoods overlap
merge for free — a cell reachable from both appears once, and nothing has to reconcile two notions
of where it is. Drawing, picking and accessibility then need no special case: a link-spawned cell
sits in `visible_cells_` like any other, with a neighbourhood around it, which is exactly the
question an off-focus pinned cell could not have answered.

What it costs is that `ZigzagVisualizer` has exactly one focus today — `accursed_cell_focus_`, a
single `CellID` seeded from the slice's focus or `manifold().home()`
(`apps/zigzag/zigzag_visualizer.cpp:216-221`) — and zigzag's core carries no cursor concept at all
(nothing in `zzcore.hpp` or `zzstructure.hpp`). So the work is generalising one focus into a set: a
primary cursor plus zero or more ephemeral ones, each running the radius walk, with lifetimes tied
to the links that spawned them. Its own doc, but no longer an open-ended one.

One nearer-term consequence to design for rather than fix: `cellAnchor()` also returns `nullopt` for
a cell whose `current_alpha` and `target_alpha` are both below `0.02F`
(`apps/zigzag/zigzag_visualizer.cpp:1577`) — i.e. one mid-fade. So even an in-neighbourhood cell has
a *transient* unresolvable state, which means Stage 0's per-strand drawing will see cross-domain
strands legitimately come and go as cells fade in and out. That is the correct behaviour, not
flicker to be suppressed by waiting — but it does mean "unresolved" must be an ordinary per-frame
condition in the Stage 0 design, not an error path.

## Staged implementation

Each stage builds, tests, and ships on its own.

### Stage 0 — let beams draw before the document finishes (done)

Replaced the all-documents `isFullyLoaded()` gate in `LinkBeams::drawFrame()`
(`apps/xudu/beams.cpp`) with a per-strand condition. The gate itself is gone — `rebuildStrands()`
and `resolveAnchors()` now run unconditionally (as they already did once any strand existed), and
the existing per-strand `fromValid`/`toValid` checks (unchanged) already skip drawing and aligning a
strand until its own two ends resolve. What changed is what feeds `unsettled`: a new
`anyStrandStillLoading` flag, set only for a strand whose document endpoint is missing its anchor
*and* that document is not yet `isFullyLoaded()` — the same "don't count what will never resolve"
reasoning `danglingOutstanding()`'s own comment already states for half-links, extended here to
avoid the opposite failure (a run that never settles because it is waiting on a strand that resolved
and was simply skipped, or on a cell that will never place). Cell endpoints and endpoints whose
document is already fully loaded never count, matching what the *old* code already tolerated (it
never gated settling on cell readiness either — only on document page-build completion).

Both ends are treated the same way regardless of kind: `resolveAnchors()` already branched on
`isCell()` per end, so a cross-domain strand — one document end, one cell end — was already the case
where the two branches differ, unaffected by this change.

**Verified**: built a scenario with two small documents linked to each other (settle in well under a
second) alongside a third, 3 MB unrelated document (settles after several seconds of real page
building), driven through the real `xudu` binary. Confirmed, by rebuilding with the old gate
temporarily restored and comparing against the fix: with the old gate, the beam drew for a handful
of frames (an artifact of the background document not yet being in `state.docs` when the first
couple of frames ran) and then stopped completely until the whole 8-9 second run settled; with the
fix, the beam drew on every sampled frame continuously from ~250ms through to settling.
`tests/xudu/beam_progressive_loading_test.cpp` is the permanent regression test for this scenario:
since asserting the *timing* of when a beam first appears needs instrumentation `--profile` does not
currently report (Stage 4 is where adding that is planned), the test instead locks down what is
unit-testable today — the whole scene, camera alignment included, still settles correctly and within
a generous bound rather than hanging, which is exactly what a wrong `anyStrandStillLoading` could
break. Full `xudu_test`/`gleditor_test`/`zigzag_test` suites pass with no new failures (three
pre-existing, unrelated `AnimationTransclusionTest` failures reproduce identically on unpatched
`HEAD`).

### Stage 1 — the oracle and the channel, without reordering anything (done)

- Populate `LayoutEntry::byteLength` from `shaping.limit` when filling `pageEntries` (renamed from
  `pageHeightsPx`) in `Doc::makePages()` (`src/doc.cpp`).
- Add `Doc::pageIndexForOffset()` and `Doc::approximateAnchorFor()`, both answered from
  `pageIndexFilade` via a shared, self-refreshing `Doc::refreshPageIndexFilade() const` (needed
  `shapingMutex`/`pageIndexFilade`/`pageIndexFiladeBuiltFor` to become `mutable`, since these are
  meant to be callable before any page is built and so cannot wait for `buildBudgetForThisCall()` to
  have triggered the refresh as a side effect).
- Add `Doc::setPriorityOffsets()` and store the resulting page indices.
- Extend `Doc::buildBudgetForThisCall()` to take the furthest-behind of {camera target page,
  priority target pages} rather than the camera alone — so the existing catch-up multiplier hurries
  toward whatever a beam needs, still building in document order.
- Wire `LinkBeams` to push its ribbon-crossing offsets: `LinkBeams::updatePriorityOffsets()`
  (`apps/xudu/beams.cpp`), called from `drawFrame()` right after `resolveAnchors()`. For each strand
  and transclusion strand with at least one still-unresolved document endpoint, it resolves both
  ends to approximate world points (`approximateAnchorFor()` + `Doc::worldPoint()` for a document
  end still waiting on its page, the exact anchor's `worldPoint()` for one that has resolved,
  `CellAnchor::position` for a cell end), and calls the new static
  `LinkBeams::ribbonMaybeOnScreen()` — a box test built on `outsideFrustum()`
  (`include/gleditor/draw_budget.hpp`), centred on the segment's midpoint and inflated by one page
  height (`approximateAnchorFor()`'s own accuracy contract) in every direction to absorb the
  approximation. Offsets from strands whose ribbon survives are pushed via
  `Doc::setPriorityOffsets()`, one call per document per frame (an empty span clears a document with
  no surviving strand, so a beam that scrolls away does not leave a stale priority behind).

For a **cross-domain** strand the ribbon test has one endpoint from the filade (the document end,
approximate) and one from `cellAnchorResolver` (the cell end, exact but only if the cell is
currently placed). When the cell end does not resolve, there is no ribbon to test — treat the strand
as *possibly* crossing and push its document end anyway. Being conservative costs one page built
early; being clever costs a beam that stays missing because the page it needed was never
prioritised. `updatePriorityOffsets()` implements this as an explicit third case alongside "both
ends resolved, test the ribbon" and "neither resolved, nothing to say yet".

Real value on its own: a beam whose far end is page 900 makes the loader hurry to 900, rather than
ambling there at the plain per-call budget. No invariant changes, nothing can arrive out of order.

**Tests**: `DocPageBudgetTest` gained three cases (`tests/lib/doc_page_budget_test.cpp`) —
`PriorityOffsetFarAheadEngagesCatchUpToo` (mirrors the existing camera case, but with a pushed
offset instead of a moved camera), `PageIndexForOffsetAnswersBeforeAnyPageIsBuilt` (the oracle
answers before `buildPendingPages()` has ever run, cross-checked against `anchorFor()` once building
catches up), and `ApproximateAnchorAgreesWithAnchorOncePageIsBuilt` (the approximate and exact
anchors name the same page and land within about one page height of each other in world space).
`tests/xudu/beam_priority_offset_test.cpp` is the Stage 1 analogue of Stage 0's own binary-driven
regression test: a beam whose far end is five bytes from the end of a 1000+-page document, driven
through the real `xudu` binary, asserted to settle correctly and within a generous bound — the same
"does not hang" property Stage 0 guards, now for the priority channel rather than the settling
condition.

**Verified by hand**, the same way Stage 0 was: two documents (a small foreground one and a
1000+-page background one, `--background`, so camera auto-framing — an unrelated cost that does not
scale with page count, found while building this scenario and ruled out as out of scope — never
engages), linked at either the very start or five bytes from the very end of the large document, run
through the real `xudu --profile` binary with `--no-sworph` (isolating the priority channel from the
sworph/alignment machinery), repeated three times each with the system otherwise idle. Linking to
the end settled consistently faster than linking to the start (roughly 8.8-9.2s vs 9.2-9.5s to build
the same 1465 pages) — modest rather than dramatic, exactly as the stage's own subtitle predicts:
building still happens strictly in document order, so the saving is fewer, larger per-call budgets
rather than the target page arriving early. The bigger win — the target page appearing out of turn —
is Stage 2's to make possible and Stage 3's to turn on.

### Stage 2 — make the machinery gap-tolerant, without using it (done)

`Doc::pages` is `std::vector<std::optional<Page>>` rather than `std::vector<Page>` — addressable by
true page index, and able to hold a gap, rather than only appendable. `pendingShapings` is a
`std::map<std::uint32_t, PendingShaping>` keyed by true page index rather than a FIFO vector drained
in insertion order, though `std::map`'s ascending iteration means Stage 2 still drains it in exactly
the same order as before. A single `Doc::placePageAt()` is now the one place a page is actually
built — shared by `buildPendingPages()`, `newPage()`, `reflowFrom()`, and the new
`ensurePagesBuiltThrough()` guard — so all four agree on how a slot is filled rather than each
growing the old vector its own way.

The Y-position blocker (`pages.back()` meant nothing once a page can be built out of order) is fixed
in `buildPendingPages()` and `newPage()`: both now take a page's Y from
`pageIndexFilade.findEntryByIndex(n)->startYPx` instead of chaining off whatever was built most
recently. `reflowFrom()` keeps its own chain (from `pages[firstPage - 1]`, a specific known page
rather than "the back of the vector"), which stays valid regardless of gaps elsewhere. The baked
picking index was already the true index at every call site (`pages.size()` was always the next true
index in the old sequential-only world); `placePageAt()` makes that explicit by taking the index as
a parameter rather than inferring it from a push. `numPages()` now answers "how many pages are known
so far" (`pageEntries.size()`, independent of build progress) rather than "how many are built";
`builtPageCount()` is the new name for the old meaning, and is what the existing `DocPageBudgetTest`
progress assertions moved to, since that is what they were actually testing. `reflowFrom()` gained
`ensurePagesBuiltThrough()`, a synchronous guard run at its own entry that fills any gap up to and
including the page it is about to read from — unreachable by clicking (which requires the page
drawn) but reachable by automation typing at an arbitrary offset once out-of-order building exists.

**Build order stays document order** — behaviour-preserving and proven as such: the full
`gleditor_test`/`xudu_test`/`zigzag_test` suites pass unchanged (one pre-existing, load-sensitive
`ArrayfiladeBenchmarkTest` timing threshold aside, confirmed flaky independent of this work), and
`compare-backends.sh`'s `opengl`/`opengles` frames stay byte-identical (0 of 1,440,000 bytes differ,
before and after this stage).

**Tests** (`tests/lib/doc_gap_test.cpp`): a `DocGapTest` fixture, declared a friend of `Doc`
expressly for this file since no public API can construct a gap yet, builds page 5 directly while
leaving 0-4 unbuilt. Four cases: the gap holds and `builtPageCount()` counts only the one placed
page; picking a background click on the out-of-order page resolves against *its* text rather than
page 0's; `reflowFrom()`'s guard fills the one page it is about to read from before using it; and
`DocumentsSource::observe()`/`describe()` (the accessibility tree) still describe the document
without incident, exercising `boundsOf()`'s existing per-page null check now that a gap is a real,
reachable state rather than merely an unbuilt tail.

Shipped and soaking before Stage 3. The risk here was concentrated in picking and reflow, and both
are far easier to trust when the only variable was the container rather than the container *and* the
order.

### Stage 3 — turn on priority ordering (done)

`buildPendingPages()` picks its pages as: P0 (`Doc::viewportPriorityRange()`'s indices, ascending),
then P1 (`setPriorityOffsets()`'s pages, ascending), then P2 (everything else, ascending), until the
budget is spent. Selection happens once per call, up front: the current backlog (`pendingShapings`,
swapped out under `shapingMutex` as before) is walked into a single ordered key list — P0's range
via `pendingShapings.lower_bound()` so an empty or distant viewport costs nothing beyond one lookup,
P1 via `pageIndexFilade.findEntryAtByte()` per pushed offset, P2 by iterating whatever neither tier
already queued — deduplicated against a `std::unordered_set` so a page inside more than one tier is
only built once, in its highest-priority tier. The budget logic itself is unchanged — Stage 3
decides *what* to build, `buildBudgetForThisCall()`'s existing multiplier still decides *how much*,
with one fix Stage 2's gap-tolerant `pages` made necessary: its "is the target already built" check
used to compare `*targetIndex <= pages.size()`, which stopped meaning "already built" the moment a
page could be built out of order (a P0 build far from the front grows `pages.size()` without
building everything before it). It now asks `page(*targetIndex) != nullptr` directly.

`Doc::viewportPriorityRange()` answers P0: camera Y projected into this document's own page-pixel
coordinate (`Doc::cameraInfo()`, factored out of `buildBudgetForThisCall()`'s own camera code so
both functions read `AppState::view` once), ± half the frustum's visible height at this document's
distance from the camera (`2 * distance * tan(fov / 2)`, the same perspective-correct formula
`src/app.cpp`'s touch panning already uses), widened by `render::kViewportPriorityMarginFraction`
(25% of the viewport's own height, so a page about to scroll into view is already built rather than
popping in at the edge) and resolved to a page-index range via `Layoutfilade::visibleRange()` — no
new tree-walking code needed, since Stage 1's filade already answers exactly this shape of query.

**Tests** (`tests/lib/doc_page_budget_test.cpp`): `ViewportPriorityBuildsTheTargetPageDirectly`
parks the camera on a late page and confirms it is built in the very next call, without the whole
backlog having been built to get there. `PriorityOffsetComesRightAfterTheViewport` leaves the camera
at the default position, pushes a priority offset for a late page, and confirms both the viewport's
own pages (P0) and the priority page (P1) are built while a page *between* them — in neither tier —
is still a gap, which is what actually distinguishes reordering from simply finishing the backlog
faster. `DegeneratesToDocumentOrderWithNoPriorityAndAParkedCamera` is the degeneration test: with a
default camera and no priority offsets, every built page forms a contiguous prefix
`[0, builtPageCount())` after every single call until the document finishes loading — the exact
signature of ascending document order, checked after every call rather than once at the end so a
reordering that only shows up transiently cannot slip past.

The two Stage 1 catch-up tests (`CatchesUpFasterWhenTheCameraIsAheadOfBuildProgress` and
`PriorityOffsetFarAheadEngagesCatchUpToo`) asserted a *bigger sequential batch* got built once the
camera or a priority offset moved far ahead — true before Stage 3, when catching up meant extending
the same contiguous run faster. Once selection targets the page directly instead, that assertion
stopped holding (a targeted build reaches the target in a handful of pages, not hundreds) despite
the underlying property working *better* than before, so both were renamed and rewritten to assert
the stronger, more direct thing Stage 3 actually guarantees: the named page itself gets built.

The two priority-order tests need far more pages than the rest of this file's fixture text
(`manyManyPagesOfText()`, several times `manyPagesOfText()`'s size) to stay meaningful regardless of
how warm the process-wide glyph/font cache already is by the time they run: a warm cache made the
original 512 KB fixture (~100 pages) finish entirely within one catch-up-widened call in some test
orderings, which would make "a page between the two tiers is still unbuilt" trivially true for the
wrong reason.

**Verified**: the full `gleditor_test`/`xudu_test`/`zigzag_test` suites pass (the same pre-existing,
load-sensitive `ArrayfiladeBenchmarkTest` case aside), `compare-backends.sh`'s `opengl`/`opengles`
frames stay byte-identical, and a manual `kjv.txt` run still renders its first page in well under a
second with `--profile`, confirming the degeneration path (no camera movement, no priority offsets
-- exactly how a freshly opened document is framed) still behaves like Stages 0-2 for the scenario
that motivated this whole plan.

### Stage 4 — close the loop (done)

With Stages 0-3 in, `LinkBeams` can stop guessing: the beams whose pages it asked for are the ones
that resolve first. Re-measured the thing that motivated this — time from opening a document to a
beam crossing the viewport being drawn — and recorded it here, per
`.agents/rules/architectural_governance.md` §3's "performance claims must be supported by empirical
data".

**Instrumentation**: `RenderState` gained `loopStart`, a `std::chrono::steady_clock::time_point` set
once at the top of `Renderer::renderLoop()` — the same reference point its own
`[TIMING] First page rendered` / `[TIMING] Complete render settled` lines already measured from, now
shared rather than a local the render loop kept to itself. `LinkBeams::recordFirstBeamCrossing()`
(`apps/xudu/beams.cpp`) is called right after every `band()` draw call (both plain strands and
transclusion strands) and, the first time a strand's ribbon centreline actually tests on-screen (via
the same `ribbonMaybeOnScreen()` `outsideFrustum()`-based test `updatePriorityOffsets()` uses for
prioritising, but with zero inflation — both edges are exact here, resolved and about to be drawn,
not approximate), prints `[TIMING] First beam crossing viewport drawn: N ms` once and never again.
Library code still knows nothing about beams; the instrumentation lives entirely in `apps/xudu`,
reading only the shared clock the library now exposes.

**Measurement**: a small foreground document ("alpha beta gamma") linked to a 100 KB, 49-page
document opened alongside it (`--alongside`, so both are foreground and eligible for sworph — see
the note below on why `--background` cannot be used for this specific measurement), averaged over
two runs each on an otherwise-idle machine:

| Link target                                                                           | First beam crossing viewport | Complete render settled |
| ------------------------------------------------------------------------------------- | ---------------------------: | ----------------------: |
| Near the **start** of the big document (page ~0, builds first regardless of priority) |                   ~0.8-1.2 s |             ~8.9-12.2 s |
| Near the **end** of the big document (page ~48 of 49, needs the priority push)        |                       ~4.4 s |            ~11.2-11.8 s |

The number that matters is the second row against what the *old* gate (pre-Stage-0) would have
given: `LinkBeams::drawFrame()` returned before drawing anything — including every strand already
resolved — unless *every* open document reported `isFullyLoaded()`. A beam whose far end sits on the
last page of a document could not appear before that document's own full settle time, full stop: in
this scenario, not before ~11-12 s. Stages 0-3 bring that down to ~4.4 s for the same link — the
beam is visible while the document is still two-thirds of the way from finishing, rather than only
at the very end. The first row is the ceiling on how good Stage 3's reordering alone can make the
second row: a link to the very first page needs no priority push to begin with, so ~0.8-1.2 s is
roughly what "already there when requested" looks like for this document size — the gap between it
and 4.4 s is what document order still costs a page 48 pages deep even with priority ordering
pushing it as hard as one link can.

**Why `--alongside` rather than `--background` here, unlike Stages 0-1's own verification**: a
beam's ribbon has to be geometrically on-screen for `recordFirstBeamCrossing()` to fire at all,
which needs its far document positioned somewhere the camera can plausibly reach — `--background`
deliberately excludes a document from camera auto-framing and parks it deep in Z exactly so a large
corpus does not drag the reader's attention, which is correct for that document but means its pages,
however quickly built, are never "crossing the viewport" for this specific metric to observe.
`--alongside` reintroduces the camera auto-framing cost the earlier stages' own verification notes
worked around with `--background`, which is why this measurement deliberately stayed at 100 KB / 49
pages rather than the 1000+-page documents Stages 1-3's own tests use: at that scale the framing
cost (an existing, orthogonal issue, not this plan's to fix) dominates the number being measured
rather than the priority mechanism.

### Stage 5 — bank what is not wanted, so it never reaches the GPU (done)

Independent of Stages 0-4 in motivation, but only cheap *because* of them. Stages 1-3 decide what to
build **first**; this decides what not to build **at all**. A page that is neither near the viewport
nor touched by a visible beam is **banked**: shaped, addressable, ready — and never turned into GPU
resources unless it becomes wanted. A reader who opens `kjv.txt` and reads the first chapter should
finish with a handful of pages on the GPU, not 1261.

Once Stage 3 is in, this is a policy change rather than another rewrite: stop draining the P2 tier.
`pages` already tolerates gaps (Stage 2), a page's position already comes from the filade rather
than its predecessor (Stage 2), and priority selection already knows what P0 and P1 are (Stage 3).
Banking is what is left when you simply never get to P2.

Three things needed deciding, and they are what made this a stage rather than a flag:

- **A bank must be cheap, or it defeats itself.** `Doc` does *not* keep the `PageShaping` for a page
  it decides not to build — `Page`'s own header explains why (`include/gleditor/doc.hpp:150`): on a
  megabyte of text the layouts of every page came to ninety megabytes, "more than the vertex buffer
  they produced". `pendingShapings` held every not-yet-built page's full `PageShaping` regardless of
  tier before this stage, which meant the background loader's eager, whole-document `makePages()`
  (see "Non-goals" below — shaping itself stays sequential and eager; only building is reordered)
  could still leave the entire document's shaping resident in memory the moment it raced ahead of
  building, independent of priority. `Doc::buildPendingPages()` (`src/doc.cpp`) now resolves the
  wanted set (`Doc::wantedPageIndices()`: P0 then P1, ascending, deduplicated, falling back to page
  0 when there is no camera signal yet so a document never reports done having built nothing) once
  per call, and anything left in that call's `toBuild` map that is *not* in the wanted set is
  dropped — `std::erase_if` — rather than merged back into `pendingShapings`. Only a still-wanted
  leftover the budget did not reach goes back. A banked page keeps only its
  `pageEntries`/`pageIndexFilade` record (offset and length, from Stage 1) and is re-shaped on
  demand through `layoutFrom()` if it becomes wanted later — exactly the path
  `ensurePagesBuiltThrough()` already established for reflow (`src/doc.cpp`), reused rather than
  duplicated.
- **"Fully loaded" stops being reachable in the old sense.** `Doc::isFullyLoaded()` is no longer a
  cached `bool` set once and read back — it is
  `shapingComplete && every page in wantedPageIndices() is built`, recomputed live on every call
  (`src/doc.cpp`). That is what lets it answer correctly after the camera moves to reveal a banked
  page *without* `buildPendingPages()` having run in between: `src/renderer.cpp`'s loop gates
  calling `buildPendingPages()` on `!isFullyLoaded()` in the first place, so a stale cached flag
  would have frozen a document "done" forever the first time it settled. `pool->trim()` moved with
  it — called at the end of `buildPendingPages()` whenever the live check comes back true, rather
  than once ever, since `BufferPool::trim()` is already a cheap no-op when there is nothing to trim
  (`src/buffer_pool.cpp:328`).
- **The kjv benchmark changes meaning, on purpose.** `tools/benchmark-kjv-load.py` and the
  `[TIMING] Complete render settled` line used to measure "time to build every page". The settled
  line now reports `built pages` alongside the unchanged `total pages` (`src/renderer.cpp`), and the
  benchmark's summary table shows `Built/Total Pages` instead of the old MB/s throughput column,
  which assumed settle time scaled with document size — under banking it does not, on purpose. See
  the measurement below; `design/kjv-load-blocking-regression.md`'s figures remain the before.

**Tests** (`tests/lib/doc_page_budget_test.cpp`): `SettlesWithABoundedBuiltPageCountWellBelowTotal`
builds a many-hundred-page document with a parked default camera and no priority offsets, and checks
that `builtPageCount()` stays under a quarter of `numPages()` once `isFullyLoaded()` is true.
`ABankedPageBuildsIdenticallyToOneNeverBanked` bank a late page on purpose (one
`buildPendingPages()` call with nothing pointed at it), confirms it is still unbuilt, then names it
via `setPriorityOffsets()` and waits for it to build — comparing its `anchorFor()` result, text
length and base offset against the same page in a second document that wanted it from the very first
call and so was never banked at all. The existing Stage 1/3 tests that waited on `isFullyLoaded()`
expecting the *whole* document to finish
(`ABacklogShapedBeforeTheFirstCallStillBuildsInBoundedBatches`,
`PageIndexForOffsetAnswersBeforeAnyPageIsBuilt`, `ApproximateAnchorAgreesWithAnchorOncePageIsBuilt`)
needed updating to name what they actually wanted via `setPriorityOffsets()` first, since a bare
`isFullyLoaded()` wait no longer implies "everything got built" — see those tests' own comments. "A
beam that becomes visible pulls its banked endpoint page in" needed no new production code or test:
`LinkBeams` already pushes a strand's endpoint offsets through the same `setPriorityOffsets()`
channel (Stage 0/3), and Stage 5 changes nothing about what that channel does with them — a beam's
target page is simply banked, rather than eventually-built-via-P2, until the moment a strand names
it.

**Verified**: the full `gleditor_test` (514/514, the two new tests above included)/`xudu_test`
(944/944, excluding the pre-existing unrelated `AnimationTransclusionTest` hang)/`zigzag_test`
(90/90) suites pass, and `compare-backends.sh`'s `opengl`/`opengles` frames stay byte-identical --
the hard requirement per "Determinism is a hard constraint" above.

`compare-backends.sh`'s Vulkan-vs-OpenGL comparison, unrelated to Stage 5 but chased down and fixed
in the same pass since it had been failing (~1.4% against a 1% tolerance) at every prior stage: not
stale `image.vert/frag.spv` (a real bug, since fixed -- the Makefile's `SPIRV` is now derived from
`$(wildcard assets/shaders/*.glsl)` rather than a hand-maintained list that had silently stopped
including them -- but `quick_brown_fox.txt`, the default sample, draws no images, so rebuilding fresh
SPIR-V left the mismatch unchanged). Diffing the frames pixel-by-pixel and viewing the results showed
UI chrome -- `DocumentSwitcher`'s tab-bar border, not document text -- rendering a pixel taller/shorter
between backends. `Canvas::addLine()` (`src/canvas.cpp`) built a thin line's quad directly from its
caller's coordinates; layout arithmetic like `height - barHeight` routinely lands exactly on an
integer, which puts a 1px line's edges at `N-0.5`/`N+0.5` -- straddling two pixel rows/columns evenly,
an ambiguous split llvmpipe (GL/GLES here, forced software by this project's own headless convention)
and RADV (Vulkan, the only ICD available in this environment, real/virtual AMD hardware -- an entirely
different rasteriser, not just a different API over the same one) are free to resolve differently.
`addLine()` now snaps an axis-aligned segment's shared coordinate so both edges land on exact pixel
boundaries (`round(centre - thickness/2) + thickness/2`, which fixes 1px and 2px UI lines alike and
leaves radial_menu's angled borders, which are never axis-aligned, untouched). Dropped the mismatch
from 1.4852% (max delta 172) to 0.5756% (max delta 69) -- comfortably under the 1% limit, and the
residual is the ordinary antialiasing variance the tolerance exists for. Getting past that first
failure for the first time also reached a stage of `compare-backends.sh` this plan had never actually
exercised before: growing the Vulkan glyph atlas past 256x256 crashes deterministically with a RADV
`context is lost` GPU error on this machine, reproduced on every retry. Genuine and unrelated to
Stage 5 or to the fixes above -- filed as its own issue rather than chased down here.

A manual `kjv.txt` run (4.4 MB, 1261 pages) with `--profile`:

```text
[TIMING] First page rendered: 353.78 ms (docs in render: 1)
[TIMING] Complete render settled: 10017.46 ms (docs: 1, total pages: 1261, built pages: 1)
```

**1 page built, not 1261** — the number banking exists to shrink. `--profile` still terminates
cleanly (no hang, no race between the live `isFullyLoaded()` check and `settled`). The remaining ~10
s is not page building: it is `makePages()`'s own whole-document shaping, run once on a background
thread and unthrottled by design (see "Non-goals" below — shaping order was never in this stage's
scope, only what gets *built*), which `Renderer::hasPendingWork()` already waited on before this
stage existed. Before Stage 0, settling this same document meant that shaping cost *plus* building
all 1261 pages; Stage 5 removes the second term for a document whose viewport does not need it,
which is the whole point — it does not, and was never meant to, touch the first.

## Non-goals

- **Materialising an out-of-neighbourhood cell.** Cross-domain strands are in scope (see "Cell
  endpoints" above) and this plan prioritises their *document* ends; what it cannot do is make an
  unresolvable *cell* end resolve. That is its own work — see the same section for why it is a
  different problem and what it would take.
- **Eviction — its own design doc.** Nothing here ever destroys a page that has been built. The line
  worth keeping sharp, because the two are easy to run together: **banking** (Stage 5) is never
  spending — a page that was never wanted never reaches the GPU. **Eviction** is reclaiming what was
  already spent — a page that *was* wanted, was built, and is now scrolled far away. Banking bounds
  what a document costs on the way in and needs no policy beyond "is it wanted"; eviction needs a
  policy (what is cold, how much pressure justifies reclaiming it, what stops a page thrashing in
  and out at a scroll boundary) and has to unwind a built page's `BufferPool` allocation and
  glyph-atlas claims safely. Stages 0-5 are all a prerequisite for it and none of them decide it.
- **Shaping order.** `Doc::makePages()` must stay sequential from byte 0: page N's start offset is
  page N-1's end offset, so there is no shaping a page out of order without guessing where it
  starts. Only *building* is reordered here. Random-access shaping is a much larger question —
  `design/enfilade/layoutfilade-virtualized-scrolling.md` §1 is where it starts.
