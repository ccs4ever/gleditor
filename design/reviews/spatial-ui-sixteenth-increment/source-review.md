# Source review: immutable edition switch

Read-only review of the uncommitted implementation in worktree HEAD `49b82077`. No product edits or
tests were made; the owning build was in progress.

## Result

I found no blocker in the reviewed invariants. The implementation performs the requested two-carrier
preference switch atomically at the preference-file boundary and keeps the on-disk format at version
2\. `ReaderLinkPackages::switchEnabled()` verifies both exact cached payloads, matching curator and
salt, distinct sequence, retained state for both carriers, and that the chosen baseline is still
enabled. It mutates a copy of the complete preference map, changes only `from.enabled` and
`to.enabled`, then calls the existing atomic `save()` path. Retention and emphasis are untouched. A
failed write leaves in-memory choices unchanged; the added test also checks persisted bytes and a
symlink target.

The session wrapper validates candidate phase eligibility, exact signed package bytes/hash through
`cacheVerified()`, and open-store authority collisions before switching. The review predicate
accepts Ready/Published payloads plus only outgoing (`!received`) Superseded/NeedsVerification
payloads. This allows an exact older outgoing carrier to be re-cached and switched back after
supersession; a received Superseded package is not eligible. Candidate retention is explicit: the
comparison first offers “Retain candidate without enabling,” and the switch requires both exact
carriers already retained. No preference is copied to a new hash.

The comparison refresh action carries the selected baseline hash and re-reads exchange status.
Before mutation, it checks the refreshed candidate hash against the captured candidate and checks
the selected baseline still matches the displayed hash; the model then revalidates baseline
enabled/retained and both payloads. A Link selected from the prior carrier remains bound to its
original `LinkKey`; `packageVisibilityChanged()` preserves it as disabled and the UI test checks the
original 2-left/3-right endsets, visit and activity/source operation counts. The reverse switch
through the old Superseded status demonstrates the intended explicit revert. The unrelated enabled
carrier remains unchanged in the model regression.

No store or preference format bump is present. `ReaderLinkPackages` still reads/writes format 2.

## Remaining limits

The added unit coverage is useful but the comparison form itself has no direct form/accessibility
regression in this diff. The model and navigation tests establish switch invariants, but a focused
view test should still exercise that the displayed baseline choice and exact candidate hashes
survive selection/refresh and that a stale candidate cannot trigger a different switch. This is a
coverage gap, not a source correctness failure found in the current callback: the callback re-reads
and checks exact hashes before mutation.

The trust and citation availability work remains open. The comparison explicitly says enrollment is
unchecked and that referenced publication bytes are checked separately when opened. It makes no
network or remote-availability claim, which is appropriate. The current product still needs an
explicit user-visible citation acquisition result once the exact pinned publication is opened; this
implementation does not supply that result.

`git diff --check` was clean at review time. I did not run or wait for tests because the owning
build was already in progress.

The final compact-form source audit confirms the answer indices: `0` is the action control, `1` is
Comparison status, and `2` is the retained-edition baseline selector. The callback reads
`answers[2]` for refresh and switch decisions, so the inserted status field does not shift baseline
handling incorrectly. Selecting a different baseline before switching reopens that selected pair
with a visible refusal notice; candidate identity remains guarded by the captured and refreshed
immutable hashes.
