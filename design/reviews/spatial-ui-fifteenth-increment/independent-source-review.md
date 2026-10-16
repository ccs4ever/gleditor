# Independent implementation review: reader retain, enable, and emphasis

Reviewed worktree: `.worktrees/spatial-ui-overhaul`, HEAD
`d41664a3ba2f57ddb1e08fd082cf10a40bf67185`, with the current uncommitted fifteenth-increment
changes. This was a source review only; I did not build or run tests because the implementation
build is already in progress.

## Missing-payload behavior

The reader and removed inventories retain the immutable hash and label absent bytes as “Payload
unavailable” (`apps/common/ui/xanadoc/views_publication_links.cpp:156-168`). “Show selected package”
requires that hash to already have a `LinkPackageExchange` status; otherwise the handler reports
that the carrier must be fetched and verified (`:226-243`). Restoring a removed row likewise
requires a current status (`:233-240`). This is an explicit unavailable state, separate from
preference intent and verified payload presence. The exchange fetch API starts from a signed catalog
and entry, so hash-only fetching is outside this increment. The inventory now offers “Find
packages,” which routes to the existing discovery flow. Keep the unavailable identity visible if
discovery does not recover it; this remains a saved preference whose exact payload may be
unavailable, not guaranteed recovery.

## Confirmed behavior

The reader preference file now uses strict format 2, requires exactly `format` and `choices`,
requires exactly three typed fields per choice, rejects enabled-but-unretained records, applies the
prior byte/count bounds and collision check, and refuses other versions
(`apps/common/xanadu/reader_link_packages.cpp:20-72`). There is no version-1 fallback.
`cacheVerified()` still calls package review and recomputes the immutable torrent hash before
storing payload bytes (`:88-110`). The persistent record and verified payload cache are separate;
absent bytes remain distinguishable from a retained identity.

`setRetained(false)` preserves the preference entry and emphasis, clears enabled, and writes the
updated record; enabling requires both retained intent and verified payload
(`reader_link_packages.cpp:127-159`). This supports a recoverable removed state and keeps disabled
pinned `LinkKey`s available for context. The UI's inventory separates retained from removed records
and reports unavailable retained payloads (`views_publication_links.cpp:133-205`). The selected
disabled panel offers “Re-enable package” only while the preference is retained
(`apps/common/ui/xanadoc/link_panel_overlay.cpp:402-438`); after removal it offers “Retain package,”
correctly matching the API invariant.

Quiet is applied as reader presentation state while preserving semantic link tier and hue. Passage
decoration derives curated package color, then scales only alpha
(`apps/common/ui/xanadoc/session.cpp:3100-3116`). The recompute path now appends the just-built
decoration cache to the caller output at function end (`:3148`); without that append, only
subsequent cache hits emitted styles. The new UI test exercises the first call and checks quiet
alpha while keeping RGB and range unchanged. `LinkBeams::rebuildStrands()` gets every enabled
package link from `presentationLinks()` and stores `quiet` on the strand during rebuild
(`apps/common/ui/xanadoc/beams.cpp:271-315`; field at `beams.hpp:271-284`). Drawing scales document
opacity by `packageQuietAlpha`; active selection then sets alpha to full strength, preserving RGB
(`beams.cpp:2021-2040`). The setting exists in the `BeamConfig` defaults, system-doc schema, bounded
parser, and app startup/reload wiring (`apps/common/xanadu/system_docs.hpp:1029`;
`system_docs.cpp:661-664,3209-3212`; `apps/xuzz/xuzz_app.cpp:3366,3446`). Preference UI actions call
`packageVisibilityChanged()`, which advances link-context revision; beams include that revision in
their cache generation (`link_context.cpp:98-104`; `beams.cpp:2754-2757`). Session invalidation also
invalidates cached passage decoration when quiet choice or alpha changes.

## Checks and limits

`git diff --check` passed before this focused fix. The build and tests are running in the owning
session; I did not run or duplicate them. I found no source-level format, semantic-prominence, or
generation-invalidation defect in the inspected paths. The first-call decoration append fixes a
baseline omission, and the inventory now routes to discovery. Exact payload recovery remains
dependent on catalog discovery and verification; there is no contract-level blocker in the inspected
paths.
