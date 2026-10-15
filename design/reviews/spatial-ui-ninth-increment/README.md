# Ninth spatial UI checkpoint

This checkpoint begins at `25b1ccde`. It corrects the eighth review's Origin/Back panel finding
through decoded-pixel comparisons, isolated native runs and an independent audit. No product change
is justified by these captures.

## Origin and Back correction

The eighth report and independent review said some Origin or Back frames lacked the Link panel. That
conclusion was wrong. The entire panel band, x=0..799 and y=316..550, is pixel-identical across the
pre-fix pointer, candidate-pointer and enlarged GL Back/Origin frames. Each decoded RGB crop hashes
to `7e81dc163aa69b2a1930d5c0eba34713752d39250de708177c3dee0de55c9a51`. The crop contains 112,850
pixels of the panel's dark surface plus text, controls and other panel colors. It is present in
every file. The full frames can differ in the surrounding document and focus without losing this
context.

`lead-pixel-audit.json` records original file hashes and decoded full-frame and panel-region hashes;
`candidate-origin-panel-crop.png` retains the inspected region. The independent audit separately
recomputes it. Historical eighth reports remain unchanged; this correction supersedes their
missing-panel, blank-Back and context-parity findings for these frames.

The isolated native Back and Origin branches also preserve the selected Link, both endsets,
unchanged source operations and two completed visits after the single Enter. Their initial
preview-based report repeated the mistake. The corrected `red-native/findings.txt` preserves that
original report separately and makes clear that its name describes the reported finding before
audit, not a failing product test. Neither exit codes nor preview interpretation alone are taken as
sufficient pixel evidence.

The diagnostic GL immediate and delayed Origin captures are byte-identical. Both have the same
complete panel region. A separate offscreen RADV Vulkan run also has byte-identical
immediate/delayed captures, with 112,850 dark panel pixels in each. Its panel-region hash differs
from GL, so this is retained context on each backend, not exact cross-backend image equality.

Temporary named debug/trace instrumentation confirmed the panel draw path was enabled and its dock
reservation settled. All instrumentation was removed; its patch, build logs and evidence remain
under the diagnostic directories. The restored GNU Make build passes. The shared library and all
three relevant test binaries match the prior passing checkpoint byte-for-byte. The app hash changes
with its generated version string at commit `25b1ccde`; the generated config and exact hashes are
retained. No fresh full-suite or performance pass is claimed by rebuilding unchanged product source.

## Native repeated-occurrence route

The valid native GL run uses the restored product at 1440×1000 in a fresh six-variable headless
profile. It types the fixture through the UI, selects bytes 0–20, and uses the default Ctrl+T
quotation action. The authored Transclude operation creates version `b1` from those same source
spans; this is a second occurrence of the same primedia, not a second copy of typed text. The
ordinary pouch/forge controls create one Link with two left and three right members.

Default member/occurrence commands select Left member 1/2, place 2/2. Crossing and selecting Right
member 2/3 leaves the left cursor unchanged. Crossing back retains Left place 2/2; Enter reaches the
quotation's exact `b1` version and range 0–20. Investigation confirms the other side retains Right
member 2/3, range 55–65 in version `a34`. Link identity and both complete endsets remain available
throughout. `lead-repeated-audit.json` records the lead's independent dump audit.

All eight audited navigation snapshots have the same 36-operation source dump. Completed visit cells
remain 1 through preview and independent cursor changes, become 2 after one Enter, and remain 2
through Origin, investigation and fold. Total activity operations are 12, then 16 after Enter, then
18 after Origin: the last two update the private activity-current pointer, not completed visit
nodes. They are not writes to the visited source.

The archived older template replay lacked a selected range before Ctrl+T and therefore created no
quotation on this build. A first corrected run created the real quotation but selected only its
first occurrence. Both are retained as partial/invalid attempts, excluded from the explicit second
occurrence proof. Choosing from an unset cursor first selects its first member or occurrence; the
valid run makes the subsequent explicit choice and verifies it before Enter. These current replays
do not establish what an older historical binary did.

## Remaining scope

The restoration audit resolves the reported missing-panel discrepancy in these captured states. The
repeated-occurrence proof is one GL route; it does not cover pending/unavailable cancellation, stale
replacements, cell/history endpoints, branching forward choice or same-profile reopen. Complete
J24/J25, the 16+16 journeys, native AT-SPI and optimized performance remain open. Narrow-window
long-line clipping and mini-map occlusion remain recorded limitations.

## Verification and next task

The independent repeated-occurrence review confirms the exact cursors, `b1` destination, restored
origin, private activity-pointer updates and unchanged source hash. Its decoded panel region is also
identical before Enter, after Enter and after Origin; full frame hashes and raw capture provenance
remain in the valid bundle. No source-code change remains in this checkpoint.

`final-checks.json` and compressed gate logs record the final repository checks. The root
`SHA256SUMS` inventories the committed evidence; nested manifests preserve their original run scope,
including some absolute temporary paths. Those paths are provenance, not runtime dependencies. The
next task is J24 pending/unavailable cancellation and stale replacement, then exact cell/history
endpoints and J25 branching/reopen, as recorded in the project status. The publication/pin-lifecycle
audit still precedes D2 history enumeration.
