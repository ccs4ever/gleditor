# Red-team challenge: `design/view-system-implementation-plan.md`

Reviewed against `design/view-system.md`, `design/world-space-rendering-plan.md`, `AGENTS.md`,
`.claude/rules/architectural_governance.md`, the four expert reports in this scratchpad directory,
and the tree at the commit the documents themselves cite. Every finding below was checked against
the real code or the real document text; `path:line` citations are given so each can be re-verified.

______________________________________________________________________

## BLOCKER

### 1. The plan claims a spec correction that was never made

**Evidence.** `design/view-system-implementation-plan.md:29` (row F1) states the consequence of the
finding as fact: *"Everything in `apps/xudu/` goes to `apps/common/ui/xanadoc/` and everything in
`apps/zigzag/` to `apps/common/ui/slice/`, with no exceptions… **The spec's step 1 is corrected.**"*
§1 of the plan frames the whole of §2 this way: *"Each item is either already corrected there \[in
the two documents\] or is a decision a named work package must make first"* (`...plan.md:13`).

But `design/view-system.md:2880-2889` (§17 Step 1, unchanged) still reads:

> **To `apps/common/xanadu/`** — no graphics header is included: `apps/xudu/link_context.*`; and,
> under `zigzag/`, `apps/zigzag/unified_transclusion_engine.*`.

Both claims in that sentence are false as the code stands: `unified_transclusion_engine.hpp:28-30`
includes `gleditor/doc.hpp`, `gleditor/glyphcache/cache.hpp` and `gleditor/render/stream_buffer.hpp`
directly, and its public API (`stageVisibleCells`, `stageIntoStreamBuffer`, lines 262-274) takes a
`gleditor::GlyphCache&`. `apps/xudu/link_context.hpp:31,67-70` includes `xudu/session.hpp` and takes
a `Session&` as its only non-trivial constructor argument. Routing either into `apps/common/xanadu/`
(`COMMON_XANADU_SRCS`, `Makefile:603`) would put it in `XUDU_CORE_OBJS`, linked by `xudu_test`,
`xuzz_test`, `vquery`, `vpl`, `vprolog`, `vqueryc`, `vplc`, `xudu-swarm-peer` and all three fuzz
targets (`Makefile:605,622,894-899,915,921,934,952-953,969-990`) — **none of which link
`$(LIBLINK)`/`$(APP_LDFLAGS)`** (the Makefile's own comment at `Makefile:888-892` calls this link
line the architecture check). This was independently confirmed by the build review (`03-build.md`
§1.1).

The implementation plan's own A1 package (`...plan.md:121`, "git mv **all** of `apps/xudu/`… and
**all** of `apps/zigzag/`…, with no exceptions") happens to do the right thing in spite of this —
but a reader who trusts the spec text itself (which `AGENTS.md` explicitly tells agents to consult
before a non-trivial change in this area) will misplace two files and break the link of ten targets
simultaneously, the exact failure mode `03-build.md` demonstrates line by line.

**Fix.** Edit `design/view-system.md:2880-2889` to remove the two-file carve-out (both files move to
`apps/common/ui/`, with no exception), and correct §1's framing so it does not assert document edits
that were not made. Do this before A1 is executed by anyone reading the spec rather than the plan.

______________________________________________________________________

### 2. M3 ("the wheel in three dimensions") lists no package capable of drawing it

**Evidence.** The milestone table (`...plan.md:102`):
`M3  the wheel in three dimensions L1, L5, L6, L9, E10, E12`. `L6` is `ui::PlaneSet` itself —
generic library storage for many retained planes (`...plan.md:139`). Turning a slice view's
`PlacedItem`/`PlacedFrame` records into `PlaneSet` calls (face-camera text, parent/child wheels,
per-plane state) is explicitly a different package, `U4`: *"Presenter over `PlaneSet` for placements
in three dimensions; panes, scenes and embedding…"* (`...plan.md:203`), depending on
`L3, L4, L6, L8`. **`U4` is scheduled at `M6`**, three milestones after `M3` claims the wheel is
visible.

`U2`, the only presenter package built by `M1` (`...plan.md:201`), is scoped explicitly to *"records
to `Canvas` per plane and `Beams`"* — the flat, single-shared-transform widget path (confirmed
generic by `02-rendering.md` claim 20: `WorldPanel`/`Canvas` share one transform per widget scene;
`PlaneSet` is new precisely because it needs an independent transform per plane). `Canvas` cannot
draw a wheel: a ring cell needs its own position/orientation/face-camera state in 3-D, which only
`PlaneSet` (and the presenter code that drives it, `U4`) can supply.

The plan's own framing paragraph (`...plan.md:110-115`) lists what M1/M2 "prove" — *"registry, view
space, bindings, prepare, layout, records, presenter, picking, accessibility"* — and separately
notes *"`PlaneSet` is needed first by the wheel (M3)"* without noticing that the **presenter that
drives `PlaneSet` from records** is a different, later-scheduled package. `E10` (all-dim walk) will
produce correct `layout()` records with nothing in M3's package set able to turn them into a drawn
frame through a real host.

**Fix.** Split `U4` into an early slice ("drive one pane's `PlaneSet` from one placement's records,
no `PaneTree`, no embedding") scheduled at `M3`, and the multi-pane/scene/embedding remainder kept
at `M6`; or explicitly move `M3` to depend on all of `U4` and push the wheel milestone back.

______________________________________________________________________

### 3. Command dispatch (

```text
U7
```

) is never scheduled at any milestone — M1/M2 are not drivable as described

**Evidence.** Track U's package table lists `U1` (dimension colour), `U6` (animation) and `U7`
(`view_commands`: *"every action registered, absorbing the slice commands… every chord dispatches;
nothing is handled in code"*, `...plan.md:206`). **None of U1, U6 or U7 appears anywhere in the
milestone table** (`...plan.md:99-107`, M0 through M8) — grep over the whole file confirms it; the
only U-rows the milestones name are `U2` (M1, M2), `U3` (M4), `U4` (M6) and `U5` (M7).

`M1`'s claimed deliverable is *"first light: stretch vanishing beside the legacy view, **in one
pane**"* (`...plan.md:100`) — a usable placement, not a static render. The view-system spec says the
existing nav chords (`std:nav/step_x_pos` and its five siblings, `view-system.md §7.3`) are what
drive a `SliceView::move()`; something has to register those actions against the *new* placement's
view space. That something is `U7`, and it is not built by M1 or M2's package set, nor anywhere the
milestone table names. Without it, the new stretch-vanishing placement that M1 claims to deliver
cannot be stepped through by keyboard or pointer — it can only be rendered once, from a
test-constructed cursor.

This gap is not in `§7`'s risk register either: no row addresses "nothing dispatches to the new
placement until `U7` lands," even though the milestone table's own omission makes it concrete.

**Fix.** Add `U7` (and, no later than `M2`/`M3`, `U1` for pack-view strand colours and `U6` for the
toss/transition tweens the spec requires) explicitly to the M1/M2 package sets, or state plainly
that M1/M2 are visual-only checkpoints with no working input path yet — which contradicts "in one
pane."

______________________________________________________________________

### 4. "Wrap

```text
ZigzagVisualizer
```

and `Views` as legacy placements" (U2, sized L) understates the actual coupling by roughly two
orders of magnitude

**Evidence.** `U2`'s description (`...plan.md:201`) treats this as part of one "L" (\<1,000-line)
package: *"Host with one pane and one scene; the legacy visualizer and `Views` wrapped as
placements; view switching."*

`ZigzagVisualizer` is declared

```cpp
class ZigzagVisualizer : public gleditor::FrameContributor, public gleditor::PickObserver, public gleditor::a11y::Source, public gleditor::ui::FocusScope, public xanadu::ZigzagPresentationSurface
```

(`apps/zigzag/zigzag_visualizer.hpp:113-117`); `xanadu::Views` is
`class Views : public gleditor::FrameContributor` (`apps/xudu/views.hpp:54`). Both are concrete,
directly-registered device objects with their own input handling, not objects behind any seam a
wrapper could slot in front of.

`apps/xuzz/xuzz_app.cpp` (2,915 lines) calls into them directly, not through any registry: 55
occurrences of `zigzagPresentation->…` and 116 occurrences of `views.…` (grep count), interleaved
with renderer setup, overlay wiring and command registration throughout the file (e.g.
`xuzz_app.cpp:85,500,865,2220,2259,2469` and on). None of this file's rework is named as its own
package anywhere in the plan; it is assumed to be inside `U2`'s "L" budget alongside everything else
`U2` does (presenter, host, picking, accessibility, measurer, the legacy-wrap, and view switching).

**Fix.** Give `xuzz_app.cpp`'s rewiring — extracting a `ViewHost`-compatible seam around
`ZigzagVisualizer`/`Views` and replacing the ~170 direct call sites with placement lookups — its own
package and size estimate (realistically XL, not folded into U2's L), landed before or alongside U2
rather than implied by one clause of it.

______________________________________________________________________

## MAJOR

### 5. The cut-over names no destination for most of what step 1 relocates

**Evidence.** §17 Step 1's relocation list (`view-system.md:2882-2889`) moves far more into
`apps/common/ui/xanadoc/` than `ZigzagVisualizer`, `Views` and `LinkBeams`: `session`,
`views_publication_links`, `bridge_coordinator`, `batch_orchestrator`, `satelloid`,
`tenuous_tether`, `kinetic_tether_overlay`, `wireframe_hull`, `world_card_presentation`, and the
overlays `clasp_link_forge`, `collaborator_overlay`, `link_panel_overlay`, `overview_overlay`,
`page_break_overlay`, `pouch_drawer`, `swarm_telescope_overlay`, `transcopyright_overlay`. Step 8
(`view-system.md:2942-2949`) only says what happens to `ZigzagVisualizer` (deleted),
`UnifiedTransclusionEngine` (ephemeral slots replaced) and `LinkBeams`/`Views` (reduced). Everything
else on that list is never mentioned again.

Concretely, `hypertime_graph.cpp` — named in `AGENTS.md`'s own description of `apps/xudu/` — is
never mentioned anywhere in `view-system.md` (grep over the whole document returns nothing). The
"VQL palette" that `AGENTS.md`'s `vortex_host.hpp` bullet says "calls
\[`promoteAndAttachToStore()`\] in production" is never discussed.
`apps/common/xanadu/reading_place.cpp` (session resume / reading place) is never mentioned.
`bridge_coordinator` is referenced exactly once, in §11.2, as doing "this... today for the one pair
it knows" — describing current behaviour, not a successor.

Each of these either keeps compiling as an orphaned `FrameContributor`/overlay registered outside
the `ViewHost` forever (which the plan never says is the intended end state), or needs its own
placement or fold-in that nothing budgets for. This is also missing from §7's risk register, whose
"Legacy and new paths diverge" row addresses only `ZigzagVisualizer`/`Views`.

**Fix.** For every file §17 Step 1 relocates, state its destination explicitly: wrapped placement,
folded into a named view, or "stays an independent overlay outside the view system, by design." Add
a row to §7 for whichever of these turn out to be "by design" exceptions, since each is a seam the
cut-over could silently break.

______________________________________________________________________

### 6. `Page::setModel()` stays public; nothing closes the second way to move a page

**Evidence.** F2's own evidence column already names this: *"`Page::setModel` is also already
public"* (`...plan.md:30`). `Drawable::setModel` (`include/gleditor/drawable.hpp:14`) is
unconditionally public: `void setModel(const glm::mat4 &m) { model = m; }`. L7's package description
(`...plan.md:140`) commits only to *"First split flow from pose (F2) with no behaviour change; then
`setPose`, `animatePoseTo`, `clearPose`…"* — it never says `setModel` becomes private, is routed
through `pose()`, or is refused once a non-flow pose is active. The risk-register row for this
(`...plan.md:366`, "high without F2") repeats the same non-commitment: *"L7 splits flow from pose
first, behind tests, with no behaviour change."*

Once `PagePose` exists, any caller — including `Doc`'s own reflow code at the three sites the
rendering review found (`src/doc.cpp:1241,1273,1698-1710`, which both *write* a page's matrix
unconditionally from the column formula and *read back* a previous page's live matrix to place the
next) — can still call `page->setModel()` directly and silently desync it from `pose()`.
`doc_page_pose_test.cpp`'s planned assertions ("a posed page survives reflow", "the column below it
is unaffected") would catch *some* instance of this if the implementer happens to gate all three
call sites correctly, but the plan names no mechanism that forces that outcome, only a test that can
fail and send someone back to guess at the fix mid-package.

**Fix.** State in L7 explicitly which of (a) `setModel` becomes private/protected with `Doc`'s flow
code the only caller, routed to write only the flow slot, or (b) `setPose`/`pose()` becomes the
single source of truth that `setModel` is redirected through — before the package starts, not
discovered by a failing test.

______________________________________________________________________

### 7

```text
CoalesceStrategy
```

's "pure, cold-start, fixed-25-steps" contract is unverified against the stateful engine it is
supposed to replace

**Evidence.** §10.3.2 specifies the built-in `CoalesceStrategy` as *"a pure function"* that *"never
reads a clock"* (`view-system.md:2260-2264`). But `TensionLayoutEngine` as used today
(`apps/xudu/beams.cpp`, the block ending in `far->animateMoveTo(...)`) is a **stateful, reusable**
object: `bodies_` carries velocity across calls, and `beams.cpp` keeps one `tensionEngine_` alive
across many rendered frames, so the *cumulative* step count and velocity state before a reader
perceives "settled" is far larger than one cold 25-step run from zero velocity (per `01-engine.md`
claim 6). Nothing in the plan — P2's test list is "pure; level within tolerance; parity cases from
S2" (`...plan.md:186`) — demonstrates that a fresh, zero-velocity engine run for exactly 25 steps
converges to visually equivalent positions as today's warm, continuously-integrated one for the same
scenario. Spike S2 (`...plan.md:75`) is the only check, and it is scheduled to gate P2, which is
correct — but the risk register's framing (`...plan.md:367`, "medium… S2; parity step in P3 with
captures") treats this as an ordinary risk rather than naming the specific, falsifiable mechanism (a
stateful integrator being asked to behave as a pure function) that could make S2 fail outright and
force a different `CoalesceStrategy` design, not just a parameter tweak.

**Fix.** Run S2 before P2's shape is finalized, and state explicitly in P2/P16 what happens if
parity fails — "a different solver, not a tuning pass" — rather than leaving S2's failure mode
implicit.

______________________________________________________________________

### 8. The full gate's cost and privileged dependencies are not carried into §6

**Evidence.** §6's gate table requires "the full gate" (`make -j$(nproc)`; `make test`;
`make format-check lint`; `compare-backends.sh` with captures inspected) for A1, U2, U4, X3, "and
anything touching a device" — which by §4.3/§4.4's own breakdown includes L3, L4, L5, L6, L7, L9,
plus the full gate running again "at every milestone" (`...plan.md:359`), i.e. roughly ten-plus
full-gate invocations over the project. `make test`'s last step is `tools/swarm-netns-test.sh` under
`unshare -Urnm`, which needs the `veth` kernel module (`AGENTS.md`'s Tests section) and whose
absence produces a non-zero exit **after** all four gtest binaries pass, fixed only by
`sudo modprobe veth` — "the user's job," per `AGENTS.md`, not something many CI runners or sandboxed
agents can do. §6 of the plan states the full gate flatly, with no carry-forward of `AGENTS.md`'s
own instruction to "read the four `[ PASSED ]` lines before concluding anything is broken." An
implementer or CI job without root will either (a) treat every one of these ten-plus full-gate runs
as failing, blocking all of tracks A/U/X and six of the L packages, or (b) learn to ignore the swarm
step's exit code generally, which defeats the one check `AGENTS.md` says is real
(`./tools/compare-backends.sh`, "the real check a backend still draws").

**Fix.** §6 should restate the veth caveat explicitly for every full-gate invocation it requires,
and say what a swarm-blocked (not swarm-failed) run counts as for gating a commit.

______________________________________________________________________

## MINOR

### 9. "One

```text
Canvas
```

per layer… exactly as the visualizer draws its world canvas now" overstates how simple the parity is

**Evidence.** `...plan.md:111-112` says the M1 presenter draws "one `Canvas` per layer and `Beams`…
exactly as the visualizer draws its world canvas now." `ZigzagVisualizer` actually keeps three:
`worldCanvas_`, `ancillaryCanvas_`, `hudCanvas_` (`apps/zigzag/zigzag_visualizer.hpp:683-685`,
constructed at `zigzag_visualizer.cpp:118-123`). "One Canvas per layer" is a reasonable description
if "layer" means one of these three, but "exactly as" overstates the parity claim's precision —
worth softening so U2's implementer does not assume a single shared canvas suffices.

### 10. §16.1's "no library" claim for

```text
xuzz_test
```

conflicts with §5.3 rule 1's explicit library includes

**Evidence.** `view-system.md:2822-2826`'s test table says the engine binary (`xuzz_test`) "links…
no library, no device." But §5.3 rule 1 (`view-system.md:435-438`) explicitly allows
`apps/common/xanadu/view/` to include `<gleditor/spatial.hpp>` and `<gleditor/draw_budget.hpp>` —
both genuinely header-only and device-free (confirmed: neither includes `device.hpp`, `renderer.hpp`
or any GL/Vulkan header), so this does not break the link-boundary claim, only the wording. Harmless
in practice (no new `.o` needs linking for a header-only include), but worth a one-line fix so "no
library" is read as "no library *link*," not "no library header."

______________________________________________________________________

## The three changes I would insist on before work starts

1. **Fix `design/view-system.md:2880-2889`** so the spec's own text matches what the implementation
   plan (correctly) does in A1 — stop asserting a correction that was never made to the document an
   implementer is told to read first.
1. **Re-sequence the milestone table.** M3 needs a `PlaneSet`-driving presenter (split `U4`, or move
   it earlier) to actually show the wheel, and M1/M2 need command dispatch (`U7`) to actually be
   drivable — as written, two of the plan's first three "something new can be seen and used" claims
   are not reachable with the packages the table names.
1. **Size the legacy-wrap honestly.** Give `xuzz_app.cpp`'s ~170-call-site rewiring around
   `ZigzagVisualizer`/`Views` its own package and estimate, and name a destination (placement,
   fold-in, or explicit exception) for every file `apps/xudu/`/`apps/zigzag/` hand off in the
   relocation — right now a dozen of them (including `hypertime_graph.cpp` and the VQL palette)
   simply vanish from the plan's text after step 1.
