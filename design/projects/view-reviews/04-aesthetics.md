# Aesthetic and experiential guidance for the Xuzz view system

Scope: `design/view-system.md` (cited as VS:*line*) against the visual language already built —
`include/gleditor/ui/theme.hpp`, `src/ui/metrics.cpp`, `include/gleditor/animation.hpp`,
`include/gleditor/ui/widgets.hpp`, `apps/zigzag/zigzag_visualizer.cpp`, `apps/xudu/beams.cpp`,
`apps/common/xanadu/link_layout.cpp`, and the two UI-design skills. Every recommendation below names
the exact component, a number or rule, and the reason, so an implementer who is not a designer can
apply it without guessing, and a reviewer can check it against a captured frame.

______________________________________________________________________

## 1. Design principles

**P1 — Real is a sharp rectangle; view-only is soft and seamed, never the other way round.** Today's
only drawn "real" thing, a ZigZag cell, is an unrounded box: `addRect` fill plus four `addLine`
border strokes at `scene_.border_thickness` (`apps/zigzag/zigzag_visualizer.cpp:1996-2037`,
`zigzag_visualizer.hpp:109`, default `2.0`). Keep that: every real cell and page stays
hard-cornered. Every view-only thing the new system invents — packs, ghosts, the selector's tiles,
badges — gets a soft corner radius (recommend `0.5 × theme.paddingEm` in logical px, i.e. the same
unit the padding already uses) and a thinner, lighter stroke. Consequence that settles an argument:
if an implementer proposes drawing a pack's outline as a plain hard-cornered box "because it's
simpler," the answer is no — V-R45 (VS:281) already requires a non-`cell` role, and a rounded corner
is the free, non-verbal form of that rule. A cell that is 1×1 on its group still gets pack chrome
(V9, VS:3004), precisely so a shape rule, not a count, is what tells real from view-only.

**P2 — Depth means something, or it is zero.** Every z-displacement must trace to a bound dimension
step (`stretch.layerDepth`, VS:1636-1641), a pack's own existence in the derived arena, a page's
place in its deck, or the rest camera's fixed tilt (`view.camera.restYaw/restPitch`, VS:2652,
2793-2794). Consequence: an implementer who wants to stagger cards in z purely to reduce visual
clutter (a common card-UI trick) is wrong for this system — two things that do not differ on a real
axis must not differ in depth, or the camera stops being a measuring instrument and becomes
decoration. The one sanctioned exception is the page view's `page.backgroundDepth` (VS:2683), and
that one is already a real fact (foreground vs. background document), not a cosmetic offset.

**P3 — The thing the reader should track takes the longest and leads.** `anim::sworphSubject` (0.62
s) outlasts `anim::sworphRow` (0.45 s) deliberately: "giving every one of them the same duration
reads as the whole view jumping rather than as one document being brought over"
(`include/gleditor/animation.hpp:40-45`). The view system reuses these verbatim as
`page.base.subjectMs`/`rowMs`/`rowDelayMs` = 620/450/90 (VS:2690). Generalize the rule, don't just
copy the numbers: in a riffle-or-split transition, the flown target page is the subject and must
out-duration the cycling block (§10.4.7); in a toss, the *arriving* derived cells are never the
slowest thing on screen, because nothing arriving should out-rank the real cell the reader already
has. Consequence: an argument over which of two simultaneously-moving things gets the longer
duration is settled by asking which one the reader is *watching happen to*, not which one is
"bigger."

**P4 — What stays still is the reader's anchor, and the anchor is never the thing that just
happened.** The base view's anchor page does not move (VS:2206-2208, "its page does not move"); the
accursed cell is "never faded and never hidden" in stretch vanishing (VS:1673). When more space is
needed, the *camera* reframes — "the pane's camera frames it; the layout never shrinks a page"
(VS:2233-2234) — the anchor's own coordinate does not change. Consequence: an implementer who panics
about a crowded pane and proposes re-centering the hub/anchor inside the pane (panning the world
instead of moving the camera) breaks this rule; the fix is always a camera move, never a world move
of the thing the reader is oriented on.

**P5 — Colour is identity, never state.** A dimension's colour is the one fact that must survive
every rebind, every camera angle and every theme change (V31, VS:3127-3131: "a dimension has one
colour everywhere"). Transient state — hover, armed, focus, drag-over — must therefore never be
shown by *changing* a hue, only by modulating brightness, outline weight or a glow band, because
changing hue on a dimension's own colour would make a user ask "did this become a different
dimension?" Consequence: a hover highlight on a wheel spoke brightens and thickens that spoke's
existing colour; it does not tint it toward the theme's accent cyan (`theme.colours.accent`,
`include/gleditor/ui/theme.hpp:39`, `{0.25,0.70,0.95}`) the way the current ZigZag focus override
does for quote cells (`{0.22,0.74,0.97}` at `zigzag_visualizer.cpp:1283-1284`) — that override is
fine for a one-off "this is special" cue, but it must never be the general hover/focus treatment
once dimension colour is load-bearing everywhere.

**P6 — A view-only placeholder never outshines the dimmest real thing on screen.** A ghost stands
for content that is *not* being shown; it must always read as less present than any real cell or
page still visible, however faint that real content has faded. This is the principle the task's own
example acceptance check ("a ghost is never brighter than the dimmest real cell") is testing for,
and §9 below shows the current defaults (`stretch.ghostOpacity` 0.25 vs. `stretch.fadeFloor` 0.15,
VS:2661/2663) violate it as written — flagged as Disagreement D1.

**P7 — Legibility degrades in discrete, named steps, with hysteresis, never by continuous shrinking
past the point of reading.** The wheel's detail ladder (VS:1884-1891) and a page's `Coarse` mode
(`DrawBudget::coarseBelow`, VS:2163) are *switches*, not interpolations; §10.4.3's stagger search
already keeps a hysteresis band (`stack.search.hysteresis`, VS:2697) so a 1% resize does not flip
the chosen direction. Apply the same discipline everywhere a size crosses a legibility threshold:
condense → hide → badge → summary → count, snapping at a measured threshold, with a small dead zone
around it so a camera dolly of a few pixels does not thrash between two detail levels every frame.

______________________________________________________________________

## 2. The dimension colour system

### 2.1 What already exists, and why it cannot scale

`ZigzagVisualizer::dimensionVisual()` is a ~24-entry chain of hand-picked `glm::vec3` literals keyed
by dimension name, falling back to a flat grey `{0.7, 0.7, 0.75}` for anything else
(`apps/zigzag/zigzag_visualizer.cpp:852-1060`). That is not a generation rule, it is a lookup table
someone will have to keep extending by hand, and every dimension past the 24th is visually identical
grey. `zigzag-ui-design/SKILL.md:72-74` records the *older* convention this table partially
preserves: X cyan, Y emerald, Z amber — and amber is exactly the hue family already spent on
Identity Gold transclusion prisms (`0xFFD700FF`, `apps/xudu/beams.cpp:1734`) and on the ZigZag focus
highlight (`SceneVisual::focus_color` default `{0.956, 0.773, 0.259}`, `zigzag_visualizer.hpp:104`).
The new system's `ui.dimension.<name>.colour, assigned from a palette when first seen`
(VS:2706-2708) must replace this table with an actual rule, or the collision above simply gets
re-created on day one.

### 2.2 Reserved hues (must not collide)

Collected from what is already load-bearing, in approximate hue degrees (sRGB hue of the literal
cited):

| Band (°) | Already means                                                                     | Source                                                                                                                                        |
| -------- | --------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| 350–20   | Disagreement link type (red)                                                      | `link_layout.cpp:487-489` (`0xFF7A6B`)                                                                                                        |
| 28–58    | Identity Gold transclusion, transcopyright amber, ZigZag focus, Illustration link | `beams.cpp:1734`/`1754`, `zigzag_visualizer.hpp:104`, `link_layout.cpp:484-486`                                                               |
| 140–162  | Quotation link type (green)                                                       | `link_layout.cpp:493-495` (`0x7FE0A8`)                                                                                                        |
| 190–222  | Theme accent, satelloid/tether cyan, Comment link, quote-cell cyan override       | `theme.hpp:39`, `beams.cpp:976`/`1254`, `link_layout.cpp:481-483`, `zigzag_visualizer.cpp:1283-1284`                                          |
| 258–322  | Authorship link (purple), Dimension link (violet), xanalink magenta ribbons       | `link_layout.cpp:490-492`/`509-511`, AGENTS.md:363 ("cyan and magenta... ribbons"), `xudu-ui-design/SKILL.md:77` ("Citation: purple/magenta") |

That leaves four open arcs totalling roughly 197° of the 360° wheel: **\[58°,140°)**,
**\[162°,190°)**, **\[222°,258°)**, **\[322°,350°)**. Dimension colour lives only inside these.

### 2.3 Generation rule

Reuse the codebase's own idiom — `linkColourWithInstanceShift` already derives a deterministic
micro-hue offset from a golden-ratio hash of the link id
(`apps/common/xanadu/link_layout.cpp:562-567`, "Deterministic micro-hue offset of +/- 7% based on
golden ratio hash") — but apply the golden-angle step to the *sequence of first-seen dimensions*,
not to one link's jitter, and restrict the sequence's range to the open arcs above so a collision
with a reserved band is structurally impossible rather than something to be nudged away from after
the fact:

```
domain(u)            : u in [0,1) mapped onto the four open arcs, concatenated by arc length
hue(n)   = domain( frac( n * 0.6180339887 ) )     // n = how many distinct dimensions were
                                                   // already assigned when this one was first seen
band(n)  = n mod 3                                // 0 vivid, 1 deep, 2 pale (below)
dash(n)  = n mod 5                                // solid, dash, dot, dash-dot, long-short
glyph(n) = n mod 5                                // circle, triangle, square, diamond, plus
```

`n` is monotonic and never reassigned, so adding a 50th dimension never moves the 1st dimension's
hue — the exact "no ceiling users work around" property a 360°/N linear slice would violate every
time N grows. The value is written once to `ui.dimension.<name>.colour` the first time a dimension
is drawn in a slice (VS:2706-2708 already specifies this storage; this section specifies what to
compute before that first write).

Lightness/chroma bands extend capacity once the 197°-wide arc set is revisited (roughly every 20-30
dimensions, depending on how finely hues must separate — see §2.4):

| Band  | Reads as                                             | Approx. HSV (same convention as `link_layout.cpp`) |
| ----- | ---------------------------------------------------- | -------------------------------------------------- |
| vivid | the dimension, at full presence                      | S 78%, V 92%                                       |
| deep  | the dimension, in a denser/older spot                | S 85%, V 70%                                       |
| pale  | the dimension, airy, used for large glued pack fills | S 40%, V 97%                                       |

A band change at the same hue reads as "the same dimension family, a different context" rather than
"a different dimension" — useful for e.g. a pack's `itemTinted` face wash (§9.3.7, VS:2083-2086)
versus its border, which already use a 25%-intensity-fill / full-intensity-border split today
(`bgCol = base_color * 0.25`, `borderCol = base_color` at full, `zigzag_visualizer.cpp:1982-1990`).
Keep that existing fill/border ratio; band is a second, independent lever for when hue alone runs
out.

### 2.4 Colour-blind safety and the second cue

Every one of the four open arcs survives a deuteranopia/protanopia simulation as *distinguishable
lightness steps even where hue collapses* — amber-to-cyan confusions are the ones excluded by the
reserved bands already. Still, do not rely on hue alone past a handful of dimensions: the second cue
is `dash(n)`/`glyph(n)` above, applied to every `PlacedEdge` of kind `Dimension` or `Strand` and to
the compass arm's tip. **This requires a field the current record type does not have**:
`view_records.hpp`'s `PlacedEdge` (VS:1235-1246) carries `relation`, `opacity`, `bundle` and
`gather` but no stroke pattern — add a `dashClass` (or reuse `relation`'s low bits, since `relation`
is a `DimRef` and a `DimRef`'s own ordinal is exactly `n`) so the presenter can draw the pattern
without a second lookup. Flagging this now because §5.2's package map (VS:384-411) does not mention
it and it is cheap to add before `view_records.hpp` ships.

### 2.5 Groups get no hue of their own

A group is a binding-arena cell (V13, VS:493-511), not a real dimension — giving its compass arm or
axis slot a new hue would blur P1's real/view-only line one level up (a group would start to look
like "a bigger dimension" instead of "a reader's arrangement of dimensions"). Draw a group's arm
**achromatic** (the theme's `border` colour, `{0.3, 0.33, 0.38}`, `theme.hpp:40`) with a small
swatch strip beside the name showing its members' own colours in order — a legend, not a new
identity. This also matches §7.9's existing text: "A group shows its name and how many members it
has, and lists them on hover" (VS:920-921) — the swatch strip is the always-visible form of that
list.

### 2.6 Reader override

`ui.dimension.<name>.colour` is already "the reader's to change" (VS:2708). Expose it from the
compass arm (activate → a small `Scrubber` triple for H/S/L, reusing `ui::Widget`'s existing model,
`include/gleditor/ui/widgets.hpp:76-82`) or from the selector. On a choice that lands inside a
reserved band or within the colour-blind confusion distance of another live dimension, do not refuse
it (colour taste is the reader's) — surface a §12.2-style message ("*name*'s colour is close to
*other*'s; still set.") and nothing more.

______________________________________________________________________

## 3. Component by component

### 3.1 Compass (§7.9, VS:905-926)

- **Hierarchy**: the five cardinal arms first (x/y/z always drawn even unbound, u/t as petals round
  the rim); each arm's *label text* is what the eye should land on, not the arm's colour swatch —
  text first, colour second, matching P5/P6's rule that colour is an identity cue, not the primary
  signal.
- **Proportions**: it is chrome (`ui::ScreenOverlay`, §11.7, VS:2559-2565), so size by
  `ui.minTouchPx` (44, `theme.hpp:30`) per arm hit-target, not by its drawn glyph size.
- **Stroke/state**: unbound arm = thin outline only, no fill, labelled "—"; bound arm = filled stub
  in the dimension's colour at full band-"vivid" strength; drag-over = the arm's outline doubles in
  width (never changes hue, P5); a group arm is the achromatic form from §2.5.
- **z-order**: always on top of the scene (screen space), but never over the active selector, which
  is modal (§11.7, VS:2567-2569 clarifies the selector itself is drawn in world space, not chrome).
- **One mistake**: drawing the group swatch strip *inside* the arm's own stub, so it reads as a
  multicoloured dimension instead of a legend for several — keep the strip detached, below or beside
  the arm, connected by a thin leader line only on hover.

### 3.2 Selector: tiers 1 to 3 and the armed state (§7.7)

- **Hierarchy**: the accursed cell stays dead centre and unchanged in size across all three tiers —
  that constancy is what keeps the reader oriented while everything round it swaps (VS:763-764, "the
  accursed cell, which stays in view at its centre"). Tier content is always secondary to it.
- **Tier 1**: groups ring at `ring.radius` (VS:2666 reuses the wheel's own radius setting — do not
  invent a second one); fan-out of a selected group's members sits in a *second*, larger-radius
  layer so the torus reads as "ring, then an outer petal of members," never as two competing rings
  at one radius.
- **Tier 2**: three concentric rings, **most used outermost**, **most likely** inside that,
  **pouch** innermost — outermost-to-innermost is a read-order the reader can learn once (biggest
  ring = broadest/most stable category) and reuse every time the selector opens.
- **Tier 3**: literally stretch vanishing's own placement rule over dimension cells (§9.1.3) — so
  nothing new is implemented, but the *scale* must shrink enough that "most relevant nearest the
  centre" (VS:776) does not fight the accursed cell's own radius-1 slot.
- **Armed state**: an activated item gets a steady (non-pulsing — reduced-motion users must not lose
  this cue) outline ring in the theme's accent colour, *not* its own dimension colour, because
  "armed" is UI state, not dimension identity (P5); the binding-point name it is waiting for
  (`x y z u t`) should ghost-overlay faintly on the item itself as a hint of what pressing a letter
  will do.
- **"Press a point's name" moment**: this is the one place a key press alone, with zero pointer
  motion, completes a structural change — the chosen item must visibly travel toward the pressed
  arm's direction (a short motion hint, not a cut) before the selector closes, so the reader sees
  *where* the bind landed, not just that the selector vanished.
- **One mistake**: letting tier 1's fan-out and tier 3's stretch layout share corner radius/stroke
  styling with *real* dimension cells so closely that a reader cannot tell selector chrome from the
  dimension cells it is built from — keep a thin view-only frame (P1) around every tier, even tier 3
  where the items are literally real cells.

### 3.3 Stretch vanishing (§9.1, VS:1579-1721)

- **Hierarchy**: the accursed cell (radius 0) > radius-1 aligned neighbours > everything else, by
  size (content-fit) and by the one alignment guarantee (V-R17, VS:216-217). Nothing past radius 1
  should visually compete with radius 1 for attention even though both are "real."
- **Gap**: `stretch.gap` = 4px (VS:2657) is small by design — "drawn very closely together." Do not
  grow it for breathing room; if cells feel cramped the fix is `stretch.minContact` (12px, VS:2658),
  which protects legibility of the slide, not the gap.
- **Fade curve feel**: `smoothstep(0, b, e)` (VS:1650) reads as "flat, then a gentle knee, then
  floor" — resist the temptation to linear-interpolate instead; smoothstep's zero first derivative
  at both ends is what keeps the *centre* of the pane from visibly "starting" to fade before the
  outer band.
- **Ghost outline**: stroke-only, no fill, at `stretch.ghostOpacity` (VS:2663) — see Disagreement D1
  on why that literal needs to drop relative to `fadeFloor`.
- **Edge heat**: a glow band hugging the pane's physical edge, never a glow *on* a specific cell —
  it answers "which direction has more," not "which cell." Keep its colour achromatic (a warm
  white/amber wash is customary for "heat," but that collides with the reserved gold band from §2.2
  — use the theme's `muted` colour (`{0.65,0.68,0.72}`, `theme.hpp:38`) brightened toward white with
  opacity, never an actual amber hue).
- **Ticks and breadcrumb**: `Caption` role (small, see §5), always legible even at the pane edge —
  they are orientation aids and must never be among the things that fade.
- **One mistake**: letting the anchored-slide rule (VS:1611-1619) leave visibly uneven whitespace
  pockets where a slide "gave up" early — cap how often `stretch.minContact` is allowed to reject a
  slide before falling back to pushing outward along `u`, so gaps read as deliberate overflow, not
  as a layout bug.

### 3.4 The wheel — all-dim walk (§9.2, VS:1722-1930)

- **Hierarchy**: hub first (capped at `ring.hubMaxShare` = 0.4 of the pane, VS:2669) — a cell must
  never shrink to make room for its own wheel; ring 0 second (it is flat, in the pane's own plane,
  the easiest to read); leaning rings third, read through depth and the camera's fixed
  `restYaw`/`restPitch` tilt.
- **Hub**: content at its full Body-role size; outline at the "focus" treatment (brighter/thicker,
  never re-hued, P5).
- **Slot cells** (`ring.slotWidth`×`slotHeight` = 140×44, VS:2668): fixed size regardless of content
  — this is the one component where cell size is *not* content-fit, on purpose, because the wheel's
  point is valence, not density (contrast with stretch vanishing, §9.1.1).
- **Spokes vs. ring edges**: a bound-point's pair sits exactly on R/U/F — draw it as a straight beam
  through the hub, full `connectionBeamWidthPx` weight (the same weight any other dimension edge
  uses, so a bound spoke doesn't look like a different *kind* of connection, only a differently
  *placed* one). An unbound dimension's ring-slot edge is the same weight; what differs is only
  position, never stroke.
- **Labels**: at `ring.labelAt` = 0.55 along the edge (VS:2670), middle-ellipsised (§5), facing
  camera.
- **Valence badges**: `Caption` role, tabular numerals (§5.3), one per neighbour, never on the hub
  (the hub's own valence is implied by how full the wheel is, which is the whole point of the view).
- **Child wheels, bend and flex**: the bend (VS:1866-1870) should read as "a hand cupped away from
  its parent," not "a shield in front of it" — keep the near edge of a child wheel's arc strictly
  behind its own hub's silhouette as seen from the rest camera, which is what `ring.childGap`
  enforces geometrically (VS:1868); visually, drop the child wheel's overall opacity one step below
  its parent's band so depth order is legible even before the camera orbits.
- **Detail ladder**: every step (VS:1884-1891) must be reversible in one camera nudge — never let a
  wheel "lock in" a reduced level once chosen; re-evaluate every frame the pane, cursor or valence
  changes, with the hysteresis from P7.
- **One mistake**: letting rings 1+ share ring 0's flat styling so strongly that orbiting is the
  *only* way to tell them apart — give leaning rings a faint depth-of-field-style desaturation (a
  few percent toward the pane background colour per ring) so even a static frame hints "this ring is
  further round."

### 3.5 Packs — glue, strands, spread (§9.3.6-7, VS:2036-2095)

- **Hierarchy**: the pack's own outline and header (group name + step) first — it must read as *one
  object* before any lane is read individually. Lane content second.
- **Glue**: a `pack.seam` of 1px (VS:2679) between lanes, drawn as a hairline *inside* the outline,
  never as a second border — a seam is a crease in one surface, not a joint between separate boxes.
  Corner radius applies to the pack's outer outline only; lanes inside stay square to their
  neighbour.
- **Header**: `Title` role (per §12.3, VS:2644), smaller than a page title would be — it names the
  group and the step, e.g. "contact +2," and must never be mistaken for a cell's own content.
- **Empty lane**: `itemViewOnly` chrome, no border at all, just the lane's name ghosted at
  `view.viewOnlyOpacity` (0.7, VS:2655) — an empty lane is not a ghost (nothing was cut off-screen),
  so it must look different from §3.3's ghost: no outline, just absence with a label.
- **Strand thickness**: `pack.strandWidth` = 2 (VS:2680), matching real-cell `border_thickness` — a
  strand is a real connection and should carry the same visual weight as a cell's own border, not
  look like a decorative flourish.
- **Bundle gathering shape**: strands gather to a point partway between packs and fan back out at
  each end (VS:2069, the diagram's "gathered between the packs" shape) — keep the gather point fixed
  at the geometric midpoint, not content-dependent, so the bundle's silhouette is predictable at a
  glance: thick in the middle, feathered at both ends.
- **Tint on face vs. border**: when a strand's join is toward the camera, tint the constituent's
  *face* (25%-intensity wash, matching §2.3's existing fill ratio); when the join is edge-on or
  behind, tint the *border* instead so text contrast is never compromised (VS:2083-2086 already
  specifies this switch — the number to carry over is the existing 0.25 fill ratio, so a tinted face
  doesn't look more saturated than an ordinary cell's own fill).
- **Spread animation**: lanes separate by `pack.spreadGap` (28px, VS:2679) while strands un-gather
  and lane-name labels **fade in only once separation passes a legibility threshold** (recommend:
  once the gap exceeds the label's own measured width at `Label` size, so a label never appears
  overlapping its neighbour mid-spread).
- **One mistake**: letting the glued (non-spread) state's strands touch the pack's outline at a
  visibly different point than where the spread state's un-gathered strand reaches the same lane —
  the join point on the pack's edge must be identical in both states, or the spread reads as "a
  different pack," not "the same pack, opened."

### 3.6 Base view (§10.3, VS:2179-2285)

- **Hierarchy**: the anchor page (unmoved, P4) > flown participants (lifted toward camera,
  `page.base.liftDepth` = 90, VS:2685) > windowed participants (same lift, smaller extent) > ghosts
  at home > non-participants, dimmed to `page.base.contextOpacity` = 0.42 (VS:2688, matching
  `anim::backgroundOpacity` exactly, `animation.hpp:69` — this is already internally consistent;
  preserve the match rather than introducing a second "background dim" literal).
- **Coalescing pages**: level within `page.base.levelTolerance` = 2px (VS:2689) — tight enough that
  "level" reads as level, not "roughly aligned."
- **Ghosts and tethers**: the home-place ghost is the *same* visual language as stretch vanishing's
  cut-cell ghost (§3.3) — outline only, no fill — so a reader who has learned "ghost = not really
  here" from one view recognizes it instantly in the other. The tether (V-R33, VS:254) is a thin
  curved line (reuse the existing tenuous-tether Bezier shape already in `apps/xudu/beams.cpp` /
  `apps/xudu/tenuous_tether.hpp`, which xudu-ui-design/SKILL.md:80-91 describes with a control point
  offset by `(0,0,-15)`) at low opacity (~0.25, matching the existing "tenuous" convention).
- **Windowed pages**: the window band (`page.base.bandContext` = 2 lines either side, VS:2687) needs
  a visible top/bottom edge-fade (a few pixels of gradient into transparency) so it reads as "a
  cropped strip of a bigger page," not as a short page that happens to end there.
- **z-order**: participants (whole or windowed) always draw in front of non-participants, per
  VS:2229 ("stand in front of the row and may cover pages that are not taking part"); a linked
  passage itself is the one thing that must never be covered (V-R32, VS:253) — enforce this as a
  draw-order *and* a layout constraint, not layout alone, in case of late z-fighting from animation.
- **One mistake**: letting the lift depth (`liftDepth` 90, VS:2685) read as "floating," detached
  from the row, when a participant has only partially left its column — keep the lift's start
  keyframe exactly at the page's row position so the motion itself, not a sudden pop, establishes
  the separation.

### 3.7 Stacked vanishing view (§10.4, VS:2286-2447)

- **Hierarchy**: the two tops (current + previous) first, always fully opaque and unobstructed
  (VS:2327-2329); the receding line of the current stack second; the passed stack (tucked or beside)
  third.
- **Paper tone with depth**: do not let the fading pages go desaturated-grey the way distance fog
  usually does — these are still pages of the same document, so fade *opacity* only (toward the
  pane's background, which already reads as "fewer photons," not "less paper"); a hue shift would
  make distant pages look like a different document.
- **Opacity curve feel**: exponential decay (`e^{-i/fadePages}`, VS:2363) with `fadePages` = 12
  (VS:2698) reads as "slow at first, then unmistakably thin" — resist linear fade, which would make
  the first few pages behind a top disappear too fast relative to the twelfth still being visible at
  a third opacity.
- **Marked page solidity**: a marked page is "fully opaque wherever it stands" (V-R35, VS:258) *and*
  carries "an edge tab in its link type's colour" (VS:2367) — use the reserved link-type hues from
  §2.2 for the tab (they are already a closed, known set, unlike dimension colours), drawn as a
  small rectangular flag on the page's outer edge, not a full-edge border (a full coloured border on
  an opaque page would read as "selected," competing with the current-page treatment).
- **Tail marker**: `Caption` role count, drawn as a flat chip, not a page-shaped object — it must
  not be mistaken for one more (very thin) page.
- **Riffle choreography**: each page's arc "lifts it towards the viewer" (VS:2412) — keep the lift
  shallow (a few percent of page width) so a fast riffle doesn't look like pages are being thrown;
  the point is a visible *turn*, not a toss.
- **Split choreography**: the block-move and the lead-delayed fly-in (VS:2415-2417) must feel like
  "the deck parts, then the card we want jumps out" — the delay (`splitLeadMs` = 80, VS:2701) is
  short enough that both read as one gesture, not two.
- **One mistake**: letting a page's Coarse-mode sliver (down to 1px, VS:2375) disappear into
  antialiasing noise before the tail marker takes over — give the thinnest Coarse slivers a small
  fixed-brightness floor independent of the fade curve, so the "punctuation continuing to the
  vanishing point" (VS:2391) stays visible as *lines*, not as a smear.

### 3.8 Panes, dividers, focus; embedding; `u` zoom and its rim band (§11, VS:2478-2570)

- **Dividers**: thin, theme-`border`-coloured hairlines from `ui::PaneTree` — chrome, not content;
  never a dimension colour (a divider is not a dimension's identity).
- **Focus indication**: the pane holding the keyboard gets a subtle full-border glow in the theme
  accent colour at low opacity (just enough to answer "which pane am I typing into" at a glance,
  never a thick frame that eats into the content).
- **Embedding (§11.5, VS:2539-2545)**: the embedded placement's rectangle must visibly sit *in front
  of* its host item (its own depth slice nearer than what it covers, VS:2531-2534) — give it a thin
  "lens" edge (a slightly brighter rim than the embedding frame's own border) so a reader can tell
  at a glance "this is a window into something," not a coincidentally-placed second pane.
- **`u` zoom and the rim band**: the rim band (`subspace.rimBand` = 48px, VS:2654) must stay
  **achromatic** — it is not itself a dimension, it is a navigational breadcrumb ("the containing
  cell's edge," VS:707-708), so colouring it with the entered dimension's hue would wrongly suggest
  the whole inset space *is* that dimension. Use the theme border colour, brightened, with a small
  "step back" glyph; keep it at a fixed screen-relative width regardless of zoom depth, the same
  "never shrink below legible" rule as focus indicators (§6).
- **One mistake**: animating the rim band's own width during the zoom (it should be a constant frame
  round the pane, not a shrinking/growing proxy for "how far in you are" — that job belongs to the
  zoom's own duration and easing, §4, not to chrome that is supposed to be a stable reference).

### 3.9 The `t` step

- There is a ready-made precedent: `d.version`'s existing hard-coded colour is already near-grey
  (`{0.6, 0.6, 0.7}`, `zigzag_visualizer.cpp:999-1005`) — formalize that instinct rather than
  inventing a new one. When `t` is bound to nothing (VS:711-716, VU12), apply a slight
  desaturate-and-cool tint across the whole pane, scaled by distance from "now" (more desaturated
  the further back), so the reader always has a non-verbal "this is not the live state" cue that
  does not depend on reading a version number. Keep it a tint over the existing colours, never a
  colour *replacement* — a fully desaturated pane would erase the dimension-colour identity P5
  depends on.

______________________________________________________________________

## 4. Motion language

Three families already exist and should stay the only three: **chrome** (fast, ~120-220 ms, nothing
in `animation.hpp` names this family explicitly but it is the right register for anything that is
UI, not content), **content** (`anim::docArrival` 450 ms / `anim::sworphSubject` 620 ms,
`animation.hpp:26,47`), and **camera settle** (`anim::cameraSettle` 700 ms + 140 ms delay,
`animation.hpp:58-59`, slowest and last on purpose — "last to move... rather than chasing one still
being made"). Every new transition below is placed in one of these three families rather than given
an independent number, so the whole application reads as one thing.

| Transition                                | Duration                                            | Easing                                        | Family / relation                                                                                                                                                       |
| ----------------------------------------- | --------------------------------------------------- | --------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Step along a dimension                    | 120 ms                                              | ease-out cubic                                | chrome-speed; the single most frequent action, must never lag                                                                                                           |
| Toss — old derived cells leaving          | 90 ms, fade only, **no position tween**             | ease-in (fade)                                | a toss is conceptually instant (§6.5); fading in place, not sliding away, says "this was discarded," not "this moved somewhere"                                         |
| Toss — new derived cells arriving         | 160 ms, fade + slight scale-in (0.92→1.0)           | ease-out                                      | chrome-speed; slightly longer than a plain step because context changed                                                                                                 |
| View switch                               | 220 ms cross-fade of the whole scene                | ease-in-out                                   | between chrome and content; close to `anim::toastFade` (250 ms, `animation.hpp:72`) since it is a mode change, not a move                                               |
| Sub-view switch (spread, heat, depth)     | 150 ms                                              | ease-in-out                                   | chrome-speed; smaller perceptual change than a full view switch                                                                                                         |
| Pack spread                               | 220 ms                                              | ease-out, slight overshoot (3-5%) then settle | content-adjacent; a reveal, matches wheel depth change below                                                                                                            |
| Wheel depth change (`ring.maxDepth` step) | 240 ms                                              | ease-in-out                                   | same register as pack spread — both are "show me more," not "go somewhere"                                                                                              |
| Selector open                             | 180 ms, scale 0.9→1.0 + fade in                     | ease-out                                      | chrome-speed, inviting                                                                                                                                                  |
| Selector close                            | 120 ms, fade only                                   | ease-in                                       | asymmetric with open on purpose — dismissal should feel immediate, unlike the toast's symmetric fade; flagged as a deliberate departure, not an oversight               |
| Selector tier change                      | 140 ms                                              | ease-in-out                                   | chrome-speed                                                                                                                                                            |
| Page fly-in (base view sworph)            | subject 620 ms leads; row 450 ms starts 90 ms later | ease-out, Choreograph damped spring           | **reuse `anim::sworphSubject`/`sworphRow`/`sworphRowDelay` verbatim** (`animation.hpp:47,52-53`) — already mirrored as `page.base.subjectMs/rowMs/rowDelayMs` (VS:2690) |
| Page return (link released)               | same durations, roles swapped (home position leads) | ease-in                                       | symmetric with fly-in                                                                                                                                                   |
| Riffle, per page                          | τ = max(60, min(120, 420/Δ)) ms (VS:2404,2414)      | ease-out, shallow forward arc                 | content-speed per page, but the *whole run* stays under `maxTransitionMs` (420 ms) — one content-family budget for the entire riffle                                    |
| Split                                     | block 320 ms, target flies in starting 80 ms later  | ease-in-out block, ease-out fly-in            | content-speed (`stack.splitMs/splitLeadMs`, VS:2701)                                                                                                                    |
| Pane split / close                        | 150 ms rect tween                                   | ease-in-out                                   | chrome-speed, same register as selector tier change                                                                                                                     |
| Subspace (`u`) zoom                       | 450 ms                                              | ease-in-out, 5% zoom overshoot then settle    | content-speed — matches `anim::docArrival`, because entering a subspace is as big a move as opening a document                                                          |
| Camera reframe (any view)                 | 700 ms, 140 ms delay                                | ease-in-out                                   | **reuse `anim::cameraSettle`/`cameraSettleDelay` verbatim** (`animation.hpp:58-59`) — always last and slowest                                                           |
| Reduced motion (`view.motion.reduced`)    | 0 ms for every row above                            | cut                                           | per VS:2655/2811; camera does not animate at all, it jumps                                                                                                              |

**What must never animate**: the camera's field of view / projection shape (only position and
orientation tween, never a FOV pop — a changing FOV reads as a zoom lens racking, which has no
meaning here); a ghost's *position* (ghosts appear/disappear by opacity alone — sliding a ghost
would imply something is really arriving, contradicting P6); glyph-level text reshaping mid-tween
(only the whole fitted-text plane's transform/opacity animates, per V-R44, VS:280 — `FittedText` is
retained geometry and must stay that way through a tween); and the rim band's own width during a `u`
zoom (§3.8).

One existing idiom worth keeping, cell-specific only: the focus scale-pulse on arrival, 1.25× for a
beat (`zigzag-ui-design/SKILL.md:87`). Reuse it when a cell becomes newly accursed in any slice
view. Do **not** apply it to pages — a page popping 1.25× reads as a bounce, the wrong register for
paper; pages get the sworph treatment instead (above).

______________________________________________________________________

## 5. Typography and density

### 5.1 Role assignment

| Text                                                          | Role (per VS:2644)                    |
| ------------------------------------------------------------- | ------------------------------------- |
| Cell content (stretch vanishing, wheel hub/slots, pack lanes) | `Body`                                |
| Dimension/lane names, compass arm labels, breadcrumb strip    | `Label`                               |
| Valence badges, lane counts, tail markers, quick-key digits   | `Caption`                             |
| Pack header (group name + step)                               | `Title`                               |
| Selector tier-3 items                                         | `Body` (they are drawn as real cells) |

`theme.hpp:45-49` currently sizes `Caption`/`Label`/`Body` at 10/12/12 pt and `Title` at 18 pt bold
— the new views inherit this without adding a sixth role, per VS:2643 ("No setting names a font or a
text size").

### 5.2 Minimum sizes and the fallback staircase

The hard floor is `ui.minFontPx` = 9 (`theme.hpp:29`). `PaneFrame::minReadableLinePx` (VS:1183) is a
*second*, higher threshold that should sit at roughly 1.6-2.0× the hard floor (≈14-18px) — recommend
pinning it there explicitly, because the spec defines the field but not its relationship to
`minFontPx`, and without that relationship a page or wheel label could jump straight from crisp text
to `Coarse` bars with no `Abbreviated` step in between. The staircase should always be
`Full → Abbreviated → Coarse → Badge → None` (the `ContentMode` enum's own order,
`view_records.hpp:1197`), never skip a rung, with the hysteresis from P7 at each threshold.

### 5.3 Line length and truncation

Cell content should wrap at 45-75 characters at `Body` size — translate `zigzag.contentMaxWidthPx`
to roughly 30× the `Body` font's average advance so it stays in that range as `ui.scale` changes (it
already does, since the setting is converted through `UiMetrics::px()`, `src/ui/metrics.cpp:27`).

Two different ellipsis rules, by what kind of text is being cut, not by which component draws it:

- **Dotted identifiers** (dimension names like `d.contact.phone.mobile`, VS:1821) use **middle**
  ellipsis — both the namespace prefix and the leaf name carry distinguishing information, so
  cutting the middle keeps both (this is already specified for the wheel's labels, VS:1821;
  generalize it to every dimension-name label anywhere, including the compass and the selector).
- **Prose** (cell text previews, page passage previews, pack header text) uses **end** ellipsis —
  the reading sense front-loads, so cutting the end keeps the part a reader scans first.
- **Badges and counts never ellipsise** — a truncated number is a wrong number, not a shortened one;
  widen the badge instead.

### 5.4 Numerals in badges

Use the `Mono` role's figure metrics (fixed advance, `theme.hpp:49` already reserves `Mono` for this
kind of thing) for the *digits* of any badge/count, even though the badge is otherwise
`Caption`-sized — a valence badge going from 9 to 10 must not reflow its neighbours, which
proportional digits would do. This is a direct instance of P4 ("what stays still"): a count changing
should not nudge anything beside it.

______________________________________________________________________

## 6. Accessibility as design

**Contrast at the fade floor is a real number, not a slogan — and the current defaults do not clear
it.** `theme.colours.text` is `{0.95, 0.95, 0.96}` (near-white) over a surface around
`{0.12, 0.14, 0.18}` (`theme.hpp:35-37`). Naive alpha compositing of text at `stretch.fadeFloor` =
0.15 (VS:2661) over that surface gives an apparent luminance of roughly
`0.95×0.15 + 0.13×0.85 ≈ 0.25`, against a background luminance of `≈0.13` — an approximate contrast
ratio around **1.7:1**, far under the WCAG body-text minimum of 4.5:1 and even under the large-text
minimum of 3:1. §15's own claim ("the floors' defaults are chosen for that," VS:2809) does not hold
against the theme as it stands. The fix is architectural, not a bigger floor number: a cell whose
*content* would fall below a text-contrast floor should drop to `ContentMode::Abbreviated`/`Coarse`
(which draws bars, not low-alpha glyphs) rather than keep rendering `Full` text at an unreadable
alpha — decouple "how transparent the box is" from "whether its text is still legible," because
today's single `fadeFloor` conflates the two.

**What replaces colour**: the dash/glyph pair from §2.4, always paired with full text (never
colour-only identity, already required by V-R43, VS:279).

**What replaces motion**: `view.motion.reduced` already cuts every tween (VS:2655, 2811). For a
screen-reader user the equivalent is an announcement for *every* state change that would otherwise
be conveyed by seeing something move. §12.2's message table (VS:2626-2637) is a good start but is
missing two rows a transition-heavy system needs: a **view-switch announcement** ("Switched to *view
name*.") and a **sub-view announcement** ("*view name*: now showing *subview name*.") — add both;
without them a screen-reader user who presses `view.cycle` gets no feedback at all that anything
happened.

**How a screen-reader user perceives a wheel, a pack, a deck**: already specified and good —
dimension child-groups in ring order for the wheel (VS:1904-1906), `group`-role packs named "*group*
step *n*: *k* of *m* lanes" (VS:2118-2119), decks as named lists of "page *n* of *N*"
(VS:2434-2435). One addition: expose each dimension's `dash`/`glyph` class as a short suffix on its
accessible description (e.g., "(dotted)") so a low-vision user combining a screen magnifier with
residual colour vision can cross-reference the spoken class against what they can see, rather than
relying on colour alone even when they can perceive some of it.

**Focus visibility at every depth**: a focus outline that only scales with its plane (correct per
V15, VS:3031-3038 — it is a world-space object) can shrink below legibility on a distant wheel
neighbour or a deep pack lane. Use two layers: the outline itself scales with the plane (so its
exact bounds are always truthfully shown), plus a small four-corner tick motif clamped to a
**minimum screen size** that never shrinks below roughly `ui.minFontPx`-equivalent screen pixels
regardless of world distance — the same "never disappear" treatment §3.8 already applies to the
subspace rim band.

______________________________________________________________________

## 7. Aesthetic acceptance checklist

Numbered; `[auto]` means assertable purely from `LayoutSink` records or settings values with no
human judgement; `[human]` needs a captured frame or recording.

1. `[auto]` No two `PlacedItem` boxes in the same depth plane overlap, except the base view's
   explicitly-sanctioned coalesce/windowed overlap (V-R32/V36) and a pack's own constituents (glued
   by design).
1. `[auto]` Every `Marker` with `itemGhost` has opacity ≤ the minimum opacity among same-pane
   `PlacedItem`s with `ContentMode::Full`/`Abbreviated` — this directly checks P6/D1 and will fail
   today's literal defaults (0.25 vs. 0.15) until they are reconciled.
1. `[auto]` No two items of `SubjectKind::Label` have intersecting boxes ("no label crosses another
   label").
1. `[human]` Where two labels are near in screen projection but far apart in world depth, the nearer
   one reads clearly in front — geometry alone cannot judge perceived legibility of a near-overlap
   at an angle.
1. `[auto]` During a riffle, sampling every `MotionHint`'s interpolated position at a shared time
   `t`, no two concurrently-flying pages of one run are further apart than `stack.gutter` (48px,
   VS:2691).
1. `[auto]` Every `PlacedEdge` of kind `Strand` sharing a `bundle` id has the same two `gather`
   points, and its colour equals `ui.dimension.<name>.colour` for its `relation`.
1. `[human]` A captured frame of a full-detail wheel reads as a wheel, not a flower or a cone — no
   automatic geometry check substitutes for this holistic read.
1. `[auto]` Every assigned dimension colour has a contrast ratio ≥ 3:1 against the pane background
   at `pack.strandWidth`/`ring` stroke weights (thin strokes need more contrast than fills).
1. `[auto]` No two dimensions present in one slice have hues within the chosen colour-blind
   confusion threshold under a protanopia/deuteranopia simulation matrix — catches a defect in the
   generation rule itself, not just a bad reader override.
1. `[human]` A glued pack is agreed, by eye, to look like one object rather than several adjacent
   cells — seam width and tint can be checked automatically (#1-type overlap and the fill-ratio from
   §2.3), but the gestalt is a human call.
1. `[auto]` In the `spread` sub-view, a lane-name label's opacity is zero until lane separation
   exceeds that label's own measured width at `Label` size.
1. `[auto]` With `view.motion.reduced` set, a captured sequence across a bind/toss/transition shows
   exactly one old-state frame followed by one new-state frame, with zero intermediate tween frames.
1. `[human]` Orbiting the camera through a captured sequence, a person can tell which ring of a
   wheel or which page of a deck is nearer without reading any label.
1. `[auto]` Opacity is non-increasing with depth along a leaning ring or a deck stack (already part
   of the spec's own §9/§10 acceptance lists, restated here as an aesthetic, not just correctness,
   check).
1. `[human]` No middle-ellipsised dotted identifier in a captured frame is confusable with a
   different, shorter real dimension name also present in the slice.
1. `[auto]` A focus indicator's corner-tick motif never projects smaller than its configured minimum
   screen size, at any world depth.
1. `[auto]` Every `SubjectKind::Badge` item's digit glyphs resolve through the `Mono` font role (or
   an equivalent fixed-advance metric), lintable the way `tools/check-ui-text-policy.py` already
   lints other text policy.
1. `[human]` After a reader recolors a dimension through the override UI (§2.6), the result still
   reads clearly against the dark theme at the fade floor — automatable in part via check #8, but
   whether the reader's own pick is *pleasing* is a human call.
1. `[auto]` Every binding point, bound or not, has a non-empty accessible name on the compass (an
   unbound arm still announces, e.g., "x: unbound").
1. `[auto]` No `SubjectId` resolving to a group ever carries a dimension-palette colour (lints the
   presenter's colour source against §2.5's rule that groups are achromatic).

______________________________________________________________________

## 8. Where visual review belongs in the implementation order

Mapped against §17's migration steps (VS:2870-2955):

- **Before step 3 lands** (view space/binding model, no application change yet): prototype the
  **dimension-colour generation rule** (§2) as a free-standing, engine-free tool — generate 40
  swatches via the golden-angle rule, render them as a strip, and by eye confirm no two
  neighbouring-by-hue swatches are confusable and none lands in a reserved band. This costs nothing
  (a static image, no GPU, no store) and must happen first because `ui.dimension.<name>.colour`
  (VS:2706) is referenced by every one of steps 4, 5 and 8; discovering the rule needs revision at
  step 8 means revisiting dozens of call sites instead of one swatch strip.
- **Alongside step 3**, not deferred to its first consumer in step 4: pull the **shared ghost,
  edge-heat and pack-presentation chrome** (`pack_presentation.{hpp,cpp}`, the ghost/marker drawing
  rules of §3.3/§3.6) into one place before stretch vanishing, the wheel and the pack view are built
  in sequence (as §17 step 4 lists them). Otherwise each of the three views will independently guess
  at ghost opacity, seam width and fade feel and visibly drift from each other — the single biggest
  coherence risk this migration order creates, because step 4's three sub-steps are sequential, not
  simultaneous.
- **Before step 4's golden layouts are locked for stretch vanishing**: run the anchored-slide
  placement and fade curve through the text raster (§8.7, VS:1475-1491) over synthetic slices of
  varying content size and valence, and *read the raster output*, not just assert on numbers — the
  spec itself says two alternatives (track tables, relaxation) were weighed on exactly this kind of
  qualitative judgement (VS:1631-1635), so the review tool the spec already built for this purpose
  (the raster) should be used before, not after, the golden fixtures are written.
- **Before step 4's wheel acceptance tests are finalized**: the wheel's bend/flex/lean formulas
  (§9.2.3, §9.2.8) are geometric guesses never validated against a real valence distribution.
  Capture a dedicated throwaway scene (via the `compare-backends.sh` convention) with synthetic
  cells of valence 2, 8, 20 and 60, and tune `ring.radius`/`radiusStep`/`tiltStep`/`childBend` by
  eye — this is squarely a "does it look like a wheel, not a flower or a cone" question (checklist
  #7), which the acceptance test (§9.2.11) checks the *mechanism* against, not the *look*.
- **Before step 5's base-view parity check is declared done**: step 5 already requires numeric
  parity with today's `LinkBeams` (VS:2922-2923, "reproduces `LinkBeams`' result"). Capture the
  *old* and *new* renderers side by side on the same fixture, not just assert the numbers match —
  because the moment windowed/overlapping pages (V-R32/V36, new behaviour even within the "parity"
  step) are exercised, "numerically reproduces" and "looks the same" stop being the same claim.
- **Before `stack.search.*` defaults are locked** (step 5, stacked vanishing): run the direction
  search (§10.4.3) against at least one very tall/narrow page and one wide/landscape page and
  capture both — a wrong default can make every document "read sideways" in a way the acceptance
  test (§10.4.10, which only checks internal score consistency) cannot catch.

**What should be built first so the application looks coherent at every intermediate stage, not only
at the end**: the colour-generation tool and the shared ghost/pack-presentation/focus-indicator
chrome, both ahead of their first real consumer. Step 6's own plan already protects correctness this
way — wrapping the legacy visualizer and `xanadu::Views` so "nothing regresses while the new views
become selectable beside them" (VS:2929-2931) — the same protection is needed for *look*, not just
behaviour: if the shared aesthetic primitives land after the first view that needs them, the second
and third views built in step 4's sequence will each invent their own answer and the whole screen
will look like three different pieces of software for however long that gap lasts.

______________________________________________________________________

## 9. Disagreements with the spec

**D1 — The ghost-vs-fade-floor ordering is backwards in the stated defaults.**
`stretch.ghostOpacity` = 0.25 is *brighter* than `stretch.fadeFloor` = 0.15 (VS:2661, 2663). A real
cell faded to the floor near the pane's edge is therefore dimmer than a ghost standing for a cell
that is not even being shown — the opposite of what a ghost should communicate, and exactly the
failure mode the task's own example acceptance check (§7, #2) is written to catch. I would not just
swap two literals (they are independent `SettingSpec`s a later edit could re-diverge) — define
`ghostOpacity` as a fraction of the *live* `fadeFloor` (e.g. `0.6 × fadeFloor`) so the invariant
holds by construction, and add a settings-harmony check in the spirit of
`tools/check-config-harmony.sh` that asserts it.

**D2 — §15's contrast claim does not survive contact with the actual theme numbers.** "Faded content
must still meet contrast at the fade floor against the pane's background... the floors' defaults are
chosen for that" (VS:2809) is not true of `stretch.fadeFloor` = 0.15 against
`theme.colours.text`/`surface` as they stand today — see §6's computation (≈1.7:1, versus a 4.5:1
text minimum). This is more than a tuning nit: it is an accessibility promise the document makes
that its own numbers do not deliver. I would decouple "how transparent a cell's box is" from
"whether its text is still legible," switching content to `Abbreviated`/`Coarse` once the contrast
floor would be crossed, rather than letting `Full` text fade past readability.

**D3 — "A palette" is left undefined, and the obvious seed re-creates an existing collision.**
VS:2706-2708 says a dimension's colour is "assigned from a palette when first seen" without saying
what the palette is or what it must avoid. The most natural migration path — seeding `x`/`y`/`z`'s
first colours from the existing convention described in `zigzag-ui-design/SKILL.md:72-74` (cyan,
emerald, amber) — puts the very first dimension colour assigned squarely in the gold/amber band
already spent on Identity Gold transclusion and the ZigZag focus highlight (§2.2). The spec should
state the reserved-hue exclusions explicitly and say that `x`/`y`/`z`'s legacy colours are *not*
carried over, rather than leaving "a palette" to be invented ad hoc at implementation time.

**D4 — A heavily-linked document defeats the stacked vanishing view's whole premise.** "A marked
page is the exception: it is fully opaque wherever it stands" (VS:2366-2367) combined with "marked
pages go on being emitted beyond \[`minPagePx`\]... as Coarse slivers" (VS:2374-2375) means a
document where a large fraction of pages are marked would render as a near-solid wall of opaque
paper, with no sense of depth or recession — precisely the cue this view exists to provide (§10.4.1,
"to see at a glance how long the document is"). A link-dense document is not an edge case; cross-
referenced technical documents and heavily-annotated texts are exactly what this view is for. I
would keep the page itself fully opaque (so "is this page connected" stays a yes/no fact, not a
matter of squinting) but fade the **edge tab's** saturation/size along the same recession curve as
unmarked pages, so "there are many connections, and most of them are far back" remains visually
distinguishable from "there are many connections right here."

**D5 — Ring order's "first met" rule makes a wheel's look depend on the reader's history, not the
cell.** V-R24 (VS:232-233) guarantees a *given* cell's dimension order is stable session to session,
which is necessary; but because a never-before-seen dimension is "appended to the ring order the
first time it is met" (VS:1811), two structurally-identical cells visited in different orders
earlier in the session will show their shared dimensions at different clock positions from each
other. The spec's own acceptance test (§9.2.11, VS:1912-1913) only checks that the mechanism
reproduces its own prior output, not that two similar cells agree — so this passes every stated test
while still being a visible, surprising inconsistency ("`d.email` was at 2 o'clock on the last three
contacts; why is it at 9 o'clock on this one?"). I would seed a session with no history yet from a
stable fallback — e.g. the colour-generation ordinal `n` from §2.3, since it is already a total
order over dimensions that exists independently of any one cell — so a fresh structurally-identical
twin matches its siblings until the reader's own activity diverges it, rather than diverging from
the very first visit.
