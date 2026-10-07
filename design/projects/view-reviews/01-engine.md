# Engine implementation review: `design/view-system.md`

Scope: everything that lands in `apps/common/xanadu/view/` (ViewManifold and its two ArenaManifolds,
ViewAxisSet, layout records, the text raster, the three slice views, PageView and the two page
views, the selector, dimension ranking, the gesture state machine), plus the two `ArenaManifold`
changes §6.5 asks for. All line citations below were checked against the tree at the commit this
session started from (same working tree as `af5f1d5`'s descendant the spec cites; no drift was found
except the `ops.hpp` `LinkType::Format` line, noted inline).

______________________________________________________________________

## 1. Claim check

| #   | Claim                                                                                                                                           | Verdict                                                                                   | Evidence                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| --- | ----------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | "release() to an empty mark is a fixed number of steps given no shadows, no trail and the d.store-refs guard"                                   | **CONFIRMED, but the guard does not exist yet and must be placed carefully**              | `ArenaManifold::release()` (`apps/common/xanadu/zigzag/arena_manifold.cpp:1566-1654`) loops, in order, over `quoteSpaceOrder_`, `proxyShadowedEdgeOrder_`, `quoteOccurrenceOrder_`, `proxyOrder_`, `trail_`, `shadowOrder_` — all bounded by the *delta since the mark*, which is zero when nothing of that kind was minted. For a view arena (no spaces attached, no shadows, no trail) every one of those loops runs zero iterations. The one unbounded step is lines 1632-1647: `const auto dimStoreRefs = dimensionNamed("d.store-refs");` runs **unconditionally**, before the `if (dimStoreRefs != noCell)` that guards the `for (s : spaces_)` loop. `ArenaManifold::dimensionNamed` with no reader (`arena_manifold.cpp:56-76`) falls through to `base_->dimensionNamed(name)` when the arena has a base (a `ViewManifold`'s two arenas always do — see §6.1's `ArenaManifold(base, nullptr)`), which lands in `Manifold::dimensionNamed(name, reader)` (`manifold.cpp:785-824`). Because `d.store-refs` is never a real slice dimension, the `DimensionRegistry` fast path misses every time, and the slow path (`manifold.cpp:796-817`) scans **every dimension of the base**, calling `textOf()` (which allocates a `std::string`) on each one to find `d.alias`, then filters all of them by name. That scan is *not* proportional to what the view minted, but it **is** proportional to the slice's real dimension count, it allocates, and it repeats on every toss. |
| 2   | "linking two ephemeral cells on an ephemeral dimension key never shadows a base cell"                                                           | **CONFIRMED**                                                                             | `ArenaManifold::link()` (`arena_manifold.cpp:1300-1358`) calls `shadow()` only on `from`, `to`, and the two displaced occupants. `shadow()` (`arena_manifold.cpp:1422-1461`) returns `denseOf(ref)` immediately when the ref is ephemeral and already minted (`denseOf`, `arena_manifold.cpp:254-267`, is arithmetic — `ref & ~ephemeralBit`, bounds-checked against `slots_.size()`), never reaching the `base_->slot(ref)` branch. The dimension argument is checked only with `contains(dim)` (`arena_manifold.cpp:1309`), which is `slot(ref).has_value()`; for an ephemeral dim this is the same arithmetic check, with no base lookup at all.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| 3   | "makeCell(text) + setValueBits(OpHandle) gives a handle cell and handleTarget reads it"                                                         | **CONFIRMED**                                                                             | `makeCell()`/`makeCell(text)` mint via `mintSlot(ValueKind::None, 0, ...)` (`arena_manifold.cpp:1178-1229`). `setValueBits(cell, OpHandle, bits)` (`arena_manifold.cpp:1287-1298`) shadows the cell and overwrites `valueKind`/`valueBits` in place. `handleTarget()` (`arena_manifold.cpp:1074-1082`) reads `slot(ref)`, checks `valueKind == OpHandle`, returns `valueBits` cast to `CellRef`. The spec's own §6.3 example uses a bare `makeCell()`, not `makeCell(text)` — both work identically for this purpose since neither sets `valueKind`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| 4   | "promote() turns handle cells into real OpHandle cells"                                                                                         | **CONFIRMED**                                                                             | `arena_manifold.cpp:1836-1839`: `else if (xanadu::ValueKind::OpHandle == kind) { minted = store.makeOpHandle(out.version, from.handleTarget(arena).value_or(0), from.textOf(arena)); }`, inside `promote()`'s per-cell minting loop (`arena_manifold.cpp:1762-1848`). `Store::makeOpHandle` exists at `store.hpp:515`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| 5   | "a CellRef is stable across versions"                                                                                                           | **CONFIRMED for real cells, with a sharper statement needed**                             | `CellRef`/`DimRef` are plain `std::uint32_t` (`dim_vector.hpp:21-23`). For a real cell, the value *is* the index of the operation that birthed it (`manifold.hpp`'s file doc: "a cell is not a new kind of object: it is an operation"); once an operation is appended to the spool its index never changes, so the integer is stable for the life of the store. What is *not* stable is whether a given fold *contains* that index yet (a manifold folded to an earlier version won't have a later cell) — stability of naming, not of presence. For **ephemeral** (arena) refs the opposite holds: `mintSlot` assigns dense indices as `slots_.size()` at mint time (`arena_manifold.cpp:1156`), and `release()` truncates `slots_` back to the mark (`arena_manifold.cpp:1625`), so the *same* `ephemeralBit \| dense` value is reused by the next mint after a toss for a *different* cell. This is exactly why `ViewCellRef` must carry an epoch (§6.5) — the design is internally consistent, but "a CellRef is stable" should not be read as applying to `ViewCellRef`s across a toss.                                                                                                                                                                                                                                                                                                                                                                                       |
| 6   | "TensionLayoutEngine stepped a fixed number of times is a pure function"                                                                        | **CONFIRMED for the integrator; UNCERTAIN that today's usage pattern transfers directly** | `tension_layout.cpp` reads no clock and no RNG (checked by grep); `step()` is RK4 over `bodies_`/`constraints_`/`params_` only. `apps/xudu/beams.cpp:953-954` confirms the literal "25 steps at a fixed dt" the spec describes. But `TensionLayoutEngine` is a **stateful, reusable** object (`bodies_` carries velocity across calls; `running_`, `isSettled()`, `toggleRunning()` exist for a continuous per-frame simulation). Today's `beams.cpp` keeps one `tensionEngine_` alive across many rendered frames, so its *cumulative* step count before a reader notices settling is much larger than 25, and velocity persists frame to frame. §10.3's `CoalesceStrategy::solve(bodies, ties)` is specified as pure — same bodies/ties in, same positions out — which is only true if the adapter constructs a **fresh** engine (or zeroes velocity) and loads exactly the bodies/ties passed in on every call. That adapter is new code, not a direct reuse of `beams.cpp`'s object lifetime, and nothing in the spec says the fixed-25-steps-from-cold-start result is numerically close to what today's continuously-stepped engine converges to for the same scenario. See Spike S2.                                                                                                                                                                                                                                                                                         |
| 7   | "LinkOccurrences can supply link ends per page"                                                                                                 | **UNCERTAIN — partial.**                                                                  | `resolveLinkOccurrences()` (`link_occurrences.hpp:195-198`) returns, per member, a list of `Occurrence{OccurrenceSite, Coverage}` where `OccurrenceSite` is a `DocumentSite{store, version, Extent range}` or `CellSite`. That is a **byte range in a document's text**, not a page + height. `page_view.hpp`'s `LinkEnd{PageRef page; float top, bottom; ...}` needs a byte-extent → (page, top-px, bottom-px) mapping that does not exist anywhere in the tree today (grep for `pageOf`/`offsetToPage`/`byteToPage` under `include/gleditor/doc.hpp` and `apps/xudu/` returned nothing). `LinkOccurrences` supplies the *raw* occurrence data the mapping would consume; it does not itself supply page-shaped ends. See Hidden work, item H4.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| 8   | "ActivityLog can take a new kind of record"                                                                                                     | **NOT CONFIRMED as of today's code — and the spec itself says so (VU5)**                  | `ActivityLog` (`link_navigation.hpp:88-105`) is a closed abstract interface around one concrete payload type, `Visit` (`id, parent, OccurrenceSite target, Arrival, optional<LinkVisitContext>`). `append(Visit)` is the only writer; there is no generic "append any record" extension point. `StoreActivityLog::appendRecord(dimension, text)` (`store_activity_log.cpp:173`) is a private helper, not part of the public interface. Adding `WalkSummary` storage (§7.8) requires either a new virtual method on `ActivityLog` (an interface break, cheap pre-1.0 but still a real change) or a second, parallel append-only structure. This is exactly what VU5 defers ("their shape there... is that store's design to make"), so the spec is not asserting this already works — the prompt's phrasing of the claim is what I am flagging as not-yet-true, not a spec defect.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| 9   | "ArenaManifold gains two things and no new concept: the guard above, and shadowCount(), a const accessor for a count it already keeps for Mark" | **CONFIRMED, with a precise fix needed**                                                  | `shadowOrder_.size()` is tracked today only as a private member read once into `Mark::shadowCount` at `mark()` time (`arena_manifold.cpp:1538`) and compared against at `release()` time (`arena_manifold.cpp:1620`); there is no live public accessor. Adding `[[nodiscard]] std::size_t shadowCount() const noexcept { return shadowOrder_.size(); }` is a one-line, zero-risk addition. The `release()` guard must wrap the **lookup**, not just the loop: `if (!spaces_.empty()) { const auto dimStoreRefs = dimensionNamed("d.store-refs"); if (dimStoreRefs != noCell) { ...for (s : spaces_)... } }`. Wrapping only the `for` loop (a literal reading of "guard the loop") leaves the expensive, unconditional `dimensionNamed()` call in place and defeats the fix.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |

Two secondary findings that came out of tracing claim 8 and the ViewManifold design, stated here
because they affect how claims 1-9 compose:

- **`Manifold::dimensionNamed(name)` (no-reader overload) returns `nullopt` whenever the manifold's
  own `store_` is null** (`manifold.cpp:826-832`), but
  `ArenaManifold::dimensionNamed(name, reader=nullptr)` (`arena_manifold.cpp:56-76`) falls back
  through `base_->store()` when the *base* has a store, even though the arena's own `store_` is null
  — which is exactly a view arena's situation (`ArenaManifold(base, nullptr)`). So **dimension names
  are read correctly today with no extra plumbing**: calling `arena.textOf(dimRef)` or
  `arena.dimensionNamed("d.pack")` on a view arena resolves through `base_->store()` automatically.
  This answers one of the prompt's named "hidden work" questions directly — see Hidden work, item
  H2.
- **`Manifold` has no public cell-minting API.** Cells exist only as folded operations
  (`applyStructure`); there is no `Manifold::makeCell()`. Any engine test that needs a "hand-built
  manifold" must build a small `xanadu::Store` via its Structure API (`sliceGenesis`, `makeCell`,
  `setLink`, `addLink`, ...) and then call `store.rebuildManifold(version)` to get a real `Manifold`
  — exactly the pattern `tests/xuzz/two_by_three_fixture.hpp` already uses for a (text, not ZigZag)
  fixture. This matters for the Testing section below.

______________________________________________________________________

## 2. Work packages

Ordered; sizes are rough new lines of implementation + test code. "Depends on" names other packages
in this table, not migration steps. The spec's migration step numbering (§17) groups several of
these per commit; the breakdown below is finer, matching "small enough to land as a single commit."

| #   | Package                                               | Files (new unless noted)                                                               | Depends on                                                                                     | Tests (new file under `tests/xuzz/`)                                                                                                                                                                                                                                                                                                                                                                                                                                                  | Size                          | Main risk                                                                                                                                                                                                                                                                                                                                                               |
| --- | ----------------------------------------------------- | -------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 0   | `ArenaManifold`'s two changes                         | `apps/common/xanadu/zigzag/arena_manifold.{hpp,cpp}` (edit)                            | —                                                                                              | extend `tests/xudu/arena_manifold_test.cpp`: `shadowCount()` tracks mint/shadow/release; `release()` on an arena with `spaces_.empty()` performs no `dimensionNamed` lookup (instrument with a call counter or assert via `DimensionRegistry` cache state)                                                                                                                                                                                                                            | S (~20 lines + ~40 test)      | Getting the guard's placement wrong (around the loop, not the lookup) silently keeps the O(dimensions) scan — see claim 9. Must not break `tests/xudu/arena_federation_test.cpp`, which exercises `d.store-refs` with real attached spaces.                                                                                                                             |
| 1   | `view_records.hpp`                                    | `apps/common/xanadu/view/view_records.hpp`                                             | —                                                                                              | `view_records_test.cpp`: `LayoutSink::push` never drops (grows + logs on `view.layout` category — assert via a spdlog sink or a growth counter); `clear()` resets; `SubjectId` equality/epoch semantics                                                                                                                                                                                                                                                                               | M (~250 impl + ~150 test)     | None significant; it's data plus a grow-only sink.                                                                                                                                                                                                                                                                                                                      |
| 2   | `view_error.hpp`                                      | `apps/common/xanadu/view/view_error.hpp`                                               | —                                                                                              | folded into package 3's test (`messageKey` coverage for every enumerator)                                                                                                                                                                                                                                                                                                                                                                                                             | S (~60 + ~40)                 | See Gap G8: not every §12.2 message is backed by a `ViewError`; keep this file to the enumerators §8.11 actually lists and resolve the gap before relying on `messageKey()` for everything in §12.2.                                                                                                                                                                    |
| 3   | `view.hpp` (registry)                                 | `apps/common/xanadu/view/view.hpp`                                                     | 2                                                                                              | `view_registry_test.cpp`: `add()` refuses `DuplicateViewKind` and `ChordCollision`; `views()`/`find()`; two independently-constructed registries don't share state (it's an owned object, not a singleton)                                                                                                                                                                                                                                                                            | M (~200 + ~150)               | `registerBuiltinViews()` is necessarily a stub until packages 9-19 exist; don't let it grow ad hoc — keep it a thin fold over the already-built descriptors, added one call per package as each view lands.                                                                                                                                                             |
| 4   | `view_manifold.{hpp,cpp}`                             | `apps/common/xanadu/view/view_manifold.{hpp,cpp}`                                      | 0, 2                                                                                           | `view_manifold_test.cpp`: I1 (`OccupiedDirection` refusal on a second claimant), I3 (`verifyViewSpace` + `shadowCount()==0` after random bind/derive/toss sequences), I4 (the §6.5 preconditions test, ported from package 0's groundwork), I5 (`resolveReal` on an occurrence and on a pack container), `StaleEpoch` refusal on a ref from a tossed generation, `RealCellInViewLink` refusal when a real base `CellRef` is passed as either end **or as the dimension** (see Gap G1) | L (~700 + ~400)               | This is the centerpiece everything else sits on. The two-ArenaManifold design means a binding-layer `ViewCellRef` and a derived-layer one can carry the *same* numeric `.ref` (both arenas mint dense indices from zero) — see Gap G1 and Spike S4. Get the `Layer` discipline wrong here and every later package inherits a silent wrong-cell bug.                     |
| 5   | `view_binding.{hpp,cpp}` (`ViewAxisSet`)              | `apps/common/xanadu/view/view_binding.{hpp,cpp}`                                       | 4                                                                                              | `view_binding_test.cpp`: uncapped `axisCount`/group size/nesting; `bind`/`unbind`/`swap`; `createGroup`/`insertMember`/`removeMember`/`moveMember`/`deleteGroup`; `GroupCycle`, `EmptyGroupBind` refusals; `ringPlace`/`moveInRing`; `forEachAxisShowing`/`forEachGroupContaining`; `undo`/`redo` round-trips `shown()` for every axis                                                                                                                                                | L (~550 + ~350)               | Nested-group persistence replay ordering (Gap/Hidden-work item H6) is not needed for this package's in-memory behaviour, only for the later persistence package — don't let it block landing this one.                                                                                                                                                                  |
| 6   | `raster.{hpp,cpp}`                                    | `apps/common/xanadu/view/raster.{hpp,cpp}`                                             | 1                                                                                              | `raster_test.cpp`: a fixed `LayoutSink` → known ASCII grid, nearest-first ordering, `edges` on/off                                                                                                                                                                                                                                                                                                                                                                                    | S/M (~150 + ~100)             | None; depends only on records. **Can run fully in parallel with 4/5.**                                                                                                                                                                                                                                                                                                  |
| 7   | `pack_rank.{hpp,cpp}` + `pack_presentation.{hpp,cpp}` | `apps/common/xanadu/view/slice/pack_rank.{hpp,cpp}`, `.../pack_presentation.{hpp,cpp}` | 4, 5, 1                                                                                        | `pack_rank_test.cpp`: §9.3.4's two worked examples verbatim (simple group; nested group with `e1` appearing twice and the existence of step +3 / non-existence of +4); `pack_presentation_test.cpp`: strand count = lanes present at both ends, spread sub-view/hover emits separated strands + lane labels                                                                                                                                                                           | L (~500+400 impl + ~400 test) | The "two occurrences of one real cell, focusing either highlights both" behaviour (§9.3.4) is implemented by comparing `resolveReal()`/`target()` results across currently-visible occurrences — the spec never states this join key explicitly (Gap G7); decide and document it here.                                                                                  |
| 8   | `slice_view.hpp`                                      | `apps/common/xanadu/view/slice_view.hpp`                                               | 4, 5                                                                                           | `slice_view_test.cpp`: `cellAt()` over origin/pack-cursor variants; default `move()`'s `AlongAxis` handling                                                                                                                                                                                                                                                                                                                                                                           | S/M (~150 + ~100)             | None significant.                                                                                                                                                                                                                                                                                                                                                       |
| 9   | `slice/stretch_vanishing_view.{hpp,cpp}`              | —                                                                                      | 4, 5, 8, **library**: `<gleditor/spatial.hpp>`, `<gleditor/draw_budget.hpp>` (`insideFrustum`) | `stretch_vanishing_view_test.cpp`: §9.1.8 acceptance list in full, incl. the 10³ vs 10⁶-cell flat-count check (needs a synthetic manifold builder, see Testing)                                                                                                                                                                                                                                                                                                                       | L (~700 + ~400)               | **Cross-cutting dependency on library work that is nominally someone else's migration step (§17 step 2).** `insideFrustum` for the all-or-nothing clip (V-R18) does not exist yet; this package cannot test its clip/ghost behaviour until it does. Also Gap G5 (the "perpendicular v" slide formula is written for exactly 2 in-plane axes).                           |
| 10  | `slice/all_dim_walk_view.{hpp,cpp}`                   | —                                                                                      | 4, 5, 8                                                                                        | `all_dim_walk_view_test.cpp`: §9.2.11 in full (valence-exactness, opposite-neighbour centre-sum, bound-axis alignment to R/U/F, ring-lean clamp, ring-order match, no-move-on-bind/unbind, cancelled-drag no-op, wheel-vs-real-cell non-overlap at depth, detail-level monotone decay)                                                                                                                                                                                                | L (~700 + ~400)               | Needs only `glm` (header-only) for the wheel math, not a device — confirm this at review time since it's easy to accidentally pull in a rendering header. **Can run in parallel with package 9** (disjoint files, independent geometry).                                                                                                                                |
| 11  | `slice/dimensional_pack_view.{hpp,cpp}`               | —                                                                                      | 7, 8, 5                                                                                        | `dimensional_pack_view_test.cpp`: §9.3.11 in full, incl. `promotePack()` wiring and its budget refusal path                                                                                                                                                                                                                                                                                                                                                                           | M/L (~400 + ~300)             | Return-type mismatch between the spec's `promotePack()` signature and `zigzag::promote()`'s real result — see Gap G4; resolve before writing the "pack kept, N cells" success/refusal path.                                                                                                                                                                             |
| 12  | `view_gesture.{hpp,cpp}`                              | —                                                                                      | 1, 5                                                                                           | `view_gesture_test.cpp`: the §9.2.7 state table verbatim (Idle→Armed→Dragging→Committed, threshold, drop radius, Escape/elsewhere→Idle no-op, stale-epoch pick discarded)                                                                                                                                                                                                                                                                                                             | S/M (~200 + ~150)             | Keep it a pure "events in, intents out" machine as specified — don't let it call `ViewAxisSet::bind` directly, or it stops being host-agnostic and untestable without a live binding arena. **Can run in parallel with 9/10/11** (only needs 1 and 5).                                                                                                                  |
| 13  | `dimension_ranking.{hpp,cpp}`                         | —                                                                                      | none beyond `zigzag::CellRef`/`DimRef`                                                         | `dimension_ranking_test.cpp`: `WalkRecorder::step/settle` slip-dropping (`bounceMs`) and run-condensation; `rankDimensions` decay (`halfLife`), Markov order keyed on `last`, `presentBoost`, determinism                                                                                                                                                                                                                                                                             | M (~300 + ~250)               | Gap G6: whether pack/lane/retrieve movement over view-owned dimensions should ever reach `WalkRecorder::step()` is unspecified — resolve before wiring call sites in packages 9-12 (recommend: no, only `MoveKind::AlongAxis` over a *real* dimension). **Fully independent of the whole slice-view-space track (4-12); can be built and merged any time in parallel.** |
| 14  | `selector.{hpp,cpp}`                                  | —                                                                                      | 4, 5, 9 (reuses stretch placement for tier 3), 13                                              | `selector_test.cpp`: tier transitions on the z keys; `quick` population bound at 10 by `inplace_vector`; tier-1 group fan-out; arm + bind-point-name surfaces the right `(target, point)` pair                                                                                                                                                                                                                                                                                        | L (~450 + ~300)               | Composes four other packages; land it last among the slice-view track.                                                                                                                                                                                                                                                                                                  |
| 15  | `page_view.hpp`                                       | `apps/common/xanadu/view/page_view.hpp`                                                | `link_occurrences.hpp` (existing), `document_id.hpp` (existing)                                | — (pure interfaces; exercised by 16-19's tests)                                                                                                                                                                                                                                                                                                                                                                                                                                       | S (~150)                      | None. **Entirely independent of packages 4-14 — page views have no view space.** This is the strongest parallelism point in the whole plan: the page-view track (15-19) and the slice-view track (4-14) touch disjoint files and can proceed as two separate workstreams from day one.                                                                                  |
| 16  | `page/coalesce.{hpp,cpp}`                             | —                                                                                      | 15, `tension_layout.hpp` (existing)                                                            | `coalesce_test.cpp`: a parity fixture reproducing `LinkBeams`' current 25-step, one-body-per-document result (captured as golden numbers from today's `beams.cpp` behaviour); "best fit" assertions (level passages within tolerance, no overlap when all fit)                                                                                                                                                                                                                        | M (~300 + ~250)               | **Same risk as claim 6**: adapting the stateful `TensionLayoutEngine` into a cold-start, exactly-N-steps pure call is not guaranteed to reproduce what today's warm, continuously-stepped engine converges to. Run Spike S2 before committing to this package's exact adapter shape.                                                                                    |
| 17  | `page/base_view.{hpp,cpp}`                            | —                                                                                      | 15, 16, 1                                                                                      | `base_view_test.cpp`: §10.3.4 in full over hand-written `PageCatalog`s                                                                                                                                                                                                                                                                                                                                                                                                                | L (~600 + ~400)               | Participant/window/overlap logic is the most case-heavy acceptance list in the document; budget real time for the "more ends than fit" branch.                                                                                                                                                                                                                          |
| 18  | `page/deck.{hpp,cpp}`                                 | —                                                                                      | 15, 1                                                                                          | `deck_test.cpp`: stagger-direction search stability under a 1%-wider pane (hysteresis), scoring formula                                                                                                                                                                                                                                                                                                                                                                               | L (~500 + ~350)               | The candidate search (§10.4.3) is a few hundred candidates × a handful of pages every `prepare()` call that changes pane/page/type size — cheap per the spec's own cost table, but confirm with a probe rather than assume.                                                                                                                                             |
| 19  | `page/stacked_vanishing_view.{hpp,cpp}`               | —                                                                                      | 18, 15, 1                                                                                      | `stacked_vanishing_view_test.cpp`: §10.4.10 in full (opacity monotonicity, marked-page-always-opaque, riffle/split threshold, right-to-left stack side)                                                                                                                                                                                                                                                                                                                               | M (~350 + ~300)               | None beyond 18's.                                                                                                                                                                                                                                                                                                                                                       |
| 20  | `builtin_views.{hpp,cpp}`                             | —                                                                                      | all of 9-14, 16-19                                                                             | a thin registry-population test asserting every built-in `ViewDescriptor` is present with no duplicate kind/chord                                                                                                                                                                                                                                                                                                                                                                     | S (~100 + ~100)               | Pure glue; keep it that way — if logic accretes here, it belongs in one of 9-19 instead.                                                                                                                                                                                                                                                                                |

**Ordering and parallelism, summarized:**

```
0 ─┬─► 4 ─► 5 ─┬─► 7 ─┬─► 11 ─┐
   │          ├─► 8 ─┼─► 9 ──┼─► 14 ─┐
 1 ┘          │      ├─► 10 ─┤       │
              └─► 12 ┘       │       ├─► 20
 2 ─► 3 ──────────────────────────────┘
 6  (parallel with everything from 1 onward)
13 (parallel with everything; independent track)
15 ─┬─► 16 ─► 17 ─┐
    └─► 18 ─► 19 ─┴──────────────────┘ (feeds 20)
```

The two biggest-value parallel splits: **(a) the slice-view track (0, 4, 5, 7-14) and the page-view
track (15-19) are file-disjoint and can be staffed as two independent workstreams**, exactly as the
spec's own §17 notes for its coarser "step 2 is independent of steps 3 to 5"; **(b) within the
slice-view track, packages 9, 10 and 12 are mutually independent once 8 lands**, and 6 and 13 are
independent of the entire rest of the table.

______________________________________________________________________

## 3. Spec gaps that block implementation

Ordered by how early an implementer hits them. The first five are the ones named in the summary.

**G1. `ViewManifold::link()`'s `Layer` parameter can disagree with the `Layer` embedded in its
`ViewCellRef` arguments, and the spec never says what happens then.** Quote, §8.2:

```cpp
ViewResult link(Layer layer, ViewCellRef from, ViewDim dim, zigzag::DimVector dir, ViewCellRef to) noexcept;
```

— `layer` is passed explicitly *and* `from`, `to` and `dim` (itself a `ViewCellRef` via
`using ViewDim = ViewCellRef`) each already carry a `.layer` field (§8.2's `ViewCellRef` struct).
Because the binding arena and the derived arena are two independent `ArenaManifold`s that both mint
dense indices from zero (`mintSlot`, `arena_manifold.cpp:1156`), a binding-layer ref and a
derived-layer ref can carry the *identical* numeric `.ref`. Nothing in §6.6's refusal list ("either
ref or the dimension is stale... either end is not a view cell of that layer's arena") says whether
a call where the explicit `layer` disagrees with `from.layer`/`to.layer`/`dim.layer` is refused, and
if so with which `ViewError`. *Recommended resolution:* treat any such disagreement as
`RealCellInViewLink` (or add a dedicated `LayerMismatch` enumerator), and state explicitly that the
explicit `layer` parameter is authoritative for which arena is written, while every `ViewCellRef`
argument's own `.layer` must match it or the call is refused.

**G2. No `Role`/role-registration type is declared anywhere, despite §7.3 and §8.10 requiring one.**
Quote, §8.10: "a binding point's role | a role, registered as a view is (§7.3) | the bound target,
the cursor, the placement | assume a view presents it." §7.3 describes `subspace` and `hypertime`
entirely in prose (zoom transitions, a rim band, "the whole slice at the previous hypertime
operation") with no corresponding struct, virtual interface, or registry anywhere in §8 — unlike
`View`/`ViewDescriptor`, which are fully specified. V-R50 requires "a role... addable without
changing the framework," which is untestable without a declared extension surface. *Recommended
resolution:* either declare a `RoleDescriptor`/`Role` interface analogous to `ViewDescriptor` before
any package builds `u` or `t` behaviour, or explicitly scope the first implementation to three
hard-coded roles (spatial/subspace/hypertime) in host logic and defer the plug-in surface to a
follow-up once a real third-party role exists — but say so, rather than leaving V-R50 silently
unmet.

**G3. `verifyViewSpace()`'s required access to both arenas' live shadow/trail/space counts has no
path through `ViewManifold`'s declared public API.** §8.2 lists only `base()`, `epoch()`, `axes()`,
the four mint/link functions, the four read functions, and the four derived-generation accessors —
no raw `ArenaManifold&` and no per-layer `shadowCount`/`trailSize`/`spaceCount`. §6.6 describes
`verifyViewSpace(const ViewManifold &, report)` as an ordinary free function, which cannot implement
I3 ("shadowCount() == 0 on both arenas") without either friend access to `ViewManifold`'s private
`bindings_`/`derived_` members or new public accessors. *Recommended resolution:* add
`shadowCount(Layer)`, `trailSize(Layer)`, `spaceCount(Layer)` to `ViewManifold`'s public surface
(thin wraps over the now-public `ArenaManifold::shadowCount()` from package 0 and the already-public
`trailSize()`), and have `verifyViewSpace` use only those — never a raw arena reference.

**G4. `promotePack()`'s declared return type discards the result a caller actually needs.** §8.5:

```cpp
std::expected<xanadu::Store *, ViewError> promotePack(const ViewManifold &space, ViewCellRef container, xanadu::Store &store, xanadu::MicroversionId parent);
```

The real `zigzag::promote()` it must call (`arena_manifold.hpp:752-754`) returns
`std::optional<Promoted>` — a new `MicroversionId` *and* the vector of real `CellRef`s minted, in
mint order. A `Store *` on success is useless (the caller already has `store`) and loses exactly the
information §12.2's "kept N cells" / refusal messages, and any cursor-retargeting after a keep,
need. *Recommended resolution:* `promotePack()` should return
`std::expected<zigzag::Promoted, ViewError>` (or an equivalent
`{MicroversionId, std::vector<CellRef>}`), with `ViewError::PromotionRefused` carrying enough (or
being paired with a separate `ArenaRefusal`/count) to render "(N cells)" in the refusal message.

**G5. The stretch-vanishing slide formula and the pack view's lane table are written for exactly two
in-plane spatial axes plus one depth axis, but V-R13 explicitly uncaps the number of spatial binding
points.** §9.1.3 step 3: "each placed in-plane axis in order with direction `u` and *perpendicular*
`v`" — well-defined for two perpendicular in-plane directions (x, y), not for three or more
simultaneously-bound spatial-role axes, since "the perpendicular" to a direction in a 2-D plane is
unique only when there is exactly one other in-plane axis. §9.3.6 similarly only discusses "a second
in-plane axis." Nothing in §7.3 or §9 says what either view does if a reader configures a fourth
spatial-role binding point (nothing stops them: the defaults are x/y/z/u/t, but roles are
reader-assignable per binding point). *Recommended resolution:* state explicitly that stretch
vanishing and the pack view's lane table use at most two in-plane spatial axes (by convention, the
first two bound to spatial-role points in binding-point order) plus one depth axis, and that
additional spatial-role points are shown on the compass and move the cursor but are not placed by
these two views — consistent with how all-dim walk already treats non-spatial roles (§7.3's last
paragraph).

**G6 (secondary).** §7.8 never states whether movement over view-owned dimensions
(`EnterPack`/`LeavePack`/`NextLane`/`PreviousLane`/`Retrieve`) should be fed to
`WalkRecorder::step()`. Recommend: no — only `MoveKind::AlongAxis` steps resolved to a *real*
dimension should be recorded, since "most used/most likely" (§7.8) is explicitly about dimensions a
reader might bind to an axis, and `d.pack`/`d.packing` are never bindable.

**G7 (secondary).** §9.3.4's claim that "focusing either \[occurrence of `e1`\] highlights both" has
no stated join key or cost. It is almost certainly "compare `resolveReal()`/`target()` results
across currently-visible occurrences," but the spec should say so and should note the cost is
bounded by the derivation window (§6.7), not by total walk length.

**G8 (secondary).** §8.11's `messageKey(ViewError)` cannot, on its own, produce every row of §12.2's
message table — several rows ("Nothing that way," "No pack that way," "Rebind tossed view cells," "A
saved binding no longer exists," "Deleting a group in use," "Pages still arriving") carry "none" in
the Error column, meaning they are informational strings assembled by the host, not keyed by a
`ViewError`. The spec should name the second mechanism (a broader `ViewMessage` key set, or
explicitly "host-formatted, not looked up").

______________________________________________________________________

## 4. Hidden work

The prompt named six specific areas; each is addressed directly.

**H1. Settings and keymap seeding volume, and whether it touches `tests/samples/`.** §12.3 lists on
the order of 100 tunables (`view.*`, `stretch.*`, `ring.*`, `pack.*`, `rank.*`, `page.*`, `stack.*`,
`activity.*`, `subspace.*`). Today's pattern (`apps/common/xanadu/system_docs.cpp:524+`) is a
hand-written `switch (kind)` in `defaultSettingSpecs()` with one `settings::k...` name constant and
one `SettingSpec{name, notes, schemas}` literal per tunable — there is no dynamic path yet from a
`ViewDescriptor.settings` vector (§8.1's `add()` says the host "seeds each descriptor's settings and
chords into system://settings and system://keymap when it is added") into that switch. Building that
dynamic seeding path (so a third-party view's settings land the same way the built-ins' do) is
itself new engineering, not just filling in ~100 literals. **This does not touch `tests/samples/`'s
binary fixtures or `CompactOpNode`**: settings live in the `system://` stores (seeded at runtime via
`ensureSetting`/`ensureAllSettings`), not in the sample fixtures under `tests/samples/xudu|xuzz/`,
and none of the proposed `ArenaManifold` changes touch `CompactOpNode`, `TrailEntry`, `CellSlot`, or
`Mark`'s layout — so the AGENTS.md rule about regenerating `tests/samples/` on a `CompactOpNode`
layout change does not apply to this work at all. Flag the settings/keymap seeding volume as real,
scoped, incremental work (one or two settings/keymap entries added per view package, not a single
giant commit), and flag the dynamic-seeding mechanism itself as a small sub-package inside package 3
(`view.hpp`/`ViewRegistry::add()`).

**H2. How `DimRef` names are read without a store — resolved, not a gap.** Covered in §1's secondary
findings: `ArenaManifold::textOf()`/`dimensionNamed()` fall through to `base_->store()` even when
the arena's own `store_` is null, which is exactly a `ViewManifold`-owned arena's situation. No new
plumbing is needed to read a real dimension's display name from either the binding or derived arena;
implementers should simply call `space.base().textOf(dim)` or `arena.textOf(dim)` and rely on the
existing fallback chain, and should **not** be tempted to thread an explicit `SpanReader`/`Store*`
through every view API just to make this work.

**H3. How the base `Manifold`'s lifetime and `Store::advance()` interact with a `ViewManifold`.**
`Manifold::advance()` mutates the same object in place (one Structure op per call;
`manifold.cpp:593-611`); `Manifold::advanceOrRefold()` falls back to
`*this = store.rebuildManifold(version)` on a refusal — also an in-place assignment, same object
identity, new contents. Since `ViewManifold` holds `const Manifold &base_` by reference (and each
inner `ArenaManifold` holds the same object by pointer), the identity a `ViewManifold` was
constructed over survives both paths, and reads through `base_` are always current (no caching) — so
far, so good, and this is why §6.2's "reads of real structure... are always current" holds. What the
spec does **not** say: after an advance/refold, cached real `CellRef`s the binding arena points at
via occurrences (an axis's bound dimension, a group member) or the `SliceCursor`'s `origin` may no
longer be `base().contains(...)` — for example after a branch switch, or (if a future delete-style
Structure verb is ever added) after the named cell stops existing. §12.2 has a message for this ("A
saved binding no longer exists") but frames it only as a **reattach-time** (persistence-replay)
event, not something `prepare()` must re-check every frame for a *live* session. **Recommended
resolution:** state explicitly that `prepare()` (or a check the host runs just before it) must
re-validate every cached real `CellRef` the binding arena and the cursor hold against
`base().contains(...)` on every call, and treat a now-unresolvable one exactly like the
reattach-time case — not just at session start. See Spike S3 for the companion question (how
expensive is a hypertime rebuild) that this interacts with for the `t` binding point.

**H4. The link-occurrence-to-page mapping needed for `PageCatalog`/`LinkEnd`.** As found in claim 7:
no byte-extent → (page, top-px, bottom-px) helper exists today. This is `apps/common/ui/view/`'s
(the presenter's) responsibility to implement against `page_view.hpp`'s `PageCatalog` interface,
which is in my area only as the *contract* — but the engine-side `PageCatalog`/`LinkEnd` types
should document the precondition clearly (an `Extent` may span a page break, in which case it names
more than one `LinkEnd`), since that shapes what "one `LinkEnd` per member per page it touches" must
mean. Flag this dependency to whoever builds the step-6 presenter early; the engine's fake
`PageCatalog` for tests (hand-supplied page/height mapping) sidesteps the problem for packages
15-19's own tests, but doesn't make it go away for real use.

**H5. Group-name storage and nested-group persistence replay.** §7.1: "a group's content is its
name" — the display name lives as the group cell's own text in the *binding* arena, which (per V7)
is never persisted directly; only `system://layout`'s per-slice `(name, [member names])` tuples are.
§7.5's one paragraph on replay ("a name that no longer resolves is skipped and reported") does not
address **order**: if group `B` contains group `A` (both named, per the nested-group example in
§7.2), replaying the persisted tuples in storage order could try to create `B` before `A` exists.
**Recommended resolution:** persist and replay groups in a topologically-sorted order (leaves
first), and state this explicitly in §7.5 rather than leaving it to be discovered when the first
nested-group persistence test is written (package 5's undo/redo tests don't exercise persistence at
all — this is a gap that would otherwise surface only once a persistence package is built on top of
package 5, which is not yet in the plan's package list and should be added once VU1 and the nested
case are settled).

**H6. Persistence replay reporting and error message keys.** Following directly from H5 and Gap G8:
§7.5 says a skipped name is "reported," with no destination named, and §12.2's message table has no
row for a batch summary ("N bindings restored, M skipped") — only a per-axis "A saved binding no
longer exists." Recommend adding that row, and deciding (per G8) whether it is `ViewError`-backed or
a separate, broader message-key enum before packages that touch persistence are written.

______________________________________________________________________

## 5. Spikes

**S1. Does `release()` on a view-shaped arena actually run in constant time once the guard lands?**
*Method:* extend `tools/layout-latency-probe.cpp` (or a throwaway harness first) to mint 0, 10³ and
10⁶ ephemeral cells into a `base`-having, `store`-less `ArenaManifold` with no spaces attached, call
`mark()`/`release()`, and time it; separately, instrument (or temporarily log)
`dimensionNamed("d.store-refs")` call sites to confirm zero full-dimension scans happen when
`spaces_.empty()`. *Pass criterion:* wall time flat across the three sizes (within measurement
noise), and zero scans observed. This is the load-bearing performance claim (V1, I4, V-R9) the
entire view-space design rests on — it should be the very first thing built and measured, before
package 4 is trusted.

**S2. Does a cold-start, fixed-25-step `TensionLayoutEngine` call reproduce what today's warm,
continuously-stepped engine converges to?** *Method:* for the handful of link scenarios
`tests/xudu/tension_layout_test.cpp` already covers (and the two/three/many-document cases
`beams.cpp` actually drives), run a fresh `TensionLayoutEngine` with zero initial velocity for
exactly 25 steps at the fixed `dt`, and diff the resulting positions against a reference captured
from today's live, multi-frame `beams.cpp` path for the same static scenario. *Pass criterion:*
positions agree within `page.base.levelTolerance` for every hand-built scenario. A failure here
means `CoalesceStrategy`'s default implementation needs more than "wrap the existing engine," before
package 16 is designed in detail.

**S3. How expensive is `rebuildManifold()` at realistic sizes, and does hypertime stepping (`t`)
need a cache?** *Method:* using one of the existing larger fixtures
(`tests/samples/xudu/multimedia`, or the KJV text `tools/benchmark-kjv-load.py` already loads),
measure `Store::rebuildManifold(olderVersion)` wall time for targets a handful of operations behind
the head. *Pass criterion:* a number that answers VU12 concretely — either "a cold refold is fast
enough that `t` can always refold on demand" or "a small per-pane LRU of recent folds is needed
before `t` ships." This retires VU12 and tells package planning (not yet in the table above, since
`t`'s role implementation is explicitly gated on VU11/VU12) whether it needs a caching sub-package.

**S4. Does the two-arena design actually keep binding-layer and derived-layer refs from
cross-dispatching when their numeric values collide?** *Method:* as the very first test in package
4, deliberately construct a binding-arena `ViewCellRef` and a derived-arena `ViewCellRef` with the
same `.ref` value (both `ephemeralBit | 0`, i.e. each arena's first-minted cell), and drive every
`ViewManifold` API (`linked`, `target`, `resolveReal`, `link`) with both, asserting neither is ever
answered from the wrong arena. *Pass criterion:* no wrong-cell read, and (once G1 is resolved) the
mismatched-`Layer` case is refused rather than silently misrouted. Cheap (well under a day) and
retires a subtle correctness risk before packages 5-14 are built on top of package 4.

**S5. Is a naive per-`prepare()` scan good enough for occurrence cross-highlighting (G7), or does it
need an index?** *Method:* build a synthetic manifold with high valence (50-100 dimensions on one
cell, matching all-dim walk's "no cap on valence") and a pack rank walked several hundred steps
deep, then compare the cost of a naive O(visible²) "which other visible occurrences share this real
cell" scan against building a small `unordered_map<CellRef, vector<ViewCellRef>>` once per
`prepare()`. *Pass criterion:* a number informing whether package 7/11's `prepare()` needs the index
proactively, or whether "visible" (bounded by the derivation window, §6.7) is small enough that
naive is fine.

______________________________________________________________________

## 6. Testing

**Mapping each §9/§10 acceptance list to a concrete file** is already given per-package in the table
in §2 above (the "Tests" column names the exact file and what it must assert); this section covers
the cross-cutting questions the prompt asks about.

**Fixtures: a correction to "hand-built manifolds."** `zigzag::Manifold` has **no public
cell-minting API** — cells exist only as folded `OpKind::Structure` operations (confirmed: no
`makeCell`/`link`-that-creates on `Manifold`, only `link()` between *existing* cells and the whole
`applyStructure` fold path). A "hand-built manifold" for an engine test must therefore be a small,
in-memory `xanadu::Store` built via its own Structure API (`sliceGenesis`, `makeCell`,
`makeScalarCell`, `setLink`, `addLink`, `makeDimension`), then turned into a real `Manifold` with
`store.rebuildManifold(version)`. This is exactly the pattern `tests/xuzz/two_by_three_fixture.hpp`
already establishes for a text/link fixture (`makeTwoByThree()`: build a `Store`, call
`sliceGenesis`/`makeCell`/`addLink`, expose the resulting `CellRef`s). Recommend a sibling
`tests/xuzz/manifold_fixtures.hpp` with a handful of named builders mirroring the spec's own worked
examples — a "person" cell with `d.email`/`d.phone`/`d.address` neighbours for the pack-view
examples (§9.3.4), a high-valence cell for all-dim walk, a small ring-shaped dimension for
rank-wraparound tests — each built the same way (`Store` → Structure API → `rebuildManifold`), and
shared across packages 7-14's tests the way `two_by_three_fixture.hpp` is already shared across the
existing navigation tests.

**Fake `PageCatalog`.** `page_view.hpp`'s `PageCatalog` is already specified as a pure virtual
interface the presenter implements over real `Doc`s and a test implements by hand (§8.6: "a test
implements it by hand"). A single `tests/xuzz/fake_page_catalog.hpp` providing a
`std::vector<DocumentFacts>`/`std::vector<PageFacts>`-backed implementation, constructible inline
per test with designated initializers, is enough for every page-view acceptance test (packages
15-19) — no store, no library, no device.

**Testing the O(1) toss honestly.** A `TEST` cannot responsibly assert "constant time" by timing
inside a unit test (flaky under load, under sanitizers, under CI). The honest split already implied
by the spec's own §6.5 ("the preconditions are the proof") is: the **unit test** asserts the
*preconditions* that make `release()` loop-free (`shadowCount() == 0`, `trailSize() == 0`,
`spaceCount() == 0` on the derived arena, both before and after minting 0/10³/10⁶ cells and tossing)
and that every earlier `ViewCellRef` is refused afterward (`StaleEpoch`) — this is package 4's
`view_manifold_test.cpp` and package 0's extension to `arena_manifold_test.cpp`. The **timing**
claim is separately retired by Spike S1 and, longer-term, by `tools/layout-latency-probe`'s per-view
mode (§14 of the spec already calls for this); timing belongs in a probe whose output is read by a
human reviewing a percentile report, not in a pass/fail gtest assertion.

**Golden layout fixtures with the text raster.** §8.7's `rasterise()` and §16.2's "golden layouts"
point to storing, for each view, a few representative inputs with their `LayoutSink` dumped both as
raw numbers (positions/sizes/opacity, for exact-equality regression) and as the text raster (for a
human reviewer to read a layout change as a picture), under
`tests/samples/view/<view-kind>/<case-name>.{txt,raster.txt}` or similar. Concretely:

- Build each case from the `manifold_fixtures.hpp` builders above (or a hand-written `PageCatalog`
  for page views), run `prepare()` + `layout()` once, and serialize the resulting `LayoutSink` (a
  simple one-line-per-record text format is enough — this is new code, maybe 100 lines, and belongs
  beside `raster.cpp` since it is the same "dump records as text" idea with full precision instead
  of a character grid).
- Regenerate **only** when a layout rule changes on purpose (matching the spec's own instruction),
  via a small `tools/`-style regenerate script, *not* via the full `create-sample-xanadocs.sh`
  pipeline — these fixtures have nothing to do with `CompactOpNode`/`ops.nodes` and must not be
  confused with the xanadoc/zigzag sample stores under `tests/samples/xudu|xuzz/`. State this
  distinction explicitly in the test README/comments so a future contributor doesn't reach for
  `create-sample-xanadocs.sh` by habit (see AGENTS.md's fixture-regeneration rule, which is about a
  completely different binary format).
- Diff reviews read the raster file in the PR; the numeric file is what the `TEST` actually compares
  byte-for-byte (or field-for-field with a tolerance for floats).

**Purity and no-ops tests, concretely.** Package 4 (or a small shared test-support header) should
provide the two cross-cutting helpers the spec's §16.2 names: a `layoutTwice(view, in)` helper that
calls `layout()` twice and compares the two `LayoutSink`s for equality (used by every one of
packages 9-19's test files, not re-implemented five times), and `expectNoOpsAppended(store, fn)`
wrapping `fn` and comparing `store`'s operation count/byte size before and after — used by every
test that drives `move`/`bind`/`switchView`/`transition` through a real (tiny) `Store` built via the
fixture pattern above.
