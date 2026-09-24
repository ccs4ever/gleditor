---
name: xuzz-link-navigation
description: >-
  Implement or review Xuzz navigation through many-to-many Xanadu links and ZigZag cell content.
  Use for link selection, endpoint browsing, activation, return, and document/cell transitions;
  use the xudu or ZigZag UI skills for unrelated layout work.
---

# Xuzz link navigation

Use this workflow when a task changes how a reader follows links in `xuzz`. The interaction contract
and acceptance criteria are in
[`design/ui_workflow_xuzz_navigation.md`](../../../design/ui_workflow_xuzz_navigation.md). Read that
document first, then check the current implementation before editing. Its proposed behavior is not
evidence that the behavior already exists.

## Agentic review and handoff

For a substantial navigation change, assign independent reviews when subagents are available. The
lead remains responsible for synthesis and implementation; reviewers do not edit overlapping files.

1. **Link semantics reviewer:** Trace `Link` and its left/right `PrimediaSpan` lists through the
   store and presentation code. Check that the change keeps one link identity, complete endsets,
   provenance, and bidirectional traversal. Flag any accidental pairing inferred from rendered
   strands.
1. **Interaction reviewer:** Walk the same scenario through pointer, sovereign keymap, and
   accessibility input. Check overlap disambiguation, selection versus entry, independent member
   cursors, Back, and pending or unavailable targets. Produce a short state-transition table and any
   ambiguous cases.
1. **Code and performance reviewer:** Locate current Xuzz composition, beam picks, bridge focus,
   cell visibility, and test seams. Estimate fanout from endpoint resolution and ribbon staging;
   identify work that may block the UI thread or grow with the Cartesian product of both endsets.

Exchange the findings before coding. The lead records the resolved command contract and a minimal
vertical slice to implement. A review disagreement is resolved against the stored link model and a
headless interaction test, not by choosing whichever rendered strand is easiest to access. For a
small, local fix, perform these checks directly without spawning agents.

## Implementation path

1. **Establish the baseline.** Inspect `apps/common/xanadu/ops.hpp`, `link_layout.cpp`,
   `link_views.hpp`, `apps/xudu/beams.cpp`, `bridge_coordinator.cpp`, the Xuzz setup in
   `apps/xudu/main.cpp`, and the ZigZag presentation surface. Read the relevant bridge and
   convergence sections linked from the design document. State what is wired today and what the task
   will add.
1. **Model one navigation session per selected link.** Keep the link authority and ID, both ordered
   endsets, independent left/right member and occurrence cursors, explicit active side, origin, and
   reversible history. Resolve each member to exact document or cell occurrences. Use the functional
   link and range APIs on `main` where they clarify the query; avoid widening disjoint members into
   a single highlighted extent.
1. **Implement one command boundary.** `Select link` pins context without moving focus.
   `Select member` changes only one side's cursor. `Cross link` changes the active side;
   `Enter endpoint` focuses the explicitly chosen occurrence. `Back` restores the saved location and
   link state. Feed beam picks, cell picks, keymap actions, and accessibility actions through this
   boundary. A beam with no unambiguous source member selects its link for disambiguation.
1. **Compose the document/cell transition.** Carry link ID, side, member, occurrence, and exact
   range through the Xuzz bridge. Materialize an out-of-neighborhood cell on demand, retain the
   source as a companion, and keep link context visible while either view scrolls or changes
   dimension. Distinguish loading, outside-radius, missing-version, and unavailable-remote states.
   Never fall back to another link or occurrence silently.
1. **Bound rendering and finish the input surfaces.** Keep the selected link's members individually
   pickable without requiring every left×right strand to be drawn. Expose one accessible link
   identity and meaningful side/member actions. Put bindings in `system://keymap` and
   user-adjustable presentation settings in the appropriate system xanadoc or slice.

Navigation is ephemeral under R8 of `design/store-slice-convergence.md`: browsing and camera
movement append no operations. Link direction comes from the selected side or hit target, never from
the caret's document alone. A document and a cell containing the same primedia may expose multiple
occurrences; preserve the reader's chosen occurrence. Keep network or content resolution off the UI
thread.

## Proof of completion

Exercise a 2×3 discontinuous link with overlapping links and repeated primedia. Cycle each side
independently, cross both ways, enter an exact document occurrence and an exact cell range, change a
ZigZag dimension, and use Back. The same commands through pointer, keymap, and accessibility must
produce the same link ID, member, occurrence, focus, and return state. Include distant-cell and
unavailable-target cases; the session must remain cancellable and must not invent an endpoint.

Run the affected headless tests using the repository's `make` environment, then
`make -j$(nproc) format-check` and `make -j$(nproc) lint`. For rendering changes, use the headless
backend comparison required by `AGENTS.md`. Measure large-endset selection and staging before
setting a performance threshold. Report what runs in Xuzz now and any remaining design-only stages
explicitly.
