# Sixteenth spatial UI increment: independent UX design review

Read-only design review for the next package lifecycle increment at `49b82077`. I read the Xuzz UI
design and UX validation skills, the J23/J24 contract in `design/ux_workflow_spatial_concepts.md`,
the current `Review independent links` form, and the fifteenth increment's native OpenGL frames
09–12. I made no product edits and ran no native session.

## Recommendation

Extend the existing review form with one explicit same-curator-and-salt comparison path. Keep the
currently reviewed carrier as the primary identity and preserve its title, sequence, full
`InfoHash`, signature result, enrollment result, Link ordinal, and both ordered endsets in the form.
Offer a named action such as **Compare later package…** only when a later verified carrier for the
same curator key and salt is available. The comparison view should show the two immutable carriers
side by side by sequence, title, and full hash, with their signature and enrollment states. It must
not merge their Link IDs or preferences.

From that comparison, **Switch review to this carrier** explicitly opens the newer carrier's own
review form. This changes which package the reader is inspecting; it does not change the selected
navigation Link, the old carrier's retained/enabled/emphasis preferences, or the new carrier's
preferences. A separately labelled **Enable this carrier** action is the only transition that allows
its links into drawing and picking. It is available only after that exact carrier is retained and
its payload and signature are verified. Retaining the candidate never enables it. Closing or backing
out of comparison returns to the original carrier and original Link selection.

This is the smallest extension to the form's current action menu and identity rows. It avoids a new
package-management surface and keeps the reader's current Link context visible while reviewing an
update. A list row can abbreviate a hash, but the comparison and review forms should expose the
complete hash in the visible identity text and accessible name.

## State semantics

Treat carrier identity, payload availability, signature verification, enrollment, and reader
preferences as separate facts. Do not compress them into a single “trusted” or “available” state.

| Fact       | States and consequence                                                                                                                                                                                             |
| ---------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Carrier    | Curator key + salt + sequence + title + full immutable `InfoHash`. Two sequences are two carriers even when title and curator match.                                                                               |
| Payload    | Verified bytes present; fetching/verifying; or exact payload missing. Missing bytes keep a known carrier inspectable as a pin but prevent signature verification and enabling. Never substitute a nearby sequence. |
| Signature  | Verified for this carrier; invalid; or not verifiable because exact bytes are missing/unavailable. Invalid means refuse enable and explain the cryptographic failure.                                              |
| Enrollment | Enrolled; explicitly unchecked/not enrolled; or unknown/not evaluated. Unknown is not equivalent to unchecked, and neither is equivalent to invalid signature. Enrollment does not change the signature result.    |
| Retention  | Retained or not retained, independently keyed by full carrier hash. Inspecting, comparing, and switching the review target do not change it.                                                                       |
| Drawing    | Enabled only for this exact retained carrier after exact payload and valid signature checks. Otherwise disabled.                                                                                                   |
| Emphasis   | Normal or Quiet, remembered per carrier and meaningful only to presentation. It never changes authored prominence or relation identity.                                                                            |

Use direct status language, for example: “Payload missing; signature cannot be checked; enrollment
unknown; not retained; disabled.” For a bad signature say “Signature invalid” and name the failed
verification. For an intact but unchecked signer say “Signature verified; curator not enrolled.”
These descriptions prevent a missing blob from looking like a bad signature and prevent absent trust
data from being mistaken for cryptographic failure.

## Interaction sequence

1. The reader opens the existing **Review independent links** form for package #12. Its title and
   sequence, curator key, salt, full carrier hash, verification/enrollment state, selected Link ID,
   and Left/Right endsets remain the primary context.
1. **Compare later package…** lists only later sequences with the same curator key and salt. Each
   row identifies sequence, title, and abbreviated hash; opening a row reveals the full hash and
   independent status fields. This is inspection only.
1. The comparison keeps #12 visible beside #13. It describes what differs (sequence, title, full
   carrier hash, payload/signature/enrollment state) and says plainly that reader preferences and
   Link identity belong to each carrier separately.
1. **Switch review to this carrier** opens #13's existing review form. The action does not select a
   Link for navigation and does not enable drawing. If #13 is not retained, the form says so and
   offers **Retain in reader** as a distinct action. After retention, **Enable reader layer**
   remains an additional explicit action.
1. Returning to #12 restores the same form selection and navigation context. Enabling #13 leaves
   #12's retained preference intact and does not retarget an already selected #12 Link. A user who
   wants to navigate through #13 must explicitly select one of its Links from the #13 form.
1. Close/reopen restores each carrier's own preferences and exact carrier identity. Review target
   alone need not persist unless the existing form already records it; it must not be inferred from
   the latest sequence.

Pointer, keyboard, and accessibility activation should invoke the same review/compare/switch and
preference actions. A cancelled comparison has no side effects. Candidate rows and actions need
accessible names that contain enough identity to distinguish same-title sequences, including the
sequence and a hash prefix; full hash remains discoverable from the detail row. Focus returns to the
invoking action when comparison closes.

## J23 and J24 boundaries

For J23, comparison must leave the initial and final counts attributable to explicit per-carrier
choices. Merely inspecting #13, comparing it with #12, or switching the review form to #13 adds no
retained or enabled links. Retaining #13 adds its links to retained inventory only. Enabling it is a
separate step. Quiet remains list-discoverable, and counts name whether they count packages or
links. Restart restores the actual choices per full carrier hash; the fixture's initial 22 enabled /
85 retained count is not presented as a live invariant.

For J24, the currently pinned Link keeps its exact carrier hash, Link ordinal, complete ordered
endsets, independent member/occurrence cursors, active side, and origin while package comparison is
open. Inspecting, retaining, or switching review target does not enter an endpoint or append a
visit. Disabling or removing the carrier keeps the pinned inspector available with the established
disabled context and explicit dismissal/re-enable options. No candidate action may silently replace
that context with the same ordinal from a newer package.

Package preference, review and comparison transitions are reader-local. They must leave visited
source stores untouched and append no completed visit to `system://activity`; only explicit
successful endpoint entry can record one.

## Early visual baseline

I inspected the actual frozen native GL frames, 1440×1000, from the fifteenth increment:

- `09-package-review.png` shows the centered slate form, one clear title, muted status copy, a blue
  focused Action field, and aligned identity/endset labels. The carrier hash wraps legibly across
  two lines. The status line is information-dense but does not collide with the action control.
- `10-retained.png` and `11-enabled.png` keep the same layout and change only the status phrase from
  disabled/unretained to retained, then enabled/retained. `12-quiet.png` adds the quiet state
  without moving the hierarchy. This stable form is a good comparison baseline.
- The comparison should reuse this panel width, label/value alignment, slate surface, title role,
  muted explanatory text, and blue focus treatment. Keep the existing Link identity and both endset
  rows visible in the primary review form; comparison may be a short-lived view within the same form
  flow rather than a new overlay layered over it.
- The form already carries enough text that a comparison should not add a second long paragraph to
  the status line. Put each carrier's status on its own compact row and preserve the full hash in
  detail. Selection/focus should be the only strong blue emphasis. Use no new trust colour: status
  words and a secondary cue should communicate state without borrowing link or authorship hues.

These captures show that the existing visual shell can host a concise comparison, but they do not
show same-salt competing carriers, an actual comparison state, missing payload, invalid signature,
unknown enrollment, keyboard-only use, or native accessibility. Those states need native frames and
accessibility evidence before visual or J23/J24 acceptance.

## Findings and scope

The current UI already has an appropriate home: `Views::reviewIndependentLinks()` in
`apps/common/ui/xanadoc/views_publication_links.cpp`. It binds one exchange status to one
`InfoHash`, lists the selected Link and endsets, and offers explicit Retain, Enable/Disable, and
Normal/Quiet actions. Its status currently states “signature verified; enrollment unchecked.” The
next increment should add comparison and explicit review switching at this form boundary, without
changing the existing package lifecycle controls.

The principal risk is ambiguity between “switch review target” and “switch active navigation
carrier.” Use distinct action labels and keep the current Link identity visible until the reader
explicitly selects a Link from the new carrier. Missing payload, invalid signature, and unknown
enrollment need independent status presentation and enable gating. These are design criteria, not
claims that the native app currently supports those states or that J23/J24 has passed.
