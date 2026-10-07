# Delivery challenge: `design/view-system-implementation-plan.md`

Reviewed: the plan, `design/view-system.md` (§1-4 skim, §9-12, §17-19 full),
`design/world-space-rendering-plan.md` (full), `AGENTS.md`, `git log --oneline -60`, the four expert
reports (`01-engine.md` … `04-aesthetics.md`), `design/ui-text-fit-baseline.md` and the batch
commits, and line/byte counts of the code under discussion. Read-only; nothing built or run.

## 0. The yardstick

The UI text-fit series (`ui-text-fit-baseline.md` → `batch9.md` → `review-followup.md`) is the most
recent comparable effort: fitted text, shaping cache, boxed canvas, retained overlays, migrating ~25
widgets/overlays onto it. Its 8-9 headline commits (`7007d34`, `9a9f5ec`, `17387b0`, `4b63aef`,
`359484e`, `d7a0a87`, `21ecda4`, `ced2287`) total **≈19,600 lines inserted** (impl + tests), landed
across two calendar dates in this repo's history (2026-10-06/07) at the project's own throughput
(10-46 commits/day over its 936-commit history). That throughput is the real unit of account here,
not calendar weeks — this is an agent-paced repo, and "two days" in its git log is a dense,
continuous push, not two working days with meetings in between. Read every "weeks" or "days"
estimate below as *sessions of comparable density to the text-fit series*, because that is the only
empirical rate this repository has shown.

## 1. Size, honestly

Summing the two line-level expert estimates package by package:

- **Engine + page-view track** (`01-engine.md`'s packages 0-20, which cover the plan's E and P
  tracks): **≈12,700 lines.**
- **Library track** (`02-rendering.md`'s WP1-WP8, the plan's L track): **≈5,800 lines** (heuristic
  midpoints from the plan's own S/M/L bands where the reviewer didn't give a figure).
- **Presentation/host track** (plan's U1-U7, no line-level review commissioned): **≈5,150 lines** by
  the same heuristic.
- **Cut-over track** (X1-X4): **≈3,100 lines new**, against **≈8,000-9,000 lines deleted or gutted**
  (`zigzag_visualizer.{cpp,hpp}` 5,047 lines deleted outright; large fractions of `beams.cpp` 2,216,
  `views.cpp` 2,068, and `unified_transclusion_engine.cpp` 724 retired).
- **Relocation** (A1): no new logic, but **24,536 lines** (`apps/xudu` + `apps/zigzag`) physically
  moved, every include spelling in ~20 files rewritten, five Makefile sections and one packaging
  script edited.

**Total new implementation + test code: ≈26,700 lines**, roughly **1.4× the text-fit series**,
before counting ~100 settings' schema/notes prose, ~30 new default chords, and the golden-layout
fixtures §16.2 and the aesthetics review both call for. That is not a "few months" project by this
repo's own cadence; it is several text-fit-series multiples, i.e. plausibly 60-120 commits at this
project's historical commit size, which the plan's own §7 risk register half-admits ("Scope: this is
many months of work") without ever putting a number on it. **Finding: the plan should state the
≈26,700-line, ~1.4×-text-fit figure explicitly in §1 or §7, not leave size as an unquantified risk
row.** *Recommendation the author can accept or refuse in one line: add the line-count rollup above
to §7 as a named, dated estimate, re-estimated after M2 lands.*

### Estimates I do not believe

1. **E4 (`view_manifold`) is marked M in the plan's own §4.4 table; the commissioned estimate for
   exactly this package is ~1,100 lines — over the plan's own L threshold (1,000).** This is not a
   rounding question: §4.4 calls E4 "M" while `01-engine.md` calls it "L" and separately flags it as
   "the centerpiece everything else sits on" and the single highest-risk package in the whole engine
   track (two arenas minting numerically colliding refs). The plan's own size band undersells its
   riskiest package. *Recommendation: re-tag E4 as L in §4.4; this changes nothing about its
   position in the dependency graph, only the expectation set for whoever picks it up.*
1. **E7 (`pack_rank` + `pack_presentation`) is marked M; the commissioned estimate is ~500+400 impl
   plus ~400 test ≈ 1,300 lines — also over the L threshold.** It is two files bundled into one row,
   which is where the miscount comes from: `pack_rank.{hpp,cpp}` alone might be M, but
   `pack_presentation` is shared chrome every later pack-showing view depends on (ghosts, strands,
   spread, the glue look) and the visual reviewer separately flags it as the single biggest
   coherence risk if built late or piecemeal. *Recommendation: split E7 into two rows — `pack_rank`
   (M) and `pack_presentation` (M, but called out as a hard prerequisite, not just a dependency, for
   E9-E11's visual coherence; see Finding 9).*
1. **A1 "relocation" is sized M.** Line-count-wise that is about right for the diff itself (mostly
   `sed` and Makefile edits), but the size band hides that `03-build.md` found the spec's own step-1
   destination table is **factually wrong for two files** (`link_context.*`,
   `unified_transclusion_engine.*`) in a way that would break the **link** step of ten build targets
   at once if executed as literally written. An M-sized package whose main risk is "this breaks ten
   targets if one `git mv` target list is wrong" deserves a higher risk flag than its size band
   implies, independent of how many lines move. *Recommendation: keep A1 at M for size, but add an
   explicit "corrected destination table" sub-item citing `03-build.md`'s table before this package
   is allowed to start.*
1. **The ~100 settings + ~30 chords are costed as "tedium," not as engineering.** `03-build.md`
   found that the dynamic path from a `ViewDescriptor.settings` vector into
   `defaultSettingSpecs()`'s hand-written switch **does not exist yet** — building it is a small
   sub-package, not just filling in literals one at a time as the plan's §4.4 prose implies.
   *Recommendation: give that seeding mechanism its own row (owned by whichever package first needs
   more than one or two settings — in practice E9, the first slice view) rather than folding it
   silently into "added per view."*

Comparing the ≈26,700 new lines against what is replaced (≈8,000-9,000 lines of
`ZigzagVisualizer`/`LinkBeams`/ `Views`/`UnifiedTransclusionEngine` logic) gives a **~3× expansion
ratio** — reasonable for going from "one closed visualizer with three fixed axes" to "an open
registry with uncapped dimensions, groups and roles," but the plan should say this is a deliberate
3× cost of generality, because stated baldly ("we are writing three lines of new code for every line
we delete") it is a decision the owner should make explicitly, not discover midway.

## 2. Value order

### Is A1 (relocation) rightly first

Yes, but for a sharper reason than "it's independent and mechanical": the plan's own §6 gate table
requires **"no file under `apps/xudu/` or `apps/zigzag/`"** at *every* package's gate, which is
simply false until A1 lands — both directories are non-empty today. Read literally, no package can
pass its gate before A1 completes, which is actually the right dependency, just unstated as such.
*Recommendation: say so explicitly in §6 ("this clause is vacuous, and every other gate is blocked,
until A1 lands"), and fix §17 step 1's two misrouted files per `03-build.md`'s corrected table
before A1 is attempted — the two engine/E-track reviewers independently confirmed the current
destinations break linking, not just layering.*

### Should "first light" (M1) really be stretch vanishing beside the legacy view

**M1 retires engineering risk; it does not deliver anything a reader would choose to use.** The
milestone literally ships the new view "beside the legacy view, in one pane" with the legacy view
staying the default. That is valuable — it proves registry, view space, binding, prepare/layout, the
presenter, picking and accessibility end to end on cheap primitives — but it is an *internal*
milestone wearing a user-facing name. Nothing in M1 changes what a reader who has not been told to
go find the new view palette entry will ever see. **Recommendation: rename M1 to what it is ("engine
spine proven") and do not report it to stakeholders as a release; reserve "first light" language for
the first milestone a reader notices without being told where to look** (see the revised table
below).

### Is stretch vanishing the right thing to notice first, or should something else be

The genuinely novel, immediately legible capability this system adds that the shipped app **cannot
do today** is the all-dim walk: today's `ZigzagVisualizer` hard-codes exactly three bound dimensions
(`ViewAxisBinding{x,y,z}`); a cell with ten links on ten different dimensions shows three of them.
All-dim walk, even at depth 0 with no neighbours'-wheels, shows every one, which is a capability
jump a reader notices on sight, not a re-skin. The cost is that it depends on `PlaneSet` (L6), the
single highest architectural-risk item in the whole plan per `02-rendering.md` (no VAO caching
today, re-issues every attribute pointer on every GL draw — "10,000 buffer binds... per frame" at
the probe's own stress ceiling). **That dependency is exactly why R1 (the draw-per-plane spike)
belongs before anything else, not after stretch vanishing is already shipped — the plan already
spikes it early, which is correct; what's missing is sequencing the *milestones*, not just the
spikes, so the wheel is the first thing reported as a release, with stretch vanishing and the pack
view staying "available, not advertised" until then.** This is a relabelling of the plan's own
milestone order more than a reordering of packages — see the revised table.

### Are the page views (parity-first) worth their cost before the slice views are done

The base view's first implementation step is explicitly a numeric-parity reproduction of what
`LinkBeams` already does (`page.base` "the base view is first checked for parity with today... then
switched to pages"), which by construction delivers **zero** visible improvement — it is a slower,
better-tested re-implementation of an existing behaviour, gated behind the plan's single
highest-likelihood-and-cost risk (`L7`'s reflow/pose interaction, flagged "high without F2," and
found by `02-rendering.md` to have *more* failure surface than the stated regression test
(`doc_gap_test.cpp`/`onion_skin_test.cpp`) actually exercises). Being a parallel track (P,
independent of E after E1) softens this — it doesn't block the slice views — but the *cost* is still
real: real engineering hours go into reproducing status quo before the windowing/overlap payoff
(V-R32/V36) that is the actual reason to rebuild `LinkBeams` at all. **Recommendation: don't ship
the parity step as a user-visible milestone at all — treat it as an internal checkpoint inside the
same package that lands windowing/overlap, so the first time a reader sees the new base view it
already does something the old one couldn't.** This folds M4's two P-track sub-steps into one
milestone in the revised table.

### Is the selector and compass (M7) too late

**This is the sharpest ordering problem in the plan.** V4's own ruling is "there is no fixed number
of axes," and V-R13 uncaps binding points — that is the central promise of this rewrite over today's
`ViewAxisBinding` three-field struct. But tracing the milestone table, the *only* way to bind
anything before M7 is: the wheel's drag-to-axis gesture or its keyboard form (`B` + axis name), both
introduced at **M3**. Before M3, M1 and M2 (stretch vanishing, packs) have **no stated UI for
choosing what is bound at all** — a reader is stuck with whatever the host wires up by default,
which is exactly the "three named fields" limitation this system exists to remove. Even at M3,
binding only reaches a *fourth* point (`u`) once a fourth binding point's role exists (E16,
explicitly last in its track, "nothing depends on it") and the *full* selector — groups, ranking,
the pouch — doesn't land until M7, five milestones after the view it is supposed to make usable. **A
reader who wants to bind `d.phone` to a second axis on a cell with forty dimensions has no way to do
it for four milestones' worth of calendar time.** *Recommendation: pull a minimal, ugly binding
mechanism into M1 — a typed/command-form bind (dimension name → axis name, no world-space selector,
no ranking, no groups UI) — decoupling "can the reader reach any dimension" (needed from M1) from
"is reaching it pleasant and ranked" (M7 can keep that). This is cheap: it's a thin wrapper over
`ViewAxisSet::bind()`, which package 5 already builds, exposed as a command before package 14 (the
selector) exists to make it a gesture.*

## 3. What to cut or defer

| Spec feature                                      | Verdict                                                                                        | Why                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| ------------------------------------------------- | ---------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `u`, `t` roles                                    | **DEFER**                                                                                      | E16 already "last in its track, nothing depends on it" per the plan's own risk row; VU12 (what `t` means unbound) is an open question, not a design. Cost of deferring: no subspace zoom, no hypertime step in v1. Fix needed regardless: §12.1's chord table lists `step.u`/`step.t` defaults with a dagger as if nearly shipped — say explicitly they ship with E16, not before.                                                                                                                                                                                                                                                 |
| Neighbours' wheels beyond depth 0                 | **DEFER**                                                                                      | Depth-0 wheel (direct neighbours only) delivers the whole "see every dimension" win; bend/flex/detail-under-pressure (§9.2.8-9) is real geometry work for a secondary view of a view. Cost: a reader can't see a neighbour's own wheel without stepping to it first — acceptable, that's what stepping is for.                                                                                                                                                                                                                                                                                                                     |
| Edge heat                                         | **CUT for v1**                                                                                 | It is already the view's *second* sub-view and explicitly a candidate ("settled by: use," VU15). Ghosts alone communicate "something is off-screen"; the sector-glow math is extra surface for a cue that duplicates what ghosts already say, not a new one.                                                                                                                                                                                                                                                                                                                                                                       |
| Nested packs                                      | **DEFER**                                                                                      | Flat (single-level) packs deliver the table-reading value; recursion (`pack.nestDepth`, a group containing a group) is rare in a first real workload and adds real layout complexity. Refuse/flatten nested groups at bind time until this lands.                                                                                                                                                                                                                                                                                                                                                                                  |
| The spread                                        | **KEEP**                                                                                       | Cheap (pure function of hover/sub-view, shared `pack_presentation` code) and *necessary* — without it a glued pack is unreadable as "which lane is which." Not a luxury feature.                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| Promotion of packs (`promotePack`)                | **DEFER**                                                                                      | Explicitly reader-invoked, never automatic, and its own return type is broken as specified (Gap G4, `01-engine.md`). Cost of deferring: no way to make a pack permanent; low — it's a convenience over an already-working retrieve.                                                                                                                                                                                                                                                                                                                                                                                                |
| Windowed pages                                    | **DEFER, phase it**                                                                            | Ship whole-page coalesce + lift + ghost/tether first (the parity-plus-lift improvement over today); add the window/band form in a fast-follow once that lands, since `01-engine.md` independently flags "participant/window/overlap" as the single most case-heavy acceptance list in the whole document. Cost of deferring: a link with many ends on large pages still can't all be seen in one pane for one more milestone — the same limitation that exists today.                                                                                                                                                              |
| The stacked view's search (§10.4.3)               | **DEFER**                                                                                      | Ship a fixed default stagger direction first; add the candidate search once V3's spike result is in hand and the basic deck is proven. The search is real, separable engineering (hysteresis, a few-hundred-candidate scoring grid) layered on top of a deck that works without it.                                                                                                                                                                                                                                                                                                                                                |
| Scenes with several placements (mixing)           | **KEEP, but don't rebuild it early**                                                           | This is *today's* unified mode — cutting it would be a regression, not a deferral. The plan already preserves it correctly by wrapping both legacy pieces as placements in one scene through step 6. Don't build the *new* cross-placement edge machinery (§11.2) until the single-placement slice and page views are both proven; the legacy wrapped mode covers the gap.                                                                                                                                                                                                                                                         |
| Embedding (§11.5)                                 | **DEFER**                                                                                      | No named user journey demands it urgently; it is its own scene/pane/depth-slice mechanism with real complexity and is explicitly listed as a "candidate," not a requirement.                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| Selector's tier 1 (groups) and the Markov ranking | **tier 1: DEFER the selector UI, not groups themselves; ranking: CUT the Markov model for v1** | Groups must exist for the pack view (M2) via the existing `group.new`/`group.edit` commands — that is not selector UI, it's a prerequisite for packs and ships with M2. The selector's *fancy* tier-1 presentation (fan-out, radius) can wait for M7. The Markov/decay ranking (`dimension_ranking.hpp`, V-R53) is statistically elaborate for a question the spec itself says is "settled by: use" (VU13) with no real activity data yet (VU5 — the activity store doesn't exist). Replace with "most recently bound, in ring order" (a trivial LRU) for v1; revisit with real telemetry once the activity store's design exists. |
| `PaneTree` / multi-pane                           | **KEEP, build early; defer exposure**                                                          | L8 is S-sized, zero dependencies, built directly on the already-tested `ui::split()` — cheapest, lowest-risk item in the entire plan per both expert reports. Build it whenever convenient (even before M1) since it costs nothing to have ready, but don't expose multi-pane to readers until the single-pane views are solid; each pane multiplies the QA surface (its own camera, cursor, bindings) and that surface shouldn't multiply before the thing being multiplied is proven.                                                                                                                                            |

**The minimal coherent first release**, reading the above as one set: relocation (corrected); a
minimal typed bind (no selector); stretch vanishing with ghosts only (no edge heat); all-dim walk at
depth 0 only (no child wheels); flat (unnested) dimensional pack view with the spread, no promotion;
page base view with whole-page coalesce/lift/ghost/tether, no windowing yet; one pane; legacy
visualizer and `Views` remain the default, new views reachable from the palette. That is roughly
M1+M2+M3+M4 of the plan's own table, pruned of edge heat, nesting, windowing and promotion — still
substantial (it needs E0-E9, E11 flat, L1/L5/L6/L9, L7's non-reflow-interacting half, P1-P3's
whole-page case, U2) but it is the smallest set that gives a reader three working, genuinely
different ways to look at a slice plus one real page-view improvement, rather than one hidden
toggle.

## 4. The riskiest assumptions, ranked

Ranked by probability of being wrong × cost if wrong, using the two expert reports' own evidence
rather than restating the plan's risk table:

1. **Page pose corrupts reflow (L7/F2).** Probability high — `02-rendering.md` found the hazard is
   *broader* than the plan's stated fix: reflow doesn't just overwrite a posed page's matrix
   (`doc.cpp:1273`), it also **reads back** a previous page's *live* matrix to place the next one
   (`doc.cpp:1241,1698`), so a posed page can corrupt every page stacked after it in the same reflow
   pass, not just snap itself back. Cost: high — a corrupted document on screen, not a slow frame,
   and it is load-bearing for M4, M5 and M6 (half the page-view track). The stated spike (R4, "does
   a page keep a matrix set from outside through a reflow") tests only the snap-back direction; the
   expert's own proposed Spike 5 (set a pose, edit elsewhere, check what happens to *later* pages)
   is the complementary half and is not in the plan's §3 spike table. **Top risk.**
1. **Draw-per-plane cost on OpenGL (`PlaneSet`/R1).** Probability high — confirmed today's GL device
   has no VAO caching and re-issues every vertex attribute pointer on every single draw call; this
   blocks M3, M4 (poses interact with the same translucency-sort machinery), M6 and M7. Cost high:
   if the number is bad the fix is architectural (per-instance transform packing), not a tuning
   pass. Already correctly spiked first; the risk is in the *milestone* order treating M3 as
   reachable before R1's answer is known, which the revised table below fixes by not promising a
   wheel milestone date until R1 reports.
1. **The two-arena numeric-ref collision (E4/G1/S4).** Probability moderate (it's real by
   construction — both arenas mint dense indices from zero) but mitigated by being the *first* test
   written in package 4. Cost if it ships wrong is severe: silent wrong-cell reads threaded through
   every slice view. Ranked below 1-2 only because the mitigation is already in the plan and is
   cheap.
1. **The `d.store-refs` guard is placed around the lookup, not just the loop (S1).** `01-engine.md`
   found the literal reading of the plan's own fix text ("guard the loop") would leave the
   expensive, allocating, O(real-dimension-count) `dimensionNamed()` scan unconditional — defeating
   the *entire* O(1)-toss claim (V1, V-R9) the view-space design is built on. Probability of getting
   this specific wording wrong in implementation: real, because the plan's own prose invites it.
   Cost: the foundational performance claim of the whole engine track quietly stops holding,
   discovered only under load.
1. **Translucency ordering across `PlaneSet`, page batches and `Beams` (no spike at all).** See
   Finding below — this is the missing spike, not just a risk.

### The missing spike

**None of S1-S5/R1-R4/V1-V3 tests cross-kind translucent draw order.** `02-rendering.md` found that
`PlaneSet` sorts its own planes back-to-front, pages sort back-to-front among themselves (once any
page is translucent), and `Beams` doesn't sort at all — and nothing unifies the three into one order
for a region. The plan's L6/L7 gate language ("a scene that forces the three to overlap is part of
L7's gate") only actually forces overlap among *page* batches; it doesn't cover a `PlaneSet` plane
and a page and a beam all genuinely overlapping at once, which is exactly the scene a wheel-and-page
mixed scene (M6) or a pack drawn near a windowed page (M4+M2 together) will eventually produce.
**Recommended spike, before L6 and L7 are both built (call it R5): one translucent `PlaneSet` plane,
one translucent page, one beam, mutually overlapping in depth; confirm blend order is
camera-distance-correct across backends, not call-order-dependent.** This is cheap (a day, headless,
no new mechanism needed to build it — just a scene) and far cheaper to run before both L6 and L7
land independently with their own private sort than to debug the interaction after the fact.

## 5. Parallelism and people

The plan's track diagram (§4.1) is accurate about E, P and L being file-disjoint, but it understates
two real collision points in *this* codebase:

1. **`apps/common/xanadu/system_docs.cpp` (3,423 lines) is the single file every settings-bearing
   package and every chord-bearing package must edit — and that is essentially every package in E, P
   and U.** `defaultSettingSpecs()` is one large per-kind switch; ~100 new settings and ~30 new
   default chords, spread across roughly 15-20 packages the plan intends to let proceed on separate
   tracks, all land as edits to the same function in the same file (plus its paired test,
   `tests/xudu/system_docs_test.cpp`, 906 lines). This is a mechanical merge-conflict generator the
   plan does not mention. *Recommendation: either serialize settings-adding commits through one
   rotating "settings landing" slot, or split `defaultSettingSpecs()` by view-kind into separate
   functions/files now, before the volume arrives, rather than after the first few conflicts teach
   the lesson.*
1. **§5.6's visual-review sequencing contradicts §4.4's parallelism claim.** §4.4 says "E9 and E10
   are independent once E8 lands." §5.6 says the shared ghost/seam/strand/focus-mark chrome must be
   "written once, in `pack_presentation` and a small shared chrome helper... before E9 to E11," i.e.
   before E9's and E10's goldens are fixed — which means E9 and E10 are **not** actually safe to
   start as soon as E8 lands if E7 (which carries `pack_presentation`) hasn't landed its
   shared-chrome half yet. One of these two statements needs to change: either extract the shared
   chrome into its own package ahead of E7/E8/E9/E10 so the parallelism claim holds, or admit E7's
   chrome half gates E9/E10 and remove "independent" from §4.4.
1. **The presentation track (U) is the real fan-in point, not a parallel track.** U2 depends on E9
   and A1; U3 depends on L7 and P3; U4 depends on L3, L4, L6 and L8. Every one of E, P and L
   converges on `apps/common/ui/view/` and, at step 6, on `xuzz_app.cpp` (2,915 lines) and
   `view_coordinator.*`. That is exactly where multiple contributors finishing different tracks at
   different times will collide on the same few files at once — worth calling out explicitly as "the
   squeeze point," rather than letting the parallelism language in §4 imply the whole plan is
   embarrassingly parallel when one step (U2/step 6) is a hard synchronization barrier.

## 6. Legacy coexistence

**The window is the whole plan.** §4.7 says legacy tests "run until X3," and X3 is milestone **M8**,
the last milestone before the closing M8's own sub-steps. By the size estimate in §1, that is on the
order of 60-120 commits at this repo's pace — not weeks, but a genuinely long coexistence window by
any standard. The cost is not primarily extra CI time (the full build already always links and tests
both paths, since `COMMON_UI_OBJS` is one undifferentiated object list) — it is:

- **Double behavioural surface for one concept.** `LinkBeams`' coalescing and the new
  `CoalesceStrategy` implement the *same idea* (bring linked passages together) in parallel for the
  entire window; nothing forces them to agree on edge cases beyond the one parity check at P3, and
  `02-rendering.md`'s Spike 5 shows the two are not even isolated from each other — `LinkBeams`
  calls the *same* `Page::setModel()`/`reflowFrom()` code the new pose work is changing, so "legacy"
  is not a frozen target during this window; it is exposed to every library change the new work
  makes.
- **No stated exit if the new views aren't good enough.** The plan assumes X3 (deletion) happens
  once every legacy test is "re-homed or retired with a reason." There is no go/no-go checkpoint, no
  stated fallback if, say, the base view's windowing genuinely reads worse than `LinkBeams` on a
  real link-dense document, and no costed "keep both indefinitely" contingency.

*Recommendation: add an explicit go/no-go review at M7 (before X3 is scheduled), with §5.5's
acceptance checks run on real fixtures as the pass bar; if it fails, the fallback is "both paths
stay live, re-scope X3," stated as a real option in §8's owner-decision list, not an implicit
assumption that cut-over always happens.*

## 7. Definition of done, rewritten where vague

The milestone table (§4.1) names packages per milestone but states no user-observable "done" for any
of them — that has to be reconstructed from §5.5, §5.6 and the individual packages' acceptance
lists. Three examples, rewritten:

- **M1** (current: "stretch vanishing beside the legacy view, in one pane") → *"A reader can switch
  the active pane to stretch vanishing via the palette, read real cell content with edge ghosts,
  step along at least two bound dimensions by chord, and switch back with no data loss; §5.5 checks
  1, 2, 3, 5, 6, 9, 13, 14 pass on a captured frame; S1 and S4's pass criteria hold in the shipped
  binary, not only in the spike harness."*
- **M3** (current: "the wheel in three dimensions") → *"From any real cell of valence ≥ 8, all-dim
  walk shows every neighbour on every dimension, correctly opposite-paired, bound axes on R/U/F;
  drag-to-axis and the keyboard form produce the same rebind; §9.2.11 and §5.5 checks 1, 4, 5, 6,
  7(human), 9, 13 pass; R1's draw-cost gate holds at the wheel's own stated plane count (~1,300 at
  depth 2, valence 20) within frame budget on `llvmpipe`, not only on a real GPU."*
- **M8** (current: "cut-over: the legacy presentations removed") → *"Every test in
  `test_visualizer.cpp` and `world_presentation_test.cpp` is re-homed against the view that replaced
  its behaviour, or retired with a one-line reason in the commit; `ZigzagVisualizer` and
  `LinkBeams`' arrangement code are deleted; the legacy placement no longer appears in the view
  registry; full `make check` plus one `compare-backends.sh` scene per view passes; AND the
  §6-of-this-report go/no-go checkpoint was explicitly affirmed at M7, not assumed."*

______________________________________________________________________

## Proposed revised milestone order

| Milestone                                                   | Contents                                                                                                                                                                                          | What a reader sees                                                                                                                                        | What risk it retires                                                                                                                                   |
| ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **M0**                                                      | Spikes S1-S5, R1-R4, V1-V3 (unchanged from the plan, but run as one batch before any package starts, including A1)                                                                                | Nothing — internal                                                                                                                                        | Every foundational numeric/performance claim the rest of the plan rests on, before a line of production code is written against them                   |
| **M0.5**                                                    | A1, corrected per `03-build.md`'s destination table (both misrouted files go to `apps/common/ui/`, not `apps/common/xanadu/`)                                                                     | Nothing visible; tree still builds and behaves identically                                                                                                | The relocation-breaks-linking risk, with the gate in §6 now actually satisfiable                                                                       |
| **M1 — "engine spine proven"** (renamed from "first light") | E0-E6, E8, a **minimal typed bind command** (pulled forward from M7, no selector UI), stretch vanishing with ghosts only, U2, in one pane beside the legacy view, reachable only from the palette | Nothing by default; an engineer/tester can switch to it and read real cell content on any dimension they can name                                         | Registry, view space, toss, binding mechanics, presenter/picking/accessibility path, end to end                                                        |
| **M2 — packs**                                              | E7 (both `pack_rank` and `pack_presentation`, re-tagged L), flat/unnested dimensional pack view with the spread, no promotion; still palette-only                                                 | A reader with a group already defined (via `group.new`) can see it as a glued, spreadable table                                                           | Shared ghost/seam/strand chrome exists before the next two views need it, resolving the §4.4/§5.6 contradiction                                        |
| **M3 — "first light" (renamed)**                            | L1, L5, L6 (gated on R1's result), L9, E10 at depth 0 only, E12; promoted to default-visible in the palette                                                                                       | The first thing a reader notices unprompted: every dimension a cell is linked on, not just three, shown as a wheel                                        | PlaneSet draw-cost risk (R1) and beam-along-Z (R3), with real production exposure, not just a probe                                                    |
| **M4 — pages, once**                                        | L7 (gated on the expanded R4 + the expert's Spike 5), P1-P3 for the **whole-page coalesce + lift + ghost/tether case only** (parity folded in as an internal checkpoint, not a shipped step), U3  | Links bring pages together by page, not whole document, with the same look as today plus a tether — a visible improvement over `LinkBeams`, not a re-skin | The single highest-probability-and-cost risk (page pose vs. reflow), confirmed safe before any further page-view work builds on it                     |
| **M5 — many-ended links**                                   | Windowed/overlapping pages (V-R32/V36) added to the M4 base view                                                                                                                                  | Links with many ends, or on large pages, can finally all be seen in one pane                                                                              | The most case-heavy acceptance list in the spec, built on a base view already proven safe                                                              |
| **M6 — decks and multi-pane**                               | P4/P5 with a **fixed** stagger direction (search deferred), L3/L4/L8/U4, `PaneTree` exposed                                                                                                       | A document reads as a receding deck; two scenes can sit side by side                                                                                      | Multi-pane's QA-surface multiplication, taken on only after single-pane views are proven; deck rendering proven before its direction-search refinement |
| **M7 — reachability**                                       | V3's search added to decks; the full three-tier selector, compass, groups UI; dimension ranking as a plain recency list (Markov model cut, see §3)                                                | Any dimension, not just the ones a reader can type from memory, is reachable and bindable without the typed-command fallback from M1                      | The "binding model is useless beyond three axes without a picker" gap, closed five milestones earlier than the current plan                            |
| **M8 — cut-over**                                           | X1-X4, gated on the explicit go/no-go review of §6                                                                                                                                                | The legacy visualizer and `LinkBeams`' arrangement code are gone; one coherent set of views remains                                                       | The double-maintenance tax of the whole coexistence window, closed deliberately rather than assumed                                                    |

## Deferral list (summary)

**CUT for v1:** edge heat; the Markov/decay dimension-ranking model. **DEFER to a fast-follow:**
`u`/`t` roles; neighbours' wheels beyond depth 0; nested packs; pack promotion; windowed/overlapping
pages (ship whole-page coalesce first); the stacked view's stagger search (ship a fixed direction
first); embedding; the selector's tier-1 fan-out presentation (groups themselves ship with M2 via
existing commands, just not through selector UI). **KEEP, not negotiable:** the spread; mixing via
wrapped legacy placements (it's today's behaviour); `PaneTree` (build early, expose late).
