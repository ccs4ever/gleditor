# Independent initial review: readable navigation landing

Reviewed the second checkpoint at `85a9f8ab`, using `xuzz-ui-design` and `ux-validation`. This
reviewer did not implement product code. This is a scoped UX and visual review of retained native
evidence and implementation seams, not a new journey pass.

## Findings from native frames

| Severity                       | Reader action                           | Observed result                                                                                                                                                                               | Required behavior                                                                                                                                    | Evidence                                                                                             |
| ------------------------------ | --------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------- |
| Blocking readable landing      | Enter the chosen left document endpoint | The left passage starts beyond the viewport; only its trailing words are visible. Both pages are shown at a readable size, but centering their combined widths sacrifices the chosen passage. | The complete chosen passage begins inside the reading viewport at the configured readable size. A companion is secondary when both pages cannot fit. | `../spatial-ui-second-increment/evidence/navigation/loaded-enter.png`                                |
| High orientation inconsistency | Enter the left document                 | The caret and link context identify the left member, while the selected tab remains Doc 3.                                                                                                    | Tab selection, caret document and reading focus agree after a semantic transition. Preview and Cross leave them unchanged.                           | Same frame and `loaded-session.log`                                                                  |
| High restoration inconsistency | Back to the immutable root              | The transcript reports default focus at caret 0, while Doc 3 remains selected. The lower-left overview is not a substitute for a coherent selected document.                                  | Back restores the existing root and selects its document without appending a visit. Origin continues to restore its distinct frozen checkpoint.      | `../spatial-ui-second-increment/evidence/navigation/loaded-back-second.png` and `loaded-session.log` |

The existing dark tab and link-panel styling remains coherent: compact spacing, restrained cyan
selection and muted metadata make the controls easy to distinguish. The white source pages give text
strong contrast. The landing composition is unfinished because the selected words fall off-screen
and large empty page surfaces dominate. Correct framing should spend the visual emphasis on readable
content rather than introducing new decoration.

## Bounded implementation recommendation

Keep this increment inside the existing camera and document-view architecture.
`Views::activateDocument` already synchronizes the switcher, establishes the caret document and
requests ordinary reading framing. `Views::focusSpanAt` selects an exact range but currently changes
only caret and camera XY; the Link entry callback calls it without activating the tab. Consolidate
semantic document focus there or in a shared Views entry method, preserving selection rather than
resetting it to byte zero.

The callback in `xuzz_app.cpp` currently aligns a companion, fits the union of two page widths, then
caps the distance using `readableCameraDistance`. Once that cap prevents the union from fitting, the
union center may put the focused passage off-screen. Reuse `frameForReading`, page frames, range
anchors and `placeCameraWhenReady` to choose a camera that keeps the focused range inside the
available reading area. Obtain geometry after layout readiness; preserve the chosen page's pose.
Keep a source companion where the available width permits it, and never reduce text below the
configured readable threshold to force the complete pair onto the screen.

This is camera/view state, not source data. Camera work and tab synchronization must add no source
operations or completed visits. Enter records exactly one successful visit; Back and Origin restore
existing visits. Do not infer another occurrence, replace an exact version with latest, or introduce
a new view host, placed-plane implementation or screen-space duplicate of the source page. The
render-region/page-pose plans remain gated and are not prerequisites for correcting this existing
native view.

## Final review gates

Inspect fresh native frames after pointer Enter, default-key Enter, Origin and Back. Check exact
caret/range, selected tab, active side, link authority and visit identity in the matching tree and
operation dumps. Compare sources before and after in the same profile. Repeated entry from the root
must retain sibling visits. Preview, Cross and inspection must leave the reader in place.

Include a narrow viewport and enlarged UI scale; a passage must remain readable when the companion
cannot be shown in full. Review each compiled backend. Verify pending geometry and unavailable exact
targets do not falsely signal arrival. Retain the existing AT-SPI limitation explicitly: internal
tree dumps do not establish a platform accessibility-session pass. Cell/historical targets, complete
J16/J24/J25 and full restart restoration need their own evidence and remain outside this initial
review's acceptance.
