# Independent UX and visual review

## Scope and verdict

I reviewed the native OpenGL captures and accessibility traces under
`build/validation-thirteenth/candidate-fivevisit/`, including the frozen candidate's two-process
walkthrough and the final-phase5 activity journey. The Walks current-versus-selected presentation is
clear and visually legible at 1440×1000. Preview leaves the current reader visit unchanged,
including after restart. The closed-version notice preserves and identifies the saved target while
keeping the link controls available. The final-phase5 run also demonstrates successful exact-target
recovery, separate Reference and Annotation actions, and branching Back/Forward behavior.

A preceding phase-3 retry attempt is inconclusive because its scripted key closed the Hypertime
modal instead of dispatching Enter. That is a harness limitation, not evidence of a product failure.
The final native run closes the modal through its visible control and clicks the link-panel Enter
control; it creates visit 6 from the exact saved b1 target.

## Walks: current visit versus preview selection

The strongest current/selected comparison is visible in
`candidate-fivevisit/raw/13-walks-preview5-current3.ppm` and its adjacent a11y trace in
`evidence/phase2.log`. The notice names “Current visit 3” before “Selected visit 5”; the visit 3
option carries a `(current)` marker while visit 5 remains selected for preview. The modal's concise
layout puts the status before the controls, so it remains understandable even if the longer notice
wraps. Text is readable at the captured size and the restrained dark panel matches the existing UI.
The selected row's long document title truncates in the closed combo box, but the notice provides
the selected visit number and page context; expanding the list exposes the current marker.

The activity snapshots establish that preview is non-navigating: visit count remains five at
`12-walks-current3`, `13-walks-preview5-current3`, and `14-current3-after-preview`. The
`18-current3-after-restart-preview` snapshot is also five. The source dump SHA-256 is identical
(`49a12edef0113d9b6f6b7a120c8f2f7cbf7dd1f6d865e8606dc7dfaac4a164d6`) across those checkpoints. This
is direct native evidence that choosing and previewing an older branch does not rewrite the source
document or move the current visit, and that the distinction survives restart.

The earlier `final-phase4` controls run is named as a current-6 check, but its own a11y trace says
“Current visit 3” and shows “Selected visit 2” in `01-walks-current6` and `02-preview5-current6`. I
do not count those filenames as evidence of a current-6 state. The actual final-phase5 evidence
below verifies current visit 6. Its Walks status line clearly reports “Current visit 6” while the
older reference/annotation target is selected.

## Closed exact target: status, Cancel, and retry

The native status frame `final-phase3/raw/03-enter-refused.ppm` is legible and specific: it says to
open the exact saved document or cell version and offers Cancel. The investigation panel continues
to show the link authority, left and right endpoint cursors, reading store, and exact state/range.
This makes the refusal actionable without losing the reader's link context. The status shares the
investigation area with the controls rather than obscuring the document. The saved target is not
substituted with a nearby version.

Checkpoints `00-after-restart-b1-closed`, `03-enter-refused`, `04-cancelled`, `06-reopened-b1`, and
`07-retry-enter-b1` retain five activity visits; source dump SHA-256 stays identical. The phase-3
trace at `06-reopened-b1` still has the Hypertime modal open with focus on “Close hypertime”; the
scripted Alt+Shift+Return closes that modal rather than activating Enter. Its blank later screenshot
and unchanged visit count therefore do not test a retry.

The final-phase5 run supplies the valid retry evidence. It starts with no modal overlay and invokes
the visible link-panel Enter control by label. The accessibility tree retains link 22 and both
endpoint cursors; the activity appends visit 6 with the exact `b1` state, increasing the count from
five to six. The immediate `01-after-explicit-enter.ppm` capture is black, so I do not use that
frame as visual proof. The subsequent Walks frame `02-walks-current.ppm` visibly reports current
visit 6, and `08-forward-third-child.ppm` shows the restored b1 page and link panel. The a11y trace
and activity checkpoint establish the explicit Enter action and visit append.

## Reference, annotation, and branch navigation

The visible Walks frames `final-phase5/raw/03-reference-visit5.ppm` and `04-annotate-visit4.ppm`
show status lines beginning “Current visit 6” while an older selected visit is targeted. The
reference action visibly reports “Visit reference saved” and marks visit 5 as referenced. The
annotation action reports “Annotation saved” with the exact text “J25 branch note café” on visit 4.
The accent character renders correctly. These short outcome messages fit above the controls and
retain the distinction between current and selected visits.

The activity snapshots record `reference1 5` and `annotation1 4` (with UTF-8 text) while the visit
count stays six. Reference and annotation are activity metadata, not reader visits. The Back action
returns to the parent; Forward opens a chooser with three saved destinations (visits 3, 4, and 6),
and choosing the third child returns to visit 6. These checkpoints stay at six visits, so the branch
walk is navigable without manufacturing extra visit records. The chosen destination frame
`final-phase5/raw/08-forward-third-child.ppm` shows the expected b1 page and original link panel.

Across all final-phase5 checkpoints, the source document dump retains SHA-256
`49a12edef0113d9b6f6b7a120c8f2f7cbf7dd1f6d865e8606dc7dfaac4a164d6`. Thus the exact retry, reference,
annotation, Back, and Forward journey changes only the private activity store.

## Limits

This review covers the supplied native OpenGL run at 1440×1000 and its emitted accessibility tree.
It does not validate other rendering backends, assistive technologies beyond that tree, publication
pin retention/enrollment behavior, or the native appearance of the no-link pending overlay.
