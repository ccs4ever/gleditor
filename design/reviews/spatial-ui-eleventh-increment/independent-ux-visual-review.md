# Independent UX and visual review: exact closed-target recovery

## Scope and verdict

The candidate’s minimal fallback is a good fit for the recovery case. When resolving the chosen
occurrence fails, the Inspector now exposes that occurrence’s stored destination identity (store,
version, and byte range; cell identity for a `CellSite`) before preserving the existing exact-source
reference error. It does not fabricate a source or birth mapping. The successful Inspector path
remains unchanged. I found no blocking product concern in the source diff or native same-session
recovery evidence.

The review is bounded to the occurrence identity fallback, generated presentation identities, the
new UI regression test, and the native recovery journey. It does not claim that every source detail
fits on one Inspector page; the Inspector is paged. The separate native page walk reaches and
captures the fallback descriptor and error together on page 3, as reviewed below.

## Source and test review

`appendInvestigation()` checks the selected occurrence index before reading the `OccurrenceSite`. On
a failed `ContentReference`, it emits the saved site identity and then the existing error. This
keeps endpoint destination identity distinct from source/provenance identity, which can be
unavailable while the destination frame is closed or mismatched. The unchanged success path avoids
adding duplicate rows when the reference resolves.

The presentation change computes the background `WidgetId` after the IDs already assigned to
controls, pinned labels, and body blocks, instead of using the previous fixed ID. The new test
`ClosedOccurrenceKeepsExactIdentityUntilItsVersionIsReopened` verifies the closed and wrong-version
states, verifies exact-version reopening restores the pinned state and allows Enter, and checks that
failed attempts do not focus or append visits and successful reading does not mutate the source
store. It exercises the presentation/a11y seam and model callbacks; native evidence below supplies
the real input journey.

## Native journey evidence

The baseline same-session run archived at `native-recovery/baseline-full3/` reached the closed
target, opened the hypertime map, activated b1, and recovered the exact left reference without
restarting the application. The selected link/occurrence remained Left 1/2, place 2/2 and Right 2/3.
The source ops dump hash was identical at all eight checkpoints:
`49a12edef0113d9b6f6b7a120c8f2f7cbf7dd1f6d865e8606dc7dfaac4a164d6`. After retry Enter, the activity
state advanced to visit 3. The appended `visit2` text is byte-equal to an earlier visit payload: the
token after `visit2` is the parent ID. They are distinct sibling visits from the restored root, with
separate appended visit cells and current-visit values.

The candidate core run archived at `native-recovery/candidate-refusal-retry/` used binary SHA-256
`6aade8d34a679960b3789f87cc808219dcb2d01aa2ab0602b68a14907a8d7522` for `xuzz` both before and after
the run and exited 0. Its nine native checkpoints cover selection, inspection, first Enter, return
to origin, close, Enter/refusal while closed, map activation, same-session reopen, and retry. In the
closed checkpoint the accessibility trace reports Left occurrence store
`local:dd9091a75803e93f7c5486c386db25f3`, saved state `b1`, bytes `0–20`, followed by
`Occurrence does not match the chosen document or cell frame`. Enter while closed leaves the current
reader visit at 1 and appends no activity visit. After map activation of b1, the Inspector again
shows the same store and pinned b1; retry Enter reaches current reader visit 3. The source ops hash
is identical at every checkpoint
(`49a12edef0113d9b6f6b7a120c8f2f7cbf7dd1f6d865e8606dc7dfaac4a164d6`). The activity snapshots show no
completed visit at the closed refusal, then four append-only operations for the completed retry
visit and current-visit update. These checks distinguish selected endpoint/cursor identity from
completed reader visits.

## Visual review and remaining bound

At 1440×1000, the candidate’s close frame
(`native-recovery/candidate-refusal-retry/frames/05-close-b1.png`) clearly shows the preserved Left
1/2 · place 2/2 selection, Left bytes 0–20, “a closed version,” and link authority. The native
closed Enter frame (`native-recovery/candidate-refusal-retry/frames/06-unavailable-closed.png`)
records the refused transition and no visit advance. The recovery frame
(`native-recovery/candidate-refusal-retry/frames/08-reopened-b1.png`) presents b1 again, and the
retry frame (`native-recovery/candidate-refusal-retry/frames/09-retry-after-map.png`) shows the new
reading state and visit 3. The archived PNG frames are lossless decodes of the full-resolution
1440×1000 PPM captures, verified against the raw RGB pixels; hashes and checkpoint evidence are in
the candidate archive.

The complete six-page native page-walk, archived at
`native-recovery/candidate-inspector-pagewalk/frames/07-closed-page3.png` shows the fallback rows on
page 3/6. At 1440×1000, the exact occurrence store, `State: b1 · bytes 0–20`, and full WrongFrame
explanation fit on the page without truncation or overlap. The labels are distinct from the selected
cursor summary above them and the source-reference error follows the saved destination identity, so
the reader can see both what can be recovered and why the exact reference is currently unavailable.
The native trace records current visit 1 and source/activity snapshots remain unchanged across page
turns. This closes the visual legibility check at the captured size; narrower-window behavior
remains unmeasured.

The runs used the 1440×1000 Xuzz frame. Narrower-window behavior was not measured. The map
activation and successful exact retry are demonstrated; no claim is made about a separate
unavailable-back-tail recovery path.
