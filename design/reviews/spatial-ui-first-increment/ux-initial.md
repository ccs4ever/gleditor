# Independent UX review: first native spatial increment

Baseline: `e9726a6b3825e794ab86db6cd54f18d9258e75c0`. Review date: 2026-10-13.

This is an independent design and code-seam review by an agent that edits no product code. Native
frames and interaction transcripts have not yet been supplied, so this review establishes gates, not
journey passes. Sources: the Xuzz UI design and UX validation skills, the spatial handoff, prototype
contracts, navigation workflow, J22/J24/J25, and existing `LinkNavigator`, `LinkContext`,
`StoreActivityLog`, `link_panel.cpp` and `LinkPanelOverlay`.

## Scope decision

Proceed with D1 exact content references and an opt-in quiet/expanded investigation treatment on the
existing retained Link panel. Preserve ordered source-span/range pairs, exact occurrence identity,
both complete endsets and independent cursors. Add individually reachable member and occurrence
controls through existing paging and `NavigationCommand`. Show exact authority and version in the
expanded inspector. Reuse existing Enter, Activity Back and Return to origin.

This scope extends actual navigation and activity services. It does not justify claiming the whole
investigation workspace, physical planes/wings, acquisition, search, package policy, media position
restoration or durable collections are implemented. Keep the old navigation path available. L6/L7
and U4a remain prerequisite gates for the later physical treatment; this increment needs no new
camera, host or render architecture.

## Findings and required design resolutions

| Severity | What the user tries                     | Baseline or design risk                                                                                                                                       | Required behavior                                                                                                                                                           |
| -------- | --------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Blocking | Select a Link while reading elsewhere   | `LinkContext::execute()` calls `noteOrigin()` for a new selection; that can append an Opened visit if the current caret differs from the last recorded target | Selecting must append no completed visit. Establish the origin from an existing completed visit or an explicit reader checkpoint; demonstrate before/after activity records |
| Blocking | Inspect an exact historical endpoint    | Short site labels identify a file/page or cell but omit authority and version                                                                                 | Expanded metadata must identify authority/store, exact state, source ranges and occurrence separately; never silently substitute latest state                               |
| Blocking | Browse one side of a many-to-many Link  | Compact controls could leave complete membership implicit or make a strand look like an authored pair                                                         | Every stored member and each occurrence must be individually reachable in a synchronized list; retain one Link identity and both complete counts                            |
| Major    | Expand tools around the current passage | A larger inspector or dock could cover prose or move the reading place                                                                                        | Retain document/cell focus, caret/range, reading offset, current visit and page bounds; use protected-content placement and readable paging                                 |
| Major    | Choose an occurrence, then travel       | An occurrence row's default activation could conceal entry semantics                                                                                          | Label selection/preview and Enter separately. A combined entry gesture must visibly say Enter. Cross must preview only                                                      |
| Major    | Fold and reopen the inspector           | Fold might reset cursors, dismiss the Link or record navigation                                                                                               | Fold changes presentation only; preserve exact selection, origin, both cursors and reading position                                                                         |
| Major    | Reach an unavailable target             | A missing engine service could be represented as successful arrival                                                                                           | Preserve target ordinal/identity and full counts, show an honest state, offer Cancel when pending, append no visit on failure                                               |
| Major    | Operate by keyboard or accessibility    | A new visible toggle or list might have only pointer wiring                                                                                                   | Register a default sovereign keymap/Vortex action, visible affordance and accessible name; all paths use the same command boundary                                          |

The metadata finding does not mean ranges are absent everywhere: existing `sideLine()` exposes byte
ranges. The gap is complete exact identity at the inspected endpoint, especially when filenames or
version labels repeat across authorities.

## State and affordance requirements

- Quiet mode retains a visible, named Investigation control. Expanded mode labels the selected Link,
  active side, remembered cursors, current visit and origin. Current reading and preview use words
  as well as visual emphasis.
- Empty selection explains how to choose a Link. Pending resolution and pending entry remain
  distinct from an unresolved member, missing historical state or unsupported capability.
- Unavailable members remain in their ordered list; hidden drawing never removes stored membership.
  Package-disabled but pinned context must be reported as a later gap if this increment lacks it.
- Dense and narrow modes retain a synchronized paged list. Pagination changes neither cursor nor
  visit. Expanded identity text wraps without sacrificing ordinal, selected state or control names.
- Reduced motion uses cuts and accepts input immediately. Enlarged text keeps controls reachable and
  readable. Preview/fold and camera effects append no source operations.
- Back and Return remain explicit reader-activity controls, distinguishable from document
  microversion traversal. A sibling remains reachable through its parent, never through an invented
  forward edge.

## Native evidence required before acceptance

1. Capture quiet, expanded and refolded states with the same exact target, current visit, both
   cursors, origin and reading offset. Compare passage bounds and inspect accessibility dumps.
1. Use an actual 2-by-3 Link with disjoint spans and a repeated occurrence. Browse left and right
   independently, choose an exact occurrence, Cross, and show full counts and preserved opposite
   cursor. No gap may become highlighted content.
1. Enter the chosen resolved occurrence and prove exactly one completed visit. Back, Return and
   Resume must restore existing visits without another completed visit. Continue from an old visit
   and show retained sibling futures where included in this increment.
1. Attempt an unavailable endpoint and cancel a pending entry where supported. Show unchanged target
   identity, full endset counts, reading focus and completed-visit count.
1. Repeat relevant actions by pointer, recorded default keymap chords and accessibility input;
   compare Link ID, active side, member, occurrence, focus and visit identity. A diagnostic command
   without a real control or binding is no affordance.
1. Capture narrow, enlarged-text and reduced-motion states; inspect for body occlusion, clipped
   identity, lost focus, unreachable list items and inconsistent current/preview labels.
1. Retain before/after visited-store and permascroll evidence and reader activity evidence. Draw
   changes also require backend comparison images and independent frame inspection.

Report coverage by implemented step, not by whole-journey shorthand. J22 search/comparison/layer and
pouch work, J24 package-disabled context and remote/locked alternatives, and J25 annotations/media
position/reopen cases remain unpassed until their actual services and interfaces are exercised.
Existing J16 input parity and exact navigation, J5 restart behavior and the remaining J1-J16
preservation checks remain required; this initial review supplies no regression pass.

## Initial disposition

The proposed increment is feasible and appropriately bounded. Resolve the selection/activity issue
and exact inspector identity before native validation. Final UX acceptance remains pending actual
application frames, dumps and independent journey transcripts.
