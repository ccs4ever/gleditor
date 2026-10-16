# Independent source review: J25 branching and Walks identity

Reviewed read-only against current worktree source at HEAD
`3cb0634d6789383c77282126b02345104c77be21` and uncommitted changes. I did not edit product code or
run tests or the application. The owner reports UI 241/241 and Xuzz 107/107 passing; these are
owner-reported results. Native evidence remains partial, as noted below.

## Walks current place and selected preview

`apps/common/ui/walks.cpp` now receives current visit independently from the selected form cursor,
marks the current option and describes it as current, and identifies `Current visit N` separately
from `Selected visit N`. `apps/xuzz/xuzz_app.cpp` passes `linkContext.currentVisit()` separately.
`PreviewAndCurrentStayDistinctAcrossFormReopening` covers distinct current and selected visits,
reopening after preview, equality after entry, and the no-current case.

This addresses the Walks ambiguity in the UX contract: preview must not change reading place, and
the reader needs to identify the actual current visit. The native worker observed the baseline's
branching graph and Back-to-root state, where Walks showed branch choices but no current marker. The
reported `--click-label` limitation was an automation issue; keyboard selection was the alternative.
The candidate Walks marker still has no complete native journey result in the evidence available to
this review.

## Persisted branch restoration and exact pins

The new `PersistentSiblingRestorationKeepsBothPinsAndOriginAfterReordering` in
`tests/xuzz/link_navigation_test.cpp` closes and reloads a real activity store, restores two sibling
children after resolution ordering changes, and checks each child's target, both exact pins, current
visit, root origin and checkpoint, sibling rejection, no added visits, and unchanged source-store op
count. This closes the earlier test gap between persistence-only and in-memory branching coverage.
The owner reports it passing in Xuzz 107/107.

Source review confirms `restoreVisit()` restores the recorded visit context and resolves/remaps pins
without appending a visit; Forward chooses a child of current, and ReturnToOrigin selects the saved
origin/checkpoint. Activity and selection checkpoint payloads remain separate. The activity format
is versioned without a compatibility shim, consistent with the repository's R8/no-shim policy. The
persisted occurrence pins are saved observations used to recover exact places; query ordering is not
treated as identity.

## Saved-visit page readiness

The former host-path gap is addressed in the current diff. `LinkContext::execute()` resolves the
saved target for EnterVisit, Activity Back/Forward and ReturnToOrigin, validates its exact open
document or cell, then checks both endpoint anchors before dispatch. If a document passage is not
laid out, it prioritizes the exact offsets and records a pending command; current visit is not
changed until retry succeeds. `advanceFrame()` retries after page construction, with
generation/cancel handling. The existing no-link pending panel reports the saved state/store/range
and exposes Cancel; its regression also checks that no Inspector toggle is offered when there is no
selected link.

`SavedVisitRestorationWaitsForExactPageAnchors` exercises Back, Forward, EnterVisit and
ReturnToOrigin with an unlaid-out document, asserting pending state, unchanged current visit and
activity operations, cancellation, and successful exact restoration after pages are built. The test
fixture's `MemoryTextSource("origin destination", ...)` matches the stored text and range.
`SavedVisitWithoutALinkShowsPendingTargetAndAccessibleCancel` covers the no-link pending panel.
Owner-reported UI 241/241 includes these cases.

The stale-child preflight edge is addressed in the latest diff: `ActivityForward` only derives a
target when the found visit's parent equals `navigator.currentVisit()`. The page-readiness host test
also tries the current visit as a non-child while its anchors are missing and asserts immediate
`NoPreviousVisit`, no pending entry, and unchanged current visit. This prevents invalid commands
from scheduling page-priority work before the navigator rejects them.

## Evidence and limits

The thirteenth-increment native baseline establishes the Walks current/preview ambiguity and
branching behavior. The ordinal-restoration defect was established by the prior twelfth-increment
evidence, not this baseline. The reported candidate native `4db` five-visit graph did not naturally
exercise a pending-page case; only the owning document regression proves page-wait behavior. Final
metadata replay is underway, so I make no full candidate-native J25 claim. The owner reports final
app `8aac` stable with UI 241/241 and Xuzz 107/107 passing; these results were not independently run
for this review. No performance, all-backend or full J16 acceptance claim is made here.

The governing contracts are R8 in `design/store-slice-convergence.md`, the branching Walks journey
in `design/ux_workflow_real_work.md`, and endpoint/preview rules in
`design/ui_workflow_xuzz_navigation.md`. Back, Forward, preview and restoring saved visits must not
mint visits or mutate visited source documents. The read-only review did not independently execute
the reported test suites.
