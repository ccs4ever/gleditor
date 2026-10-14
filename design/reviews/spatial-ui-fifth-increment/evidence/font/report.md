# UI font scale affordance probe

**Finding: blocked.** `system://ui` tells readers “Edit ui.scale or ui.fontScale” and describes
`ui.fontScale` as a positive typography multiplier defaulting to 1. Opening this system document
through Ctrl+O works. The value cell is not available as a visible actionable control in the native
UI state: selecting `Cell #144: 1 [number] [value kind: double]` through the normal accessible-click
path failed with `expected one visible actionable control ... found 0`. The bounded keyboard probe
stepped to blank structural cells #23 and #35 and did not reach the numeric setting.

The keymap’s E action is documented as “edit the focused cell’s text.” In code, `beginCellEdit()`
seeds its buffer from `inspectCell(...).text`; Enter calls `updateFocusCellText()`, which calls
`engine_->updateCellText()`. That path changes cell text, while the font scale is a typed scalar
setting. No visible scalar-value editor or pointer/a11y action was found. E on the open document’s
prose focus did not open a scalar editor; it was cancelled without typing. This bounded probe did
not reach enlarged chrome text through the guidance in the system document. It does not establish
that every possible graph-navigation route is unavailable.

The narrow prerequisite is a typed setting-value editing command for visible `d.values` cells,
routed to the system setting model’s typed update path (including finite-positive validation for
`ui.fontScale`/`ui.scale`), exposed through the same pointer, keymap and accessibility command
boundary. Then reopen the same isolated profile to verify the persisted value and resulting scale.
This probe did not edit the product, setting, or visited source.

## Evidence

- [system document frame](open-ui.png) shows the settings guidance and defaults.
- [focus after Alt+Home](ui-focus-home.png), [first X step](ui-focus-x1.png), and
  [second X step](ui-focus-x2.png) show the native keyboard focus at #6, #23 and #35; the latter two
  are blank structural cells.
- [E key frame](text-editor.png) records the prose-focus state after invoking E; the run then
  cancelled without typing.
- [native interaction transcript](open-ui.log) includes the system document open and its internal
  accessibility dump. [focus transcript](focus-navigation.log) records the movement run.
  [failed value action](affordance.log) preserves the exact `--click-label` failure.
- [automation arguments and runtime profile](argv.json) records the actual inputs, build version and
  isolated profiles. [source operation status](source-ops.json) records that no setting mutation was
  made; there is no before/after setting delta to claim.

Raw PPM captures and runner scripts are kept under `/workspace/spatial-build/fifthscratch/`, outside
Git. The PNGs here were converted losslessly from those PPMs.
