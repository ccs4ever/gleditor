# Thirteenth spatial UI checkpoint

Based on `3cb0634d`, this increment separates the current reader visit from the Walks preview and
makes saved document restoration wait for its exact page anchors. It continues J25 through the
existing activity, panel and document-entry seams. It does not complete the overhaul.

## Implemented behavior

Walks receives the current visit independently from its selected row. The notice starts with
`Current visit N` (or `No current visit`), and the current row carries a textual marker and an
accessible description. Selecting or previewing another row does not redefine the current visit. The
app captures both identities each time the existing form opens; no action or setting is added.

Back, Forward, Enter Visit and Origin now use the same exact document-page readiness path as Enter.
The host preflights the saved target and prioritizes its start and last selected byte before
navigator dispatch. Until both anchors exist, the current activity pointer, focus and completed
visit count stay unchanged. Cancel abandons the pending command; view-generation changes and source
arrivals retain the existing cancellation rules. A stale/non-child Forward is refused before any
page wait. Restoring an existing visit never appends another visit.

A pending restore can exist without a selected Link. The existing retained panel then shows
`Saved visit restoration`, full store authority, saved state/range, the pending reason and Cancel.
It has no Link Inspector toggle. An unavailable restore also exposes its existing status and Cancel
without requiring a selected Link. This extends the existing presentation instead of creating a
second navigation surface. C++ is needed here for typed host preflight, page-geometry readiness,
activity dispatch and the retained panel; no new sovereign action is introduced.

## Owning checks

The new real-Doc regression fails before the fix: Activity Back switches immediately while its page
anchors are absent. The final regression exercises Back, Forward, Enter Visit and Origin with an
unbuilt document, cancellation, page construction and retry. It also rejects a stale Forward before
waiting. An accessible-panel regression checks the no-Link pending target and Cancel, unchanged
current/visit/source operations, and hidden accessible chrome after cancellation.

The persistent sibling regression authors two children with distinct left/right pins, closes and
reloads the real activity store, reverses occurrence resolution order, and restores each child. It
checks exact focus, both pins, frozen Origin, retained topology, no sibling-as-child transition, and
unchanged completed visits and source operations. The Walks form regression independently checks
current versus selected preview across reopening and a changed current visit.

## Native evidence and limits

`native-branching/` retains actual frames, commands, six-variable headless profiles, frozen
executable hashes, source/activity snapshots and decoded payloads. The intermediate `4dbb` candidate
proves the five-visit graph `1→2→3` and `1→2→4→5`, an empty Forward at leaf 3, each child chosen
from parent 2, and preview 5 while current 3 stays fixed before and after restart.

The build5 reopening phase preserves exact closed `b1` and refuses Enter, then Cancel clears its
status. Its supposed retry was not endpoint entry: Hypertime still held focus, and Return closed
that modal. The following phase's filenames also overstate current visit 6; decoded activity shows
five visits, with current 3 then 2. These historical labels are explicitly excluded from successful
retry and Forward claims. They are retained as harness diagnostics.

The final `8aac` executable uses visible Enter once `b1` is open and no modal is active. Visit count
changes from 5 to 6, parent 2, exact `b1` bytes 0–20. Reference 5 and UTF-8 annotation 4 preserve
current 6 and six visits. Back restores 2; the Forward chooser retains children 3, 4 and 6, and
choosing 6 restores it without another visit. The immediate post-Enter image is black and is not
visual proof; the later restored `b1` frame, accessibility state and decoded activity establish the
bounded result. A final same-profile restart retains reference 5 and annotation 4 before preview,
current 6, and all three forward children. Its four checkpoints stay at six visits. The authored
source dump remains `49a12edef0113d9b6f6b7a120c8f2f7cbf7dd1f6d865e8606dc7dfaac4a164d6` throughout
all candidate snapshots. Reading-place metadata can grow separately from completed visits.

Final fresh full suites pass UI 241/241 and Xuzz 107/107. The canonical GNU Make build and strict
core comparison pass. Unchanged library/shared-library hashes support reuse of library 799/799 and
owning cache/mip 53/53 evidence. Independent source and UX/visual reports distinguish actual native
proof from the owning page-readiness tests.

The final check inventory records build, complete owning suites, format/lint/text-policy gates,
strict GL/GLES/Vulkan results and verified unchanged library evidence. The small renderer sample
compares configured one/four-thread recording but does not exercise actual split recording. Broad
E2E and optimized performance are not accepted by these checks. Native AT-SPI and naturally timed
pending-page interruption remain open; internal accessibility actions and owning page-readiness
tests are different evidence.

The independent publication/pin-lifecycle audit is retained here before D2 work. Immutable carrier
identity and signature/payload checks exist, but the received workflow does not establish
enrollment. Retain, enable and emphasis do not yet have separate complete reader controls; disabling
a selected package currently dismisses its Link. These are concrete next implementation
requirements, not unavailable services to simulate. Full J24/J25, media-position restoration, exact
cell/history variants, J1–J16 preservation and J17–J32 acceptance remain open.
