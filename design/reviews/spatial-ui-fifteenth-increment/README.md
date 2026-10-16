# Fifteenth spatial UI checkpoint

Based on `d41664a3ba2f57ddb1e08fd082cf10a40bf67185`, this increment separates reader package
retention, enabled drawing and normal/quiet emphasis. The verified payload cache is independent of
these preferences. Reviewing a downloaded candidate changes none of them; enabling requires explicit
retention of that exact immutable carrier. Removing a carrier disables it and preserves its
preference entry and emphasis as a recoverable removed item. Dismissing the selected Link remains a
separate navigation action and does not remove its package.

## Implementation

`ReaderPackagePreference` is keyed by `InfoHash`. `cacheVerified()` reviews signed bytes and
recomputes the torrent hash; `verifiedPackage()` resolves cached identity independently of retention
or enabled drawing. Format 2 stores exactly retained, enabled and emphasis fields per choice, keeps
the prior bounds and atomic write discipline, and refuses enabled-but-unretained or unknown emphasis
records. Format 1 is refused and preserved unchanged. No compatibility shim, store/node format
change, fixture regeneration or new camera/view architecture is introduced.

The existing package review form offers separate Retain/Remove, Enable/Disable and Quiet/Normal
choices. Links and responses distinguishes reader inventory, downloaded candidates and removed
packages; inventory counts distinguish verified drawable links from unavailable retained payloads.
Missing bytes preserve immutable preference identity, with explicit unavailability and a Find
packages route through existing discovery. Hash-only fetching is not implemented; recovery still
requires the existing signed catalog/entry verification path. No missing service is simulated.

The retained Link panel exposes the same decisions through the shared command boundary. Default
chords are Alt+Shift+R (retain), M (remove), Q (normal/quiet), U (disable) and E (enable). System
keymap and Vortex host registration use the existing action wiring. Removal and disable preserve
both endsets, independent cursors and Origin, refuse Enter, cancel pending entry and invalidate
stale beam actions. Retention alone never enables drawing. Navigation Dismiss retains its existing
separate Alt+Shift+D binding.

Quiet is reader presentation, separate from authored prominence. It scales only package passage
mark/ribbon alpha using live `beams.packageQuietAlpha`; semantic hue, signed payload, endsets,
picking and enabled state remain intact. Strands cache their quiet bit during rebuild and draw reads
the live policy without allocating. Explicitly selected beams retain their full-strength visibility
cue. The owning test also exposed a pre-existing decoration defect: a recomputed Session cache was
not emitted until the next call. Recompute now appends its styles immediately, so changed emphasis
does not wait for another render call.

## Validation and evidence

GNU Make succeeds on the final source. Complete UI 241/241, Xuzz 107/107 and package 9/9 pass.
Owning assertions cover independent immutable choices, same-title candidates, disabled/removal
persistence, missing verified payload, explicit restore without enabling, previous-format and
impossible-record refusal, pinned context and source/activity purity. The emphasis test checks
first-call marks, unchanged RGB/ranges and signed bytes with reduced alpha. Earlier UI attempts
failed this first-call assertion; they are retained as the targeted red evidence. The initial build
attempt linked mixed in-progress API names; its undefined-symbol failure is not behavioral red
proof. Later canonical builds settle those objects and pass.

Library/shared-library binaries remain byte-identical to the preceding checkpoint, permitting reuse
of library 799/799 and owning mip/cache 53/53 evidence. Strict core GL/GLES/Vulkan comparison passes
unchanged thresholds: GL/GLES ordinary and overlay match exactly, Vulkan differs by 0.5104% /
0.5096% under the unchanged 1% limit and one-level channel tolerance. One/four-thread frames match,
but this small sample does not split recording. Broad E2E is excluded. This is not
optimized-performance or native AT-SPI acceptance.

## Native controls and exact evidence

The native run uses OpenGL at 1440×1000 with all six required headless variables and a fresh
isolated GPG home. Actual controls author a two-left/three-right Link, sign a local publication,
prepare its independent package without network announcement, and review verified endpoints. The
exact carrier is `f985e7513550a08b7463fbbc7008e8cb931d75e0`; its package Link is ordinal 0,
authority `local:c299281f1dccfb5fb1be0d8d6492142f`. The public validation signing fingerprint is
`01EBFBA5CF323BA6AD62BCACCC6D22FB9B16BB79`. Signature verification is explicitly separate from
unchecked curator enrollment. No profile, private key, primedia, store binary or app binary is
committed.

Frames 00–04 use intermediate build 3 (`63c03b89…`) for real authoring/signing/preparation. Frames
05 onward use final build 4 (`d791c7ac…`), which also emits newly recomputed decorations
immediately. These executable phases are distinct; no binary-equivalence claim is made. Frame 04
shows initial seeding, not final readiness. The historical names 05-package-status and
06-review-unretained are misleading: they actually show the source-selection form and downloaded
candidate list. Frames 08–09 establish actual ready status and verified review, with the full
immutable hash legibly wrapped in the review subtitle.

Review frames 10–12 separately establish retained-but-disabled/normal, enabled/normal, then
enabled/quiet. Frame 13 selects that package Link without entering an endpoint. The format-2 ledger
snapshots start at 14; historical ledgers are not backfilled for 10–13. Inventory frames 18, 20 and
22 show removed (0 retained / 0 enabled), retained-disabled (1 / 0; 1 known retained Link / 0
drawable), and enabled (1 / 1; 1 / 1), respectively.

Some cursor step names overstate what the images show. Frames 15–22 actually preserve Left 1/2,
Right 2/3; frames 23–28 preserve Left 2/2, Right 1/3. They are valid lifecycle evidence for those
actual contexts, not Left 2/Right 3 proof. The root independently verifies Left 2/2, Right 3/3,
active Right and Origin bytes 0–20 in 29. Frames 30–33 preserve these exact pins through disable,
removal, retention without enablement and explicit re-enable. Restart frame 34 restores the same
pins and retained/enabled/quiet choices. All 10–36 operation snapshots show one completed visit,
current 1, unchanged source operation dumps and unchanged actual source store file hashes. Reading
place metadata saved on shutdown is private activity state, not another completed visit.

Frames 35–36 compare unselected normal and quiet while both remain retained and enabled. The
format-2 ledgers prove that only emphasis changes. The connector gap region has 1,032 differing
pixels with maximum channel delta 97; top chrome is unchanged. The full-frame difference also
contains a minimap redraw, which is measured separately and is not attributed to quiet. Both
Relations remain available in the accessibility list. The left page remains clipped by the current
camera framing; these captures do not establish full reading-layout acceptance. Native AT-SPI,
GLES/Vulkan native package visuals and curator enrollment are not verified by this GL run.

Independent UX/visual review records the compact inventory's ellipsized hash (full detail remains
available in Review), the terse × control and “pinned Link retained” wording as clarity limitations.
They do not change the independently persisted reader retention state. Exact action logs, original
PPM/RGB hashes, lossless PNGs, bencode ledgers, source/activity dumps and decoding scripts are kept
in `native-package/` with separate executable phase attribution.

Separate full J23/J24/J25 and complete 16+16 acceptance, unavailable exact-carrier recovery,
cell/history/media-position restoration, native AT-SPI and optimized performance remain open.
Reconcile package lifecycle evidence against the publication audit before starting D2 enumeration.
