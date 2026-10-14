# Repaired-build 800×600 OpenGL check: failed

This single-case gate ran the repaired canonical build at 800×600/OpenGL with shader optimization
`O0`; it makes no performance claim. The build was stable enough to complete the full scripted flow
and save all six frames, but the landing still fails the visual requirement. `00-authored.png` shows
a blank white source page with only a tiny selection marker. `01-enter.png` is almost entirely
black, with chrome and tabs visible but no readable source passage. No source-complete claim is
warranted.

The UI/action path did execute: `Enter` selected Doc 2, a11y reports the left member active, and the
native tree contains all eight source lines and the full 0–654/0–23 link ranges. Source before/after
hashes match in both documents. The run also exercised Origin, Back, and fold/unfold, and preserved
its activity dump. These state records do not change the visual failure.

Evidence is under this directory; raw PPMs and the fresh XDG profile are outside Git under
`/workspace/spatial-build/fourth-raw/wide/failed-repair-build/` and
`/workspace/spatial-build/fourth-raw/profiles/`. Their frame SHA-256 values are in `manifest.sha256`
and the wide archive manifest. No additional backend or size was launched after this failed 800×600
inspection.
