---
name: xuzz-ui-design
description: >-
  Design and build user interface for xuzz, the one program for xanadoc pages and ZigZag slices:
  page and slice presentations, the unified view, overlays and panels, motion, colour, input and
  accessibility. Brings the combined Xanadu and ZigZag interface principles together with a UX
  expert's and an aesthetics expert's review. Use when designing, implementing or reviewing anything a xuzz user sees or
  operates; covers link selection, endpoint browsing and activity walks; use ux-validation to prove a journey
  works in the build.
---

# Xuzz UI design

`xuzz` shows xanadocs and ZigZag slices in one space. A reader moves between page text and cell
content without crossing an application boundary, so a design that serves only one of the two is
half a design. This skill gives the principles that apply to both, says where UI code goes, and adds
two reviewers: a UX expert who speaks for the person using it, and an aesthetics expert who answers
for how the whole program looks.

Check the code before designing against it. A design document's proposed behaviour is not evidence
that the behaviour exists.

## What is there today

- **The program** is `apps/xuzz/`. `xuzz_app.cpp` builds every component and registers it with the
  renderer three ways: as a frame contributor (it draws), as a pick observer (it takes pointer
  input) and as an accessibility source. A component missing from one of the three is a bug a user
  will find. `ViewCoordinator` switches between three modes: unified, xanadoc only and ZigZag only.
- **Xanadoc components** are in `apps/common/ui/xanadoc/`: the session and its views, link beams,
  the tenuous tether, the satelloid, the kinetic tether, the wireframe hull, the link panel and link
  context, the overview, the pouch drawer, the swarm telescope and the transcopyright overlay.
- **Slice components** are in `apps/common/ui/slice/`. `ZigzagVisualizer` has two presentations:
  cell content (cells sized to their content, pulled close to the focus) and topology (fixed-size
  cells on a rigid lattice).
- **Device-free layout** is in `apps/common/xanadu/`: `tension_layout.cpp` (the spring solver that
  balances readability, alignment and tidiness), `link_layout.cpp`, `bridge_config.cpp`.
- **Generic UI** is in the library and is used, not rebuilt: `ui::Theme`, `ui::UiMetrics`,
  `ui::FocusManager`, the layout and widget headers under `include/gleditor/ui/`, `text::fit()`,
  `RadialMenu`, `DocumentSwitcher`, `Canvas`.

New views are specified in [`design/view-system.md`](../../../design/view-system.md), with the order
of work and the visual direction in
[`design/view-system-implementation-plan.md`](../../../design/view-system-implementation-plan.md).
Neither is built. A new presentation is a registered view over a `ViewManifold`, not another case in
`ZigzagVisualizer::ViewMode`.

## Where UI code goes

- Layout that needs no graphics device goes in `apps/common/xanadu/`. It sees text only as sizes a
  measurer returns, and is tested from `xuzz_test` with no device.
- Anything that draws or takes input goes in `apps/common/ui/`.
- `apps/xuzz/` holds only the wiring: construction, registration, the command line. A component
  written there that another front end could use belongs in `apps/common/ui/`.
- Something generic enough for the plain editor belongs in the library, named for what it does and
  not for a cell, a link or a page.

## Principles

**One space, no walls.** Documents, sources, link packages and cells appear where they are
referenced and stay connected to where they came from. A page that flies forward to be read keeps a
faint tether to its parent. Entering a cell from a page, or a page from a cell, keeps the origin on
screen as a companion.

**Stored structure and view-only chrome never look alike.** A stored cell or page is hard-edged. A
ghost, a badge, a pack or an empty lane is soft-edged or seamed, and is never brighter than the
dimmest real thing in the frame.

**A cell exists only where an operation minted one.** A view may draw things that stand for
structure, but it does not mint cells or append operations to a store the reader is only visiting.
Camera and cursor movement are ephemeral.

**Structure belongs in cells and dimensions, not in markup.** If a design needs a cell's text to be
parsed to find a role, a media reference or a heading, give it another cell on another dimension.

**One link has one identity.** However many strands, brackets and endpoints a link draws, the reader
selects and follows one link with two endsets. Do not let a rendered strand stand in for a pairing
the store does not hold.

**Depth and colour carry meaning.** Depth is a step along something bound, a position in a deck or a
lift towards the reader; it is not decoration and not the only cue. Colour is identity: a
dimension's or link type's hue does not change on hover, focus or selection, which change
brightness, weight or outline. Every colour has a second cue for readers who cannot use it.

**The thing to follow moves first and longest; the anchor stays still.** The focused cell and the
anchor page do not shift to make room; the camera does. Input never waits for a tween, and with
reduced motion every transition is a cut.

**Detail falls in steps.** Text gives way to a shorter form, then to bars, then to a count. Nothing
shrinks continuously until it cannot be read, and text is fitted to its box, never cut by byte.

**Provenance is ambient.** Whose words these are, and from which version, is shown quietly at the
edge of the content and is always available. It does not need a window frame to say so.

**Reading comes before the diagram.** When readable text, aligned link ends and a tidy arrangement
pull against each other, readable text wins, then alignment of the thing being followed.

**Nothing is hardcoded.** Every action has a default chord in `system://keymap` and a Vortex routine
behind it. Every spacing, timing, colour and threshold a user could want to change is read live from
a system xanadoc, with an entry in `defaultSettingSpecs()`. A number written into a layout routine
is a finding.

**The render path does not allocate or block.** No heap allocation in a hot draw loop, no network or
content resolution on the UI thread, and a settled frame shapes no text.

## Links and navigation

For a change to how a reader selects, browses or follows links, the interaction contract and
acceptance criteria are in
[`design/ui_workflow_xuzz_navigation.md`](../../../design/ui_workflow_xuzz_navigation.md). Read it
first, then inspect what is wired: `apps/common/xanadu/ops.hpp`, `link_layout.cpp` and
`link_views.hpp`; `beams.cpp`, `bridge_coordinator.cpp` and `link_context.cpp` in
`apps/common/ui/xanadoc/`; and the setup in `apps/xuzz/xuzz_app.cpp`.

- **One navigation session per selected link.** It holds the link's authority and ID, both ordered
  endsets, independent left and right member and occurrence cursors, the active side, and the visit
  it started from. Resolve each member to exact document or cell occurrences, and never widen
  disjoint members into one highlighted extent.
- **One command boundary.** Select link pins context without moving focus. Select member moves one
  side's cursor. Cross link changes the active side. Enter endpoint focuses the explicitly chosen
  occurrence and records a completed visit. Beam picks, cell picks, keymap actions and accessibility
  actions all go through this boundary, and a beam with no unambiguous source member selects its
  link for disambiguation.
- **Direction comes from the selected side or the hit target,** never from which document the caret
  happens to be in, and never from whichever rendered strand is easiest to reach.
- **Walks persist in the reader's own store.** Completed focus transitions append to
  `system://activity` and to nothing else; live cursor and camera movement append nowhere. A new
  route from an old visit adds a child and keeps its siblings, and restoring a visit appends
  nothing. Activity branches are not document microversion branches. Unreadable activity data is
  preserved and reported, not regenerated.
- **Crossing between page and cell** carries link ID, side, member, occurrence and exact range.
  Materialise a distant cell on demand, keep the source as a companion, and keep the link context
  visible while either view scrolls or changes dimension. Never fall back silently to another link
  or occurrence.
- **Bound the drawing.** Every member of the selected link stays individually pickable without
  drawing every left-by-right strand. Measure selection and staging on a large endset before setting
  a performance threshold, and keep endpoint resolution off the UI thread.

For a substantial change, add two checks to the experts' reviews below: trace `Link` and its endsets
through the store to confirm one identity, complete endsets and provenance survive; and estimate the
fan-out of endpoint resolution and ribbon staging. A disagreement is settled against the stored link
model and a headless interaction test. The proof is journey J16 in
[`design/ux_workflow_real_work.md`](../../../design/ux_workflow_real_work.md), run through pointer,
keymap and accessibility with the same link ID, member, occurrence and visit identity from each.

## The UX expert

Bring this role in at the start of a design and again before calling it finished. With subagents
available, give it to one that did not write the design or the code; otherwise take it up yourself
as a separate pass. It reviews from the user's seat and does not edit product code.

It asks of every design:

- **Can a person find it?** Each action is reachable from something visible: the radial menu, the
  command bar or palette, a panel or drawer, or a default key binding. An action that exists only as
  a flag, a script or an unbound command does not exist for the user.
- **Does it behave the same from every input?** Pointer, keyboard and the accessibility tree reach
  the same command and leave the same state. Every control has an accessible name, and focus is
  never lost.
- **Does the reader know where they are?** What is selected, what is focused, which side of a link
  is active and how to get back are all on screen. Selecting something does not move the reader;
  entering it is a separate, labelled act.
- **Can it be undone or abandoned?** A pending action can be cancelled. Loading, out of range,
  missing version and unavailable are told apart, and nothing is silently swapped for a near match.
- **Is it legible?** Text contrast is at least 4.5 to 1 at every opacity at which text is drawn,
  labels do not overlap, and a dense case (many links, a high-valence cell, a long rank) still
  reads.
- **What does it cost to learn?** A new gesture or mode has to earn its place against one the reader
  already knows. Prefer extending an existing control to adding a new one.
- **Does it hold up for real work?** Walk the design through the relevant journeys in
  [`design/ux_workflow_real_work.md`](../../../design/ux_workflow_real_work.md), including closing
  the program and coming back.

It reports findings as what the user tried, what happened and what should happen, ranked by how
badly each blocks real work. A disagreement with the designer is settled by running the step and
reading the captured frame, not by argument.

## The aesthetics expert

Its mandate is that every element a user sees is visually coherent with every other, attractive in
its own right, and professional and slick: finished to the standard of software people pay for, with
nothing rough, improvised or left at a default. The UX expert asks whether a person can use the
thing; this role asks whether the program looks like one made thing, and a good one. Bring it in at
the same two points, and give it, too, to someone who did not write the design or the code. It
judges captured frames and recordings, never a description, and does not edit product code.

It works from the visual language already built (`ui::Theme`, `ui::UiMetrics`, the beam and cell
drawing in `apps/common/ui/`) and from §5 of the implementation plan, with the fuller reasoning in
[`design/projects/view-reviews/04-aesthetics.md`](../../../design/projects/view-reviews/04-aesthetics.md).

It asks of every design:

- **Does it belong to the same family?** Corner treatment, stroke weight, spacing, type roles and
  shadow come from the theme and metrics the rest of the program uses. A component that invents its
  own radius, padding or font size is a finding, however good it looks alone.
- **Is the colour system kept?** The hues that already mean something stay reserved for it: gold and
  amber for transclusion and focus, cyan and blue for accent and comment links, purple and violet
  for authorship and link ribbons, green for quotation, red for disagreement. A new element takes
  its colour from the system or argues for a new entry in it.
- **Is there one hierarchy?** In any frame the eye has one place to land. Chrome is quieter than
  content, a placeholder is quieter than what it stands for, and emphasis is spent on one thing at a
  time. Three things at full strength is noise.
- **Does it move as one program?** Motion falls into three families: chrome is quick, content reuses
  the timings documents already move with, and the camera is last and slowest. A new transition
  joins a family instead of bringing its own curve and duration.
- **Is it composed?** Alignment, rhythm and proportion hold at rest and when the view is dense or
  nearly empty. Edges line up with something, gaps repeat, and nothing is placed by a number chosen
  to make one screenshot work.
- **Is it attractive, and why?** Say what makes it so (restraint, contrast, proportion, a clear
  focal point) or what is missing. "Looks fine" is not a review. Where two treatments both satisfy
  the rules, choose, and give the reason.
- **Is it finished?** Professional and slick means the details are done: edges are crisp, text sits
  on its baseline, icons and glyphs share a weight, hover and pressed states exist, transitions
  start and end cleanly, and nothing flickers, pops or jumps. A placeholder, a debug tint or a
  default-looking control left in a frame is a finding.
- **Does it stay attractive across backends and scales?** The same frame through every compiled-in
  backend and at more than one UI scale keeps its weights and spacing.

It reports each finding against a captured frame: what is inconsistent or unattractive, the element
it should match, and the specific change. Where a treatment is new and good, it proposes the rule
that would let the next component reuse it, so the look is written down once and not reinvented.
When it and the UX expert pull apart, usability wins and the aesthetics expert finds the
best-looking form of the usable answer.

## Working through a change

1. Read the code that draws the thing today and the design section that covers it. State what is
   wired now and what the change adds.
1. Decide what is stored and what is view-only, and which layer each part lives in.
1. Design the states before the picture: empty, loading, dense, unavailable, reduced motion,
   keyboard only.
1. Have the UX expert and the aesthetics expert review the design. Resolve what they find before
   building.
1. Build the device-free layout first with its tests, then the drawing, then the wiring in
   `apps/xuzz/`: frame contributor, pick observer, accessibility source, keymap default, settings.
1. Verify, then have both experts review the result from captured frames.

## Verifying

Everything runs headless (`AGENTS.md`). Capture frames and accessibility dumps and read them; a
program that draws nothing still exits 0.

- Layout is checked by tests in `tests/xuzz/` with no graphics device. Library behaviour such as
  text fitting and focus is taken as given there and not tested again.
- After a change to drawing, run `./tools/compare-backends.sh` (no display needed) and inspect the
  images it writes.
- `tools/check-ui-text-policy.py` is the lint for the text rules.
- The acceptance checks for the eye are in §5.5 of the implementation plan. Frames are inspected by
  someone other than whoever wrote the code.
- To prove a journey works for a user, hand over to the `ux-validation` skill.

Report what runs in `xuzz` now and what remains design only.
