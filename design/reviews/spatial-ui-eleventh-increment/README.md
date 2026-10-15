# Eleventh spatial UI checkpoint

Based on `325b38b1`, this increment keeps closed selected occurrences identifiable in the existing
Inspector and repairs a widget-ID collision during reopening. Same-session native recovery is now
measured; same-profile restart remains open.

## Identity and Inspector stability

Previously the Inspector required a validated `ContentReference` before showing the target store and
state. A closed document returns `WrongFrame`, hiding the identity needed to reopen it. On reference
failure it now displays the chosen `OccurrenceSite` store, version and range, plus cell ID for cell
targets, then retains the separate exact-source-reference failure. It guesses no source mapping or
document birth. Successful-reference presentation is unchanged.

The owning regression also exposed an ID collision: a status at line index 10 receives ID 70,
previously also used by the background. Reopening with that status present threw a duplicate-widget
exception. The background now shares the checked allocation above existing actions, pinned labels
and content blocks. Action IDs, theme, metrics and layout policy are unchanged. The source review
records a separate pre-existing limitation for arbitrary large mixed row sets.

The baseline regression fails to expose the closed/wrong-version identity and throws on reopening.
The candidate retains that identity and unavailable reason, refuses the wrong version, reopens the
exact version, preserves Link/cursors/origin/source operations, and allows fresh Enter to add one
visit. The final owning UI suite passes 236/236. The archived red source lacks only the later
selection-presence assertion for clearer failure reporting. This is overlay/accessibility
integration evidence, not native pending-page timing acceptance.

## Native recovery and evidence

The primary candidate run authors a quotation and a two-left/three-right Link through actual
controls. It selects Link 22, Left member 1/2 place 2/2 and Right member 2/3, enters `b1`, returns
to Origin, closes the target, and receives unavailable on Enter. In the same process it opens the
Hypertime map, chooses visible `T · Version b1`, closes the map, and explicitly enters again. The
Inspector establishes the same store authority, state, source address and occurrence range before
and after reopening; matching the title alone is insufficient.

Nine checkpoints show visits 1→2→2→3: the first Enter adds one; inspection, Origin, close, refusal
and map reopening add none; only fresh successful Enter adds the third. All source dumps have
SHA-256 `49a12edef0113d9b6f6b7a120c8f2f7cbf7dd1f6d865e8606dc7dfaac4a164d6`. The last activity delta
appends one visit cell and updates the private current scalar to 3, keeping previous operations. The
token after `visit2` is a parent ID, not the visit ID: equal payloads for the second and third
visits represent separate siblings from the restored root.

The native baseline also recovers within one process. The tenth checkpoint's unsuccessful map
experiment restarted the app and freshly resolved saved ordinal cursors, so it did not establish
that its chosen endpoint remained `b1`. Same-session recovery is established; persistence of exact
chosen occurrence identity after a changed view set is not repaired here.

`native-recovery/` retains lossless frames, original hashes, full logs, command vectors, fresh
six-variable headless environment, frozen binary hashes and source/activity dumps. The primary run
is OpenGL at 1440×1000. Inspector metadata is paged; full accessibility context on page one must not
be mistaken for visible later-page text. A separate page walk captures the actual identity and error
pages. Narrow-window and cross-backend native Inspector parity remain unverified.

## Validation and next task

Canonical GNU Make and fresh UI 236/236 pass. Fresh strict core comparison runs all three local
backends with unchanged criteria: GL/GLES ordinary and overlay images match exactly; Vulkan differs
by 0.5104% and 0.5096%, within the existing 1% criterion. Culling, atlas growth, coarse text,
minified crawl and configured one/four-thread frame checks pass. The small sample does not split
actual recording. Broad E2E is excluded. The first comparison accidentally started that optional
extension and was stopped (exit 143); its log is retained without a broad-suite pass claim.

The shared library and library/Xuzz binaries match the prior checkpoint exactly, permitting reuse of
library 799/799, owning mip/cache 53/53 and Xuzz 99/99. Those are reused results. Native AT-SPI,
optimized performance, naturally pending-page interruption, remote/locked/missing-version variants,
exact cell/history endpoints, full J24/J25 and all 16+16 journeys remain open.

Next preserve exact chosen sites across fresh resolution and same-profile restart, then prove J25
Back/Return and each retained forward child through actual controls. Follow R8 private activity
semantics and preserve unreadable activity evidence; do not silently reuse an ordinal for another
occurrence. Publication/pin-lifecycle audit still precedes D2 history enumeration. No camera,
loader, service, setting, command or format is added.
