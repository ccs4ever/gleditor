# Fourteenth spatial UI checkpoint

Based on `337c04727ba167f226e2ccb6f451b1e0d1cc7b2d`, this increment preserves a package Link's
pinned context when its drawing is disabled. It suppresses its beams, picks, selected highlights and
anchor movement; Enter refuses until explicit re-enable. The retained panel keeps both endsets and
cursors, identifies the immutable carrier and offers Re-enable and Dismiss. Re-enable is also
registered as `std:xuzz/link_enable_package`, default `Alt+Shift+E`, through the existing command
and system keymap paths. No disk format, camera or alternate service is introduced.

Disable cancels pending entry. Beam accessibility work carries its published session generation;
stale queued actions and stale pointer picks are refused. Rendering and new selections retain the
enabled-only package API; pinned inspection/restoration can resolve the verified retained payload.
The Inspector names signed global scroll addresses separately from local observed passage ranges.
Missing payloads preserve diagnostic pins and remain unavailable.

## Validation

Canonical GNU Make passes. Fresh complete UI 241/241, Xuzz 107/107 and package 7/7 checks pass. The
owning UI regression was red before the fix, and checks disabled pins, inaccessible Enter,
accessible re-enable, stale queued work, source purity and actual pending-page cancellation without
late entry after re-enable. The final pending test expansion changes only test code; the native
final executable matches the built product. Library and library-test hashes match the prior
checkpoint, so 799/799 and mip/cache 53/53 results are reused, not rerun. Provenance records hashes.

Strict offscreen core comparison passes GL/GLES exactly and Vulkan at 0.5104% ordinary / 0.5096%
overlay under the unchanged 1% / one-channel-level criteria. Configured one/four-thread frames
match, but the sample does not split recording. Broad E2E is excluded. An initial X11 attempt failed
because Vulkan had no graphics/present device; its log is retained separately. This does not negate
the successful offscreen Vulkan rendering comparison.

## Native controls and limits

The isolated native profile uses actual OpenGL at 1440×1000, all six required headless variables,
and an isolated GPG home. Controls author a Link with two left and three right members, sign a local
publication with an ephemeral validation key, prepare an independent signed package without network
announcement, and enable it through the real verification gate. “Signature verified; enrollment
unchecked” is the intended trust description. No private keys, primedia, profile or store binaries
are committed.

The package's immutable carrier is `da9e373b1f09392112a9e2f36a339defd68168bf`; Link ordinal 0
belongs to authority `local:d9c0fdf9a764d165ab2adba1e83f1ea1`. The original native Link 22 remains
independently visible. The enabled beam list has two Links; disabling removes the selected package
copy and leaves that native Link. Its remaining ribbon is not a disabled-package ghost.

Captures 10–14 use intermediate executable `e8c2fe78…`; 15–19 and 24–27 use final executable
`b30fb771…`. These are separate behavior phases, not equivalent binaries. The final Inspector and
Enter-unavailable wording fixes are verified in 15–16. Both pins (Left 2/2, Right 3/3) survive
restart-disabled and re-enable. Decoded visits remain one through 17, become two only on explicit
Enter at 18, and remain two on Origin at 19 while current returns to 1. Captures 24–27 verify
restart-enabled, disable, governed re-enable chord, and explicit dismissal with visits two/current
one throughout. Exact source operation dumps and actual source store hashes remain unchanged.

Captures 20–22 belong to a failed automation attempt: uppercase key names left the acquisition modal
open, so Dismiss was unavailable and the process exited 1. They prove neither disable nor re-enable.
Capture 23 is absent; its failure marker is retained. The corrected lowercase-key retry 24–27 exits
0\. The archive preserves original PPM/RGB hashes and lossless PNGs, action logs, arguments, scripts
and operation snapshots. Internal accessibility is not native AT-SPI.

Separate persisted Retain/Enable/Emphasis controls, full J24/J25 and complete 16+16 journeys,
optimized performance and native AT-SPI remain open. Implement the remaining reader preference
lifecycle before D2 historical enumeration. Independent source and UX/visual reviews accompany this
bounded increment; their wider requirements do not imply full acceptance.
