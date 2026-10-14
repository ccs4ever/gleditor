# Seventh spatial UI restart checkpoint

The strict capture investigation reproduced insertion-order-dependent minified glyph coverage on
actual GL/GLES/Vulkan devices. Aligning padded allocations to the deepest mip block fixes that
controlled regression. Two unchanged strict core comparison rounds now pass. This is a restart
checkpoint; full local validation is still required before marking the increment complete.

- [Source diagnosis and red/green proof](renderer-diagnosis.md)
- [Independent initial pixel review](independent-initial.md)
- [Earlier six-run reproduction](evidence/reproduction/report.md)
- [Completed checks and binary/source hashes](evidence/checks/manifest.json)
- [Native journey evidence](evidence/journeys/)
- [Local restart prompt](../../projects/restart-spatial-ui-overhaul-local.md)

The first full library run is 789/790; its unchanged O0 latency benchmark fails at 3392.3 ns, while
isolated candidate and frozen old-library measurements pass at 684.2 and 727.3 ns. No performance
criterion is weakened or accepted. UI/Xuzz suites, full-library rerun and final local gates remain
pending. GL and Vulkan native runs complete with captured source-purity and visit 1→2→2 proofs. The
prepared GLES run and independent final visual review remain pending locally. These bounded checks
do not complete J1–J32.

Raw PPMs, private XDG profiles and cloud runtime helpers are outside Git. Checked-in lossless PNGs,
source/activity dumps, transcripts, provenance and compressed test logs retain the durable evidence.
Rebuild and use fresh profiles in the local environment.
