# Priority page building: viewport first, then what beams reach

Implementation plan. The groundwork it stands on (a per-page coordinate index, and a per-call build
budget) landed with the Tier 1/Tier 2 work in
[`kjv-load-blocking-regression.md`](kjv-load-blocking-regression.md), which is worth reading first —
this note assumes its vocabulary (`Doc::buildPendingPages()`, `pageIndexFilade`,
`render::kPageBuildFrameBudget`).

**Status: Stages 0 and 1 are done** — see their sections below, both marked "(done)". Stages 2-5 are
not started.

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

### Stage 2 — make the machinery gap-tolerant, without using it

All four blocker answers above: `pages` becomes a map, Y comes from the filade, the true index is
passed through, `numPages()` splits, `reflowFrom()` gets its guard, `pendingShapings` becomes a
pool. **Build order stays document order** — this stage should be behaviour-preserving and provable
as such: the whole existing suite, plus `compare-backends.sh` byte-identical, plus the kjv profile
timings unchanged.

Ship it and let it soak before Stage 3. The risk here is concentrated in picking and reflow, and
both are far easier to trust when the only variable is the container rather than the container *and*
the order.

**Tests**: picking resolves to the correct offset for a page built out of order (construct the gap
directly in a test, since nothing produces one yet); `reflowFrom()`'s guard fills a preceding gap;
a11y's tree still enumerates correctly across a gap.

### Stage 3 — turn on priority ordering

`buildPendingPages()` picks its next page as: P0 (viewport range, from camera Y ± half a viewport,
inflated by a margin), then P1 (`setPriorityOffsets()`'s pages), then P2 (lowest unbuilt index),
until the budget is spent. The budget logic itself is unchanged — Stage 3 decides *what* to build,
the existing multiplier decides *how much*.

**Tests**: with the camera parked on a late page, that page is among the first built; with a
priority offset pushed for a late page and the camera at the start, viewport pages still win and the
priority page comes next; **the degeneration test** — fixed default camera, no priority offsets,
build sequence is exactly document order.

### Stage 4 — close the loop

With Stages 0-3 in, `LinkBeams` can stop guessing: the beams whose pages it asked for are the ones
that resolve first. Re-measure the thing that motivated this — time from opening a document to a
beam crossing the viewport being drawn — and record it, per
`.agents/rules/architectural_governance.md` §3's "performance claims must be supported by empirical
data".

### Stage 5 — bank what is not wanted, so it never reaches the GPU

Independent of Stages 0-4 in motivation, but only cheap *because* of them. Stages 1-3 decide what to
build **first**; this decides what not to build **at all**. A page that is neither near the viewport
nor touched by a visible beam is **banked**: shaped, addressable, ready — and never turned into GPU
resources unless it becomes wanted. A reader who opens `kjv.txt` and reads the first chapter should
finish with a handful of pages on the GPU, not 1261.

Once Stage 3 is in, this is a policy change rather than another rewrite: stop draining the P2 tier.
`pages` already tolerates gaps (Stage 2), a page's position already comes from the filade rather
than its predecessor (Stage 2), and priority selection already knows what P0 and P1 are (Stage 3).
Banking is what is left when you simply never get to P2.

Three things do need deciding, and they are what makes it a stage rather than a flag:

- **A bank must be cheap, or it defeats itself.** Do *not* keep the `PageShaping` — `Page`'s own
  header explains why (`include/gleditor/doc.hpp:150`): on a megabyte of text the layouts of every
  page came to ninety megabytes, "more than the vertex buffer they produced". Bank only a page's
  start offset and byte length, both of which the filade already holds after Stage 1, and re-shape
  on demand through `layoutFrom()`. That path is established, not new: `Page::ensureShaping()`
  (`src/doc.cpp:322`) already discards and re-derives shaping for exactly this reason. The bank is
  then the filade plus the text, which costs effectively nothing per page.
- **"Fully loaded" stops being reachable.** A banked document may never build every page, so
  `isFullyLoaded()` can no longer mean "all pages built" — it has to mean "everything currently
  wanted is built". That is load-bearing beyond `Doc`: `src/renderer.cpp:984` computes `docsLoading`
  from it, which gates `settled`, which is what `--profile` and `--screenshot` wait for. Get this
  wrong and captures either race or hang. (Stage 0 has already removed the *other* consumer of the
  all-or-nothing reading, `LinkBeams`' gate — which is part of why banking is tractable by now.)
- **The kjv benchmark changes meaning, on purpose.** `tools/benchmark-kjv-load.py` and the
  `[TIMING] Complete render settled` line currently measure "time to build 1261 pages". Under
  banking the honest measurement is time-to-first-page and steady-state page count, and "total
  pages" becomes a number that legitimately never reaches the document's length. Update the
  benchmark in the same change rather than leaving a number that silently means something else —
  `design/kjv-load-blocking-regression.md`'s figures are the before, and should be cited as such.

**Tests**: a document larger than the viewport settles with a bounded number of built pages while
`isFullyLoaded()` reports true; scrolling to a banked page builds it and it renders identically to
the same page built eagerly; a beam that becomes visible pulls its banked endpoint page in; the
`--profile` settle path still terminates.

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
