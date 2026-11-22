# Xuzz UI design review rubric

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
  [`design/ux_workflow_real_work.md`](../../../../design/ux_workflow_real_work.md), including
  closing the program and coming back.

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
[`design/projects/view-reviews/04-aesthetics.md`](../../../../design/projects/view-reviews/04-aesthetics.md).

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
