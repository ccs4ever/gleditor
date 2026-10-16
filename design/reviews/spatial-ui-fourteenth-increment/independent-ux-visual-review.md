# Independent native UX review: disabled pinned package

Read-only visual and interaction review of the local-only package lifecycle evidence under
`build/validation-fourteenth/native/`. The native run writes no network announcement. The workflow
uses a real local signing key, authored a 2×3 native Link, signed and sealed the local document,
prepared a canonical signed package, then enabled, selected, and disabled the package. This review
does not treat the local package as proof of network publication or enrollment; the review form
reports the signature as verified and enrollment unchecked.

## Reader-visible result

The reader can distinguish the pinned package from its source document. Frame
`09-independent-package-cursors.ppm` shows the selected package copy as Link 0 in carrier
`da9e373b1f09392112a9e2f36a339defd68168bf`, with Left 2/2 and Right 3/3. The disabled state in
`raw/13-disabled-inspector.ppm` and `raw/14-disabled-enter-no-visit.ppm` retains that identity,
carrier, both cursor positions, the current reading and return checkpoint, and the notice “Package
drawing disabled · pinned Link retained.” The page also preserves the package's two occurrence
stores and byte ranges instead of substituting a different Link.

The disabled geometry has a legitimate visual control in this fixture: the document contains its own
UI-authored native Link 22, coincident with independent package Link 0. Before disable, the
accessibility tree contains two same-label nodes for “comment link, 2 left and 3 right members,”
with the package node selected. After disable, it contains one unselected node.
`LinkBeams::describe` publishes nodes from current strands, and marks a node selected only when its
resolved key equals the pinned key. The native source operation dump confirms the authored source
Link remains. Thus, the cyan ribbons that remain visible in frames 12–14 are the separate native
Link, not evidence that disabled package Link 0 is still drawn. The package and native Links share a
label and geometry, so their visual overlap alone is ambiguous; the accessibility node count,
selection state, and distinct package render identity provide the differentiating evidence.

## Findings

- **Pinned inspection is clear and stable.** The disabled panel keeps the exact package carrier and
  authority, both non-default cursors, and the disabled state in view. The screenshot and
  accessibility label agree. The action “Re-enable package” is available through the panel/keymap.
- **The final build resolves exact signed source addresses.** Frame 16 displays each signed scroll
  identity and global source byte range beside its local occurrence store and byte range. That makes
  the authority mapping inspectable; the frame does not show source excerpts, so it should not imply
  those bytes have been rendered as text.
- **Disabled Enter now explains its state.** Frame 15 labels the control “Enter unavailable,” and
  the panel says “Package drawing disabled · re-enable to enter; pinned Link retained.” The native
  accessibility tree emits that label; the final implementation marks it non-focusable and omits its
  action. “Re-enable package” remains visible. This replaces frame 14's ambiguous ordinary “Enter”
  label.
- **Dismiss clears the pin, though its compact label is cryptic.** The compact control is rendered
  as “×”; the expanded investigation names it “Dismiss selected link.” Frame 27 confirms the compact
  control dismisses the selection. Prefer the action name in its accessible label and expose it in a
  tooltip or nearby text so “×” is not mistaken for closing the panel alone.
- **The investigation panel is information-dense.** Long carrier and store IDs, range metadata, the
  repeated disabled notice, and the source-address explanation make the expanded panel tall. At
  1440×1000 the essential status and cursor identity remain legible, but the controls require paging
  and the lower edge is close to the window boundary. Keep the concise disabled notice and both
  cursor summaries ahead of full identifiers; retain full values in the accessible details.

## Bounded evidence

The run's raw frames are 1440×1000. The relevant progression is:

| Frame                                | Reader state                                          |
| ------------------------------------ | ----------------------------------------------------- |
| `08-selected-package-link.ppm`       | Package Link 0 selected                               |
| `09-independent-package-cursors.ppm` | Left 2/2 and Right 3/3 selected                       |
| `10-before-disable.ppm`              | Exact selected state immediately before disable       |
| `12-disabled-pinned-link.ppm`        | Disabled notice, pinned identity and cursors retained |
| `13-disabled-inspector.ppm`          | Expanded exact package/source investigation           |
| `14-disabled-enter-no-visit.ppm`     | Earlier disabled view with ambiguous Enter label      |
| `15-restarted-disabled-pin.ppm`      | Final build restores disabled pin; Enter explains why |
| `16-final-disabled-inspector.ppm`    | Signed source addresses and local ranges are visible  |
| `17-explicit-reenable-same-pin.ppm`  | Re-enable restores the same package and both cursors  |
| `18-explicit-enter-endpoint.ppm`     | Enter adds one visit at the selected endpoint         |
| `19-origin-no-extra-visit.ppm`       | Origin returns without appending another visit        |
| `24-restarted-enabled-pin.ppm`       | Enabled pin and both place 1/2 cursors restored       |
| `25-disabled-again.ppm`              | Disabled notice and unavailable Enter after restart   |
| `26-chord-reenable.ppm`              | Alt+Shift+E restores the selected package pin         |
| `27-explicit-dismiss.ppm`            | Dismiss clears the pin and panel; no replacement      |

The first disable phase (frames 10–14) has unchanged `ops.nodes` and `store.tables` digests and
retains one activity visit. The final-build restart phase (frames 15–19) retains those source
digests. Its disabled restart checkpoint still has one visit and the same pinned identity and
cursors. Re-enable restores the same selected package Link; Enter appends one visit, and Origin
returns to the previous place without a third visit. The successful lowercase-key phase then
restarts with two visits/current visit 1, disables and re-enables the same pin, and dismisses it.
Across checkpoints 24–27, the activity count/current remain 2/1 and source digests remain unchanged.
Frame 27 has no selected Link panel; the package Link is still listed as an unselected link, so
Dismiss clears the pin without disabling the package or altering its activity.

The earlier `20`–`23` run is excluded: its uppercase key arguments left the modal open and the
dismiss target absent, and its exit status was nonzero. The lower-case retry captured at frames
24–27 exited 0 and is the only evidence used for this chord/dismiss result. The final phase does not
cover the separate Retain/Emphasis preference model.

The re-enable evidence uses `build/validation-fourteenth/frozen-final/xuzz`, SHA-256
`b30fb771a3110fd521a86775ec5ca15ffb3c45650a3182a042621257afd89373`, recorded in
`evidence/final-binary-hashes.txt`. It is a distinct phase from the earlier disabled-state run;
frames 15–19 and `evidence/reenable-package.log` carry its evidence.

The native evidence is in `build/validation-fourteenth/native/`, including disable, re-enable, and
chord-dismiss-retry logs, checkpoint snapshots, and raw frames 08–19 and 24–27. No product code or
test was changed for this review.
