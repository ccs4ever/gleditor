# Deferred: weak preference for shallower authored beam routes

**Accepted as a secondary layout preference rather than a universal angle rule.** The device-free
prerequisite below is implemented; renderer integration remains deferred. Grounding is
`LinkBeams::alignPair()` and the native `-10` continuous scenes.

The cyan ribbon is now visible, but its long steep run in a tiny gutter remains visually compressed.
A more balanced relative placement could make two neighboring relations easier to read. This is an
arrangement problem: changing NURBS weights can smooth the path but cannot remove the real vertical
separation of its exact anchors.

Current alignment gathers a min/max envelope, aligns its midpoint and adds one `TensionConstraint`.
It is not an objective over every visible occurrence. An envelope can be dominated by distant
authored members, including offscreen spans; one-link or one-strand alignment can also privilege the
active route while leaving a neighbor steep. Before replacing it, preserve existing exact-anchor and
camera contracts.

## Recommended later contract

- Pin the reader's current exact occurrence and its screen location. Do not move the reading
  document or pan/zoom the reader's focus merely to improve a diagram. Selection alone must not
  trigger global rearrangement or a completed visit.
- Evaluate actual visible local occurrence attachments and their rendered scaffold branches. Never
  pretend a two-endset link is a set of authored pairings; never materialize the Cartesian product.
  Count each represented occurrence fairly so a prolific link cannot dominate through duplicate
  scaffolding.
- Add a low-weight cost for projected steepness, with robust saturation and a minimum
  horizontal-distance denominator. Prefer reducing extreme steepness where inexpensive, not forcing
  every route to one angle or levelling every anchor. Degree/curvature weights remain aesthetic
  geometry parameters, not a substitute for placement.
- Bound translation and horizontal gap changes; preserve page nonoverlap, readable scale, visible
  exact spans and fixed reading focus. A shallower slope bought by an enormous gap or by putting the
  counterpart offscreen is not an improvement. Existing controls and chrome must remain clear.
- Use a fixed small iteration budget, deterministic ordering and resting destination transforms.
  Update after an explicit settled layout event, not continuously on every scroll or selection tick.
  Add hysteresis around visibility changes; freshly built pages must not make the scene hunt for a
  new optimum.
- Weight focus/readability constraints before slope. Crossing count, stable relation identity, text
  clearance and document motion are separate constraints. Conflicting or many-to-many scenes may
  retain steep routes with honest local anchors and navigation access.

A useful acceptance comparison is the same two visible KJV relations before/after: focused text
stays at the same screen location, both bodies remain visible, the worst gratuitous slope decreases
without losing readable note/context or increasing total movement markedly. Include contradictory
anchor orders and sparse first/middle/last members to show graceful refusal. No claim is made that
this objective, solver or test currently exists.

## Device-free prerequisite

`apps/common/xanadu/shallow_alignment.hpp` and `.cpp` implement `preferShallowAlignment()`. It takes
a finite snapshot of exact visible fragment attachments, scoped link/side/occurrence/fragment keys,
and explicit projected-pixel limits. It returns a counterpart vertical displacement and objective
measurements; it changes no document, camera, selection, activity or store. Horizontal gap and
readable scale stay fixed. This conservative first step cannot improve a case that needs horizontal
rearrangement, and may honestly return zero.

Duplicate scaffold references count once. Different fragments of one occurrence share that
occurrence's vote. Each link's two side centroids form a gathering scaffold with two hubs; cost
includes every represented branch and one trunk, without making left-by-right authored pairings. The
bounded `atan` slope term uses a positive minimum horizontal run. A configurable worst-route penalty
addresses the aesthetics review's concern that a saturated average can prefer one flat and one
extreme route over a balanced compromise. A quadratic movement penalty keeps the preference weak.
The solver evaluates a fixed candidate budget in stable key order and accepts a displacement only
above an explicit improvement threshold. Invalid or conflicting snapshot data and excess work and
numerical overflow produce typed refusals, never silent truncation.

The caller supplies every coefficient and the displacement interval. The interval must include zero
and preserve viewport/chrome clearance for the whole movable counterpart. This host contract is a
prerequisite, not something the device-free function can infer from attachment points alone. There
are no user settings yet because no live UI consumes this function.

`tests/xuzz/shallow_alignment_test.cpp` exercises the shared-companion two-link compromise, the
corresponding synthetic 18-pixel gutter, duplicate/order invariance, multiple-fragment fairness,
offscreen sparse members, contradictory anchor order and zero horizontal run, unchanged/refused
layouts, and invalid/work-limit handling. These are solver checks, not native UX proof.

## Why live integration remains deferred

The current `alignPair()` runs automatically as a newly built strand becomes ready. It can move the
source row and reframe the camera, and its envelope includes remote pages. Reusing that callback
would violate the pinned reading contract. `LinkBeams::drawFrame()` has visible fragment geometry,
but its present gathering hubs use the first visible fragment per representative endpoint, whereas
the prerequisite evaluates all supplied fragment attachments. A host must first reconcile the
rendered scaffold with the snapshot objective; testing a hypothetical scaffold is not evidence that
the current cyan route improves.

The next live seam should expose a visible **Arrange link companions** action and **Restore
arrangement**, with default keymap and accessibility access. It should refuse or allow cancellation
while exact geometry or scene motion is pending, snapshot only settled projected geometry, pin the
current reading document and camera, bound companion movement against full document/chrome
clearance, and recheck the snapshot before applying motion. Selection, preview, scrolling, late page
construction and ordinary frame ticks must never trigger it. Restore is view-only and records no
completed visit. A refused arrangement must leave all member/occurrence choices available.

The native acceptance comparison remains pending: the same KJV two-link frame before/after on all
backends, fixed reading text screen position/scale and source hashes, both routes visible, lower
worst gratuitous slope, no extra visits, and stable refusal for sparse/conflicting endsets. Until
that proof, shallower alignment is **not available in xuzz**.
