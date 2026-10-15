# Independent review: eighth spatial UI increment

## Product and owning-test review

`Session::advanceViewAfterCommit()` is the right boundary for the reported defect: after a
successful authored commit it refreshes the paired view, and for a changed head in a recognized
system store it repoints that store's current version, saves it, then invokes the existing
system-document callback. The new owning regression exercises the native slice commit callback and
checks the active value remains a double (`24.0`), the paired view advances while its historical
sibling remains at the base, the current head and reopened store agree, and no-op or rejected
commits add no notification. This is focused coverage of the change's contract.

I found no correctness defect in the nine-line product change. One failure boundary remains material
and is correctly disclosed in the eighth README: if `save(index)` throws, the paired view and
in-memory current version have already advanced, while the callback is skipped. The change does not
provide rollback or transactional persistence. The test checks a successful disk reload; it does not
inject a save failure.

The recorded product provenance binds the patch and changed source files to the candidate binaries.
The reported owning baseline-red and full UI 234/234 results are consistent with the evidence
package. I did not rebuild or rerun tests as this independent review was scoped to inspection and
evidence review.

## Native interaction and frame review

I inspected both archived actual-control GL runs. In the default reading-size
`pointer/png/02-controls-expanded.png`, the visible Controls expansion includes Enter in the dock;
`pointer/png/03-enter-selected.png` shows the destination and selected passage, with long text
clipped at the right edge at this 800-pixel viewport. `pointer/png/11-back.png` retains the Link
summary and controls. In `pointer/png/12-origin.png`, focus returns to `home`, but the Link panel is
not visible. The pre-fix `pointer` run and candidate `candidate-pointer` run show the same Origin
state, so this missing panel is not introduced by the present product change.

The separate enlarged combined run has a different result:
`text-gl-link/evidence/png/02-controls-expanded.png` shows the visible controls,
`03-enter-selected.png` shows the selected target, `11-back.png` returns to a blank-looking page
with no visible panel, and `12-origin.png` shows focus on `home` with the Link panel and both
endsets visible. That run also has `system://layout` open and a committed 24px reading setting, so
the difference cannot be attributed to text size. The different Origin outcomes across these runs
leave context-restoration parity unresolved; they warrant an isolated follow-up against the pinned
baseline. They do not establish a universal Origin failure or a product regression in this
increment.

The expanded controls route is demonstrated for 800×600 at the recorded 1.5 UI font scale. It
resolves the earlier collapsed-dock direct-click failure for this setup. It does not demonstrate
paging at another size or complete keyboard and AT-SPI parity.

For GLES text scaling, I independently viewed `text-gles/evidence/png/01-before-setting.png`,
`05-source-live-after.png` and `06-source-after-reopen.png`. The glyphs and line spacing are visibly
larger in the live and reopened captures. The mini-map obscures enlarged lines 9–12, and the
separate clipped-stress evidence documents long-line clipping at 800 pixels. Return-to-source
reframes the camera, so these captures are visual confirmation rather than a controlled pixel or
performance benchmark. The retained store dumps, hashes, callback log and reopen capture support the
stated setting persistence and unchanged source-store claims.

The native OpenGL and GLES evidence supports these individual routes and states. It does not
establish full J24/J25 or J1–J16 acceptance, the 16+16 journey set, native AT-SPI coverage, or
performance acceptance. No complete accessibility, dense-layout, or broad backend acceptance follows
from these frames.

## Runner archive repair

I preserved the exact original `run-native-enlargement.sh` bytes as `run-native-enlargement.sh.txt`,
formatted the current script with `shfmt -i 2 -ci`, and fixed ShellCheck findings without changing
the captured run: grouped adjacent output writes and marked comma-bearing CLI values as intentional
array arguments. `shellcheck` now exits cleanly. The README records both runner SHA-256 values. I
ran `mdformat --wrap 100` on the Markdown files I touched.
