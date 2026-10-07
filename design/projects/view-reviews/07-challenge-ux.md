# UX Challenge: View System Implementation Plan

Reviewed: `design/view-system-implementation-plan.md` (the plan) against `design/view-system.md`
(the spec), the full aesthetics report at `/tmp/claude-1000/.../scratchpad/plan/04-aesthetics.md`,
`design/ux_workflow_real_work.md`, `design/ui_workflow_xuzz_navigation.md`, the `xuzz-ux-validation`
skill, and `include/gleditor/ui/theme.hpp`. Read-only; no file in the repository was modified.

Findings are numbered F1–F19, most consequential first within each section, tagged **MUST FIX /
SHOULD FIX / CONSIDER**. Each gives the evidence (section or line) and the exact change proposed.

______________________________________________________________________

## 1. Learnability

**The plan's own milestone table (§4.1) is incomplete, and that incompleteness is itself the largest
learnability risk.**

**F1 — MUST FIX.** Four packages never appear in the M0–M8 table at all: `U1` (dimension colour),
`U6` (animation by `SubjectId`), `U7` (`view_commands`: "every action registered... nothing is
handled in code"), and `E16` (the `u`/`t` roles). The table's stated purpose is "the milestones at
which something new can be seen and *used*" (§4.1). Without `U7` tied to a milestone, the table
cannot show when any chord for any view becomes invocable; without `U1`, it cannot show when
dimensions first get colour at all. *Change:* add an explicit row or footnote in §4.1 assigning `U1`
to M1 (gated on `V1`, ahead of `E9`), `U7` split into a host half (M1: `view.palette`, `pane.*`) and
a per-view half (landing with each view's own chords, see F2), `U6` to M1 (a toss without animation
is a hard cut, which is itself a behaviour decision that should be visible in the table), and `E16`
either to a real milestone or an explicit "pending owner decision #3" marker in the table itself,
not only in prose in §7/§8.

**F2 — MUST FIX.** `E15` ("`builtin_views`, settings and **chord specs** for the slice views")
depends on "`E9` to `E11`" (§4.4) — i.e. on stretch vanishing *and* the pack view *and* the wheel
all being done. M1 ships `E9` alone; M2 ships `E11` alone. By the plan's own dependency chain, **no
default keyboard chord exists for stretch vanishing's or the pack view's own actions (axis
bind/cycle, `group.new`, `pack.enter/leave/lane/retrieve/keep`) until `E10` — the wheel, M3 — also
lands**, and `E15` is not itself required by M3 either, so it could land later still. A reader at M1
or M2 has a view to look at but, per the plan's own numbers, no promised way to act on it beyond
whatever movement chords the legacy keymap already defines. *Change:* split `E15` into per-view
chord packages gated on their own view only (`E15a` with `E9`, `E15b` with `E11`, `E15c` with
`E10`), so a view's chords land in the same milestone as the view.

**First-run gaps, by milestone:**

- **M1 (stretch vanishing beside the legacy view).** No compass, no selector (both M7); per F2,
  possibly no view-specific chords either. The only stated way to rebind an axis before M3's drag is
  "the existing cycle and swap actions" (§7.6) — and whether even *those* are registered this early
  is unclear given F1/F2. A reader can see a dense, content-packed view and has no discoverable way
  to change what it shows. Ghosts are on by default and the edge-heat sub-view exists (§9.1.5), but
  the palette that would list sub-views and name which one is active is M7 (`U5`) — this is exactly
  "sub-views before the palette lists them." The breadcrumb (§9.1.6) reads from the activity log,
  whose persistence design is explicitly deferred (risk register, §7); at M1 it is plausibly present
  but empty.
- **M2 (packs).** Building a pack requires a group bound to an axis. Group creation
  (`group.new`/`group.edit`) and pack navigation (`pack.enter`/`.leave`/`.lane`/`.retrieve`) are
  exactly the chords F2 shows may not exist yet. Without them and without the compass/selector (M7),
  there is no stated path by which a reader at M2 creates a group and binds it — the milestone's
  headline feature ("packs: the lane table with strands and the spread") may be unreachable through
  the interface it is supposed to demonstrate.
- **M3 (the wheel).** This is the first milestone with a genuinely discoverable bind-by-pointing
  path (drag an edge to an axis, §9.2.7) because the wheel draws its own x/y/z spokes in-world — it
  needs no separate compass widget to have a drop target. Good; but this means stretch vanishing and
  the pack view, which have no analogous in-world axis indicator, remain bind-blind through M1–M2
  even after M3 ships, since the compass that would help them is still two milestones away.
- **M6 (panes and scenes).** Multiple panes, each with its own cursor and bindings (V-R39), ship
  *before* the compass (M7) that is the natural way to tell, per pane, what is bound there. This is
  backwards: splitting panes is exactly when "which axis shows what, in which pane" confusion peaks,
  and the plan defers the one piece of chrome built to answer that question until after multi-pane
  support exists.
- **M7 (compass, selector, ranking).** The compass is specified as present on "every slice pane"
  from the start (§7.9) — it is basic chrome in the spec's own model, not an advanced feature — yet
  the plan's own decision #4 defers it six milestones. The plan is aware of the tension (it states
  the drag-only workaround) but does not mitigate it for the two views that have no drag-bind at all
  before M3.

**F8 — SHOULD FIX.** Pull a minimal, **display-only** compass (no drop-target, no drag support —
just a rose naming what is bound to each configured point) forward into M1's `U2` package. It needs
none of `U5`'s editing machinery to be useful as an orientation device, and it removes the single
root cause behind most of the gaps above.

______________________________________________________________________

## 2. Coherence across §5.1 / §5.4 / the spec

**F9 — SHOULD FIX.** Principle 4 ("The anchor stays still... the camera does") is written as one
rule but is actually doing two different jobs that the later sections apply inconsistently:

- **4a.** An object the reader is oriented on keeps its own position; when more room is needed the
  *camera* reframes. This is the base view's anchor page and stretch vanishing's accursed cell.
- **4b.** A view defines a fixed on-screen *slot* (the deck's current-page position `b`, the wheel's
  hub at local origin) that different objects occupy as the reader moves; filling it is *content*
  motion (something flies to the slot), and the camera need not move at all. This is the deck's
  split ("page `b`... flies forward... to the top of its stack") and, implicitly, the wheel after a
  walk (`Return`) re-derives around a new hub pinned at local origin.

A future fourth view built from Principle 4 alone could apply either reading to the wrong case —
e.g. move the camera to the wheel's new hub (breaking "fixed in the placement, not in the camera,"
§9.2.3) instead of re-deriving at a fixed local origin. *Change:* split the principle into 4a and 4b
as above, each with its own one-line worked example, in §5.1.

**F11 — SHOULD FIX.** The spec (line ~1577) says a view other than the pack view, meeting a group on
a spatial axis, "uses the group's first member dimension" — silently. Stretch vanishing's radius-1
tick, which "names the dimension and direction that reached it" (§9.1.6), will therefore print the
first member's name only. A reader who bound a three-member group `contact` to `y` sees a tick that
reads `d.email`, which looks like "this axis is `d.email`," not "this axis is a group containing
`d.email`." This is a spec-level asymmetry the plan inherits without comment. *Change:* state in
§5.2 or §5.4 that any per-cell label (tick, edge name) names the **group**, with its member swatch,
whenever the bound target is a group — not just the compass, which already does this correctly.

**F10 (partial) — colour-and-state coherence.** Principle 5 ("Colour is identity, never state") is
stated correctly in the abstract, but §5.4's component notes never spell out its sharpest corollary:
*never retint on hover/focus*. The one place this matters concretely is that the existing
`ZigzagVisualizer::dimensionVisual()` focus override already retints quote cells toward the theme's
accent cyan on focus (`zigzag_visualizer.cpp:1283-1284`) — exactly the anti-pattern Principle 5
forbids once dimension colour is load-bearing everywhere. The plan does not flag this existing code
as a precedent *not* to copy. *Change:* add one sentence to §5.2 or the "Compass" bullet of §5.4:
"Never retint a dimension's hue for hover, focus, armed or drag-over state; the existing quote-cell
focus override is legacy and must not be the model for the new views."

No contradiction was found in "real is hard-edged / view-only is soft-edged" against pages, packs or
badges as such — Principle 1 cleanly covers all of them. The one real tension is a *dropped* piece
of guidance, not a stated contradiction: the aesthetics report recommends a soft top/bottom
edge-fade specifically on a **windowed real page** (§3.6) so it reads as "a cropped strip," which is
a soft-edge treatment on a real thing. The plan's §5.4 "Base view" bullet does not mention
windowed-page edges at all. *Change (SHOULD FIX, folded into point 8 below):* state explicitly that
a windowed page's crop edge gets a one-sided opacity gradient — a different visual grammar from a
view-only object's rounded corner — so Principle 1 is not read as forbidding it.

______________________________________________________________________

## 3. The colour system

**F17 — MUST FIX.** §5.2 asserts "A `PlacedEdge` carries the dash class," stated as settled fact.
But the normative record type in the spec (§8.4) —
`from, to, a, b, kind, relation, opacity, label, bundle, gather` — has no such field. This is an
internal inconsistency between what the plan claims and what the spec it is built on actually
defines; whoever implements `view_records.hpp` from the spec alone will not find a place to put the
dash class. *Change:* add a new gap entry to §2.2 (after G11) requiring a `dashClass` field (or a
documented reuse of `relation`'s low bits, since `relation` is already a `DimRef` whose ordinal is
the colour-generation index `n`) on `PlacedEdge`, closed in `E1`/`E9` before `U1` is read by any
presenter.

**F18 — SHOULD FIX.** No minimum run-length is specified for when a dash pattern is still
distinguishable from solid — unlike text, which has `minReadableLinePx`. A pack's bundle strands
gather to a point partway between packs (§9.3.7); a strand segment there can be shorter than one
dash period well before any text on screen would be illegible. The second cue can therefore fail
silently exactly where density is highest (point 5 below). *Change:* add `view.dash.minRunPx` (or
similar); below it, only the per-edge glyph (which works at a point, unlike a repeating pattern)
carries the second cue.

**F19 — CONSIDER.** The generation rule (golden angle onto the four open arcs, concatenated by
length) places the first dimension (`n=0`, `frac(0)=0`) at the very first open arc's lower boundary
— directly abutting a reserved band. A few degrees of inset margin on each arc before mapping would
remove this edge risk cheaply.

**Capacity and the common 2–3-dimension case.** The golden-angle construction is specifically chosen
because it spreads well even at low prefix lengths (that is its whole advantage over a linear
`360°/N` slice), so two or three dimensions will *not* look arbitrarily close together by
construction — but they also will not look like anything a reader would predict (no "x is always
cyan"), and the plan's §5.2 does not say this is intentional or warn an implementer not to special-
case the first few dimensions back toward a familiar palette. Worth one clarifying sentence, not a
numbered finding on its own.

**Groups on the compass vs. groups in the content.** The compass is right to show a group as an
achromatic arm with a member swatch (D4/§2.5) — that *is* "recognisable" without giving the group
its own hue. The gap is downstream, in the content area itself (F11 above): the compass solves
recognisability at the rose, but the cell content and ticks do not currently say "this is a group,"
only "this is its first member."

______________________________________________________________________

## 4. Motion

**F4 — MUST FIX. The plan's §5.3 table and the spec's §12.3 settings disagree by omission, not by
number, and the disagreement is specific to `toss`.** Checking every family the prompt names:

| Family               | Plan §5.3                               | Spec §12.3 setting                                    | Agreement                    |
| -------------------- | --------------------------------------- | ----------------------------------------------------- | ---------------------------- |
| riffle, per page     | 60–120 ms, run < 420 ms                 | `stack.minFlipMs/flipMs/maxTransitionMs` = 60/120/420 | **Matches exactly**          |
| split                | block 320 ms; target 80 ms later        | `stack.splitMs/splitLeadMs` = 320/80                  | **Matches exactly**          |
| sworph (page fly-in) | subject 620 ms; row 450 ms, 90 ms later | `page.base.subjectMs/rowMs/rowDelayMs` = 620/450/90   | **Matches exactly**          |
| **toss** (old/new)   | 90 ms fade; 160 ms fade+scale           | **no entry anywhere in §12.3**                        | **No spec grounding at all** |

Three of the four numbers the prompt asks about are faithfully carried over; the fourth — the one
transition that fires on *every single rebind*, more often than any other in the table — has no
settings home in the spec and is a pure plan invention. The same is true, more broadly, for view
switch (220 ms), sub-view switch (150 ms), pack spread (220 ms — note `pack.spreadGap` in §12.3 is a
*distance* in px, not a duration; it is not the same number under a different name), wheel depth
change (240 ms), selector open/close/tier (180/120/140 ms), pane split/close (150 ms), and subspace
zoom (450 ms): **none of these appear as a `SettingSpec` in §12.3.** This violates V-R41 ("Every
tunable MUST be a `SettingSpec`... none MAY be a literal in code") for roughly ten numbers the plan
presents as already decided. (Camera reframe, 700 ms + 140 ms, is the one exception with a
legitimate excuse: it reuses `anim::cameraSettle`/`cameraSettleDelay`, a pre-existing *library*
animation constant that predates the view system and is arguably outside the governance rule's
scope, which targets `apps/xudu`/`apps/zigzag`.) *Change:* add a gap entry (G14) listing each
missing setting and the package responsible for adding it as a real `SettingSpec` (`U6` for
toss/view-switch, `E10`/`E11` for wheel-depth/pack-spread, `E13` for selector timings, `U4` for pane
split/close, `E16`/`U5` for subspace zoom) before `U6` reads any of them.

**F12 — MUST FIX.** Nothing in §5.3 says what happens to the 120 ms step tween under key repeat.
Stepping along a dimension is the single most frequent action in the system. If the OS fires repeat
events faster than 120 ms (typical after the initial delay), each new `MoveRequest` either
interrupts the in-flight ease-out (which assumes starting from rest — restarting it every ~40–80 ms
produces a visible stutter, since the curve never reaches its "ease" phase) or queues, so lifting
the key after a one-second hold leaves the view still catching up. Holding a movement key is one of
the most common things a reader will do, and the plan is silent on it. *Change:* state a key-repeat
policy explicitly — e.g. while a movement key auto-repeats, coalesce into a constant-velocity glide
and only apply the 120 ms ease-out on the final step (key-up) — and add a §5.5 check: "holding a
movement key produces continuous motion, not N discrete 120 ms hops."

**F13 — CONSIDER.** 450 ms for the subspace (`u`) zoom is justified by analogy to opening a whole
document (`anim::docArrival`), which implies `u` is rare and deliberate. But the spec's own
description — a persistent rim band inviting repeated dive-in/back-out exploration (§7.3) — reads as
something used as routinely as any other axis. *Change:* state explicitly whether `u` is expected to
be rare (keep 450 ms) or frequent (move it toward the wheel-depth-change register, 240 ms); three
nested dives and returns at 450 ms each is 2.7 s of pure camera animation before the reader can act
again.

**What §5.3 is missing.** No entry for camera orbit (direct pointer-coupled, or damped? unstated),
for the moment a drag enters a `DropTarget`'s radius (the aesthetics report's "drag-over = outline
doubles in width" did not survive into the plan at all, see point 8), for a hover delay before a
pack's spread opens (without one, scanning a dense rank of packs with the pointer will open and
close spreads continuously), or for the drop-accept moment itself — contrast the selector, where the
aesthetics report specifically asked for the armed item to visibly travel toward the pressed point's
direction before closing (also dropped, see point 8) so the reader sees *where* the bind landed;
dragging to an axis gets no equivalent confirmation anywhere on screen except the unrelated toss
fade elsewhere in the pane.

______________________________________________________________________

## 5. Density and legibility at 1280×800

**F3 — MUST FIX.** The wheel's clearance algorithm (`flex`, §9.2.8) only tests a candidate slot
against "a real cell already placed" — not against other already-placed labels or badges. But §5.5
check #1 ("No label overlaps another label") is stated as an automatic check with no mechanism
described that would actually guarantee it at high valence across multiple leaning rings, where two
labels from different rings can be angularly close in 2-D screen projection without either slot's
*cell box* colliding with a real cell. At valence 60 (the spike's own worst-case test), with several
rings needed to hold that many pair slots, this is a plausible realistic failure, not a corner case.
*Change:* either extend `flex` to clear against every already-placed `PlacedItem`/label (not just
real cells), or explicitly document that label-label overlap is possible and must trigger a
detail-ladder drop, with an acceptance test added at valence 60 specifically checking label-vs-label
clearance (V2's current pass criterion, "a reviewer agrees it is a wheel," would not catch this on a
quick gestalt look).

**Stretch vanishing worst case.** A high-valence slice with short content maximises both ghost count
and edge-heat saturation simultaneously: dozens of ghosts, heat glow on every edge, ticks on every
radius-1 neighbour, and a breadcrumb strip, all active at once. None of the §5.5 checks test whether
the accursed cell and its radius-1 ring remain visually distinct from this chrome at maximum density
— the closest check (#1, no label overlap) is necessary but not sufficient for "readable as a
layout, not noise." *Proposed check:* a captured frame at `stretch.heatFull` saturation in every
sector, with minimal cell content throughout, where a reviewer can still name the accursed cell and
its four radius-1 neighbours without counting pixels [H].

**Nested packs worst case.** A group of 8–10 members, each producing multi-line content, already
produces a single pack roughly 400–540 px tall at `pack.laneMaxLines`=3; `pack.nestDepth`=3
compounds this with three levels of outline-plus-header chrome nested inside one lane. No §5.5 check
targets header/content legibility at full nesting depth with a maximal member count. *Proposed
check:* a captured frame of a 3-level-nested, 8-member-group pack, checked for whether every level's
header stays legible and no level's content drops below its minimum line height [H].

**Decks worst case.** The plan already adopted the aesthetics report's fix for a link-dense single
document (its D3, renumbering the aesthetics report's D4) and §5.5 check #10 covers it. The gap is
*several* decks sharing one pane (§10.4.6 explicitly describes this case): at 1280 px, three
documents each wanting both tops plus a receding line will force the stagger search into a worse
score for all three simultaneously, and nothing in §5.5 or the acceptance list (§10.4.10) tests
multi-deck legibility, only single-deck behaviour. *Proposed check:* a captured frame of three
side-by-side decks at 1280 px width [H].

______________________________________________________________________

## 6. Accessibility and keyboard

**F5 — MUST FIX.** No chord in §12.1's table reorders ring order (`ViewAxisSet::moveInRing`) or
performs the group editor's internal edits (`insertMember`/`removeMember`/`moveMember`). The spec's
own end-of-§12.1 pointer/keyboard pairing list names "reorder the ring (drag a spoke round, or **the
HUD**)" as if a non-drag path already exists, but neither the spec nor the plan ever defines what
"the HUD" is as a registered action with a chord. This violates V-R42 ("a keyboard form if it has a
pointer form") for two of the system's core structural edits. *Change:* add `ring.reorder(±1)`
(owned by `E12`) and `group.member.move/remove/insert` (owned by `E5`) rows to §12.1 with default
chords.

**F6 — MUST FIX.** The selector (§7.7) is the one surface in the spec with no dedicated
accessibility subsection — every slice view gets one (§9.1.7, §9.2.10, §9.3.10) but the selector
does not, and the plan's `E13` test list (§4.4) tests behaviour, not accessibility. This is exactly
backwards: the selector is explicitly "the one place a key press alone, with zero pointer motion,
completes a structural change" (aesthetics report), making it the highest-stakes surface for a
missing screen-reader contract. *Change:* add a gap entry (G13) requiring the selector's role per
tier, cross-tier reading order, and "armed"/"bound" announcements, closed in `E13`/`U5` before M7.

**F7 — SHOULD FIX.** Edge heat (stretch vanishing) conveys "how much lies this way" purely through
glow brightness; §9.1.7's accessibility paragraph covers cells, neighbours and the breadcrumb but
never mentions heat sectors. A screen-reader user gets nothing where a sighted user reads "more this
way" instantly. *Change:* add a `note`-role node per heat sector naming its approximate count, in
`E9`.

**Other pointer interactions checked against §12.1:**

- *Drag an edge to an axis* — keyboard path exists (`[`/`]` select spoke, `B` binds to "the next
  axis," or the full selector for an arbitrary target) but is materially slower for a *specific*
  far-away axis via `ring.bind`, and 10–20+ keys via the selector versus one drag gesture. Not a
  MUST FIX (both paths exist), but worth a §5.5-style note acknowledging the asymmetry.
- *Drag to the compass* — functionally equivalent to opening the selector and pressing a point's
  name; no real gap.
- *Hover a pack's join* — the `spread` sub-view key gives a (broader, not narrower) keyboard
  equivalent; no gap.
- *Scrub a deck* — spec states explicitly that next/previous page and link cover it; no gap.
- *Click a page in a line* — **F14, CONSIDER.** Reaching an arbitrary non-link page 47 rows down a
  deck by keyboard means 47 discrete `next page` presses versus one click. *Change:* add a "go to
  page N" action for decks.

______________________________________________________________________

## 7. Review gates

§5.6's gates are reasonably well-placed in time (`V1` before `E4` is as early and cheap as possible;
the shared-chrome review ahead of `E9`–`E11` correctly precedes the point where three views would
otherwise each invent their own ghost/seam/strand treatment) but have two structural weaknesses:

**F16 — SHOULD FIX.** No gate names an owning role — "the eye is consulted" is passive throughout
§5.6, and §6's gate table says only "the captures inspected," by whom unstated. For gates with an
`[H]` acceptance check this matters: a gestalt judgement call needs a named reviewer, not an
implicit assumption that whoever wrote the code also judged it. *Change:* name a role at each gate
(e.g. the implementing engineer self-checks against §5.5's `[A]` checks; a second person signs off
on every `[H]` check before the milestone's commit).

Several motion-heavy milestones are checked only with still frames where the thing being validated
is inherently temporal: P3's parity gate explicitly says "numbers matching is not the same claim as
looking the same" but still only prescribes a side-by-side *static* capture, not a recording of the
coalesce animation; the same is true of M3's wheel orbit, M5's riffle/split choreography, and M6's
pane split/close. §5.5 checks #8, #9, #12 and #14 are explicitly about motion and cannot be verified
from a still. *Change:* require a short screen recording, not just stills, at every gate whose
subject is an animated transition.

`V2`'s stated pass criterion ("a reviewer agrees it is a wheel, not a cone or a flower") is a
gestalt check that would plausibly miss the label-overlap defect in F3 — a quick "does this look
like a wheel" glance does not naturally look for two small labels overlapping behind other content.
*Change:* expand `V2`'s checklist to explicitly include label-vs-label clearance, not just the
overall shape.

**Usability tests proposed for M1, M3, M7:**

- **M1.** Task: "Given a slice with ~5 dimensions and a few hundred cells, find a cell two steps
  away on a dimension not currently bound to any axis, and tell us how you would get it onto `y`."
  Given F1/F2/F8, I expect this test to surface immediately that there is no discoverable way to do
  this at M1 — exactly the finding that should land before M2–M7 compound it.
- **M3.** Task: "Given a cell with ~20–30 connections, bind a named dimension to `y`, using whatever
  means you find" — run with half the group pointer-only and half keyboard-only, to surface the
  `ring.bind`-is-"next axis"-only limitation and the keyboard-reorder gap (F5).
- **M7.** Task: "Open the selector, find a dimension you have not used recently (buried in tier 3),
  bind it to a new axis, then — a session later — find it again via the pouch or the ranking." This
  tests tier-3 navigation, the ranking's real usefulness, and binding persistence together.

______________________________________________________________________

## 8. What the condensed plan lost

Five pieces of concrete, cheap, load-bearing guidance from the full aesthetics report that §5
omitted or weakened, and where each belongs:

1. **`PlacedEdge` has no field for the dash class** the second-cue rule depends on (F17 above). →
   `design/view-system-implementation-plan.md` §2.2, as a new gap entry.
1. **Badge digits should use the `Mono` role's fixed-advance metrics** so a valence count going from
   9 to 10 does not reflow its neighbours — a direct, cheap instance of "what stays still" that the
   plan's §5.4 "Packs"/"The wheel" bullets never mention even though both bullets already discuss
   valence badges. → fold into those two bullets.
1. **The selector's bind-commit motion cue** — the armed item visibly travels toward the pressed
   point's direction before the selector closes, so the reader sees *where* the bind landed — is
   entirely absent from §5.3's motion table, which lists selector open/close/tier but not this
   moment. → a new row in §5.3, or a line in §5.4's "Selector" bullet.
1. **The concrete reserved-arc degrees and the vivid/deep/pale HSV numbers** are paraphrased away
   into "a first pass... later dimensions take a second and third band." The aesthetics report that
   has the actual numbers lives only in an ephemeral scratch directory outside the repository; once
   this session ends, those concrete values exist nowhere durable unless committed now. → commit the
   open-arc ranges and the S/V numbers verbatim into §5.2 or a new settings-adjacent table.
1. **The explicit warning against the existing quote-cell hover-retint precedent** (F10 above) — the
   plan states the rule ("colour is identity, never state") but not the concrete anti-pattern
   already in the codebase that violates it. → one sentence in §5.2 or the "Compass" bullet of §5.4.

A sixth, smaller loss worth a mention: the aesthetics report's "drag-over = arm outline doubles in
width, never re-hued" and its explicit warning against nesting a group's swatch strip inside the
arm's own stub (so it does not read as "a multicoloured dimension") are both absent from the plan's
thinner "Compass" bullet.

______________________________________________________________________

## Summary

**Three things most likely to make the result feel incoherent:**

1. The binding-affordance gap, M1 through M6: a reader has views to look at and, on the plan's own
   numbers, no reliably discoverable way to change what is bound where until M7. This is the root
   cause of most of §1's findings and is only partially acknowledged (decision #4) without
   mitigation.
1. The ~10 motion-timing numbers in §5.3 with no settings home anywhere in the spec (F4):
   implemented independently by `U2`, `U6`, `E10`, `E11`, `E13` and `U5` with no single reconciling
   pass, these are exactly the kind of unlinked literals that make three views feel like three
   different pieces of software — the risk the register already calls "high" for a different reason
   (shared chrome) but that applies equally to motion.
1. The keyboard gaps for ring-reorder and group-editing (F5), landing on top of the already-slower
   selector path for an arbitrary bind (point 6): a "keyboard-first" application that is
   qualitatively worse on keyboard than on pointer, for the exact operations (restructuring,
   grouping) a reader needs to master the system, undercuts the project's own framing.

**The single milestone-order change I would make for the reader's sake:** pull a minimal,
display-only compass into M1 (F8) rather than deferring all discoverable binding chrome to M7. It
costs little — no drop targets, no drag, no editing — and it is the one change that removes the root
cause behind the learnability gaps at M1, M2 and M6 simultaneously, rather than patching each
milestone's symptom separately.
