# Seventeenth spatial UI checkpoint

Based on `f3841729`, this increment makes cited-publication request identity and verification
visible without conflating a signed Link package, curator enrollment and available publication
content. It also fixes offline snapshot verification and subscription restore safety. The broader
spatial UI goal remains open; [remaining-goal-map.md](remaining-goal-map.md) records its full gates.

## Implementation

`PublicationDownloadStatus` carries the exact `PublicationPin` immediately for a pinned request. The
pin contains publisher, name, immutable manifest hash, sequence, version and title. Mutable requests
acquire that pin only after the returned manifest signature and all requested identity fields
verify. Subsequent retry uses that exact pin instead of resolving a newer head. Opening a citation
again returns its existing request in any phase; Retry is the explicit restart operation. Only
complete full-pin equality reuses a request, so equal titles or mutable names cannot substitute
another carrier.

Verification has three real stages: Unchecked, ManifestVerified and Complete. The second means
signed manifest identity has verified while content remains incomplete; Complete follows dependency
verification and native installation. Failure/cancellation retains the reached stage and requested
identity. Retry resets verification because that attempt rechecks bytes. A missing connection now
states that content availability has not been checked, with no test-swarm workaround in product
text.

The existing download form shows requested carrier, edition, publication name, publisher key parts
and verification, separately from transfer phase/error and unchecked publisher enrollment. It offers
Open/Subscribe only for Ready+Complete, Retry only for Failed/Cancelled, and Cancel only for active
work. Refresh/Close remain available. Default Enter refreshes; it never opens the publication.
Package Review correlates citation status by the complete pin and shows Not checked, Checking,
Complete local copy, failed or cancelled request. Merely reviewing starts no network request. The
shared identity-part helper is moved unchanged from the existing inspector to avoid duplication.

Completed snapshots previously reopened as Ready after manifest decoding and a file-existence check.
They now verify retained dependencies on the inbox worker, compare the deployed native store with
the signed history (document, exact state, every operation, scroll descriptors and deployment
bindings), then become Ready/Complete. Corruption reports failure and preserves retained files.
Retry on this path verifies locally and does not fetch or replace data. Completed pin/hash status is
reconstructed from the retained signed manifest; interrupted/failed network requests still are not
persisted across process exit. This is not an original-request persistence format or compatibility
shim.

Subscriptions restore signed citation metadata independently of current content availability, so
asynchronous local verification cannot race their constructor and discard an otherwise valid
receipt. Persisted current and notice hashes/titles must match the exact pinned snapshot. Runtime
subscription creation and update acceptance still require Ready+Complete with a native store.
Unavailable baseline receipts survive; tampered subscription records are refused and preserved.

The changes stay in the existing C++ native form and inbox worker owners: they bind native form
events, verify signed binary carriers, access files and coordinate worker cancellation. These OS,
transport and renderer interfaces are the reason for C++ rather than a new Vortex workflow. No
second network or camera architecture is introduced.

## Validation

Canonical GNU Make succeeds. Complete UI 246/246, Xuzz 107/107 and owning publication/package 61/61
pass. The owning total includes outbox 15, inbox 12, subscriptions 12, package exchange 11 and
reader packages 11. Five direct form tests cover keyboard and internal accessibility commands, exact
identity, unavailable/unchecked and manifest-only states, deliberate Open/Subscribe and
phase-appropriate actions. Engine tests cover exact completed/failed request reuse, retry, offline
hash restoration, corrupt retained content/native identity, unavailable subscription restore and
persisted hash/title tampering. Unit transport doubles are not native network evidence.

Default library and library-test hashes remain identical to the prior checkpoint, permitting reuse
of 799/799 and mip/cache 53/53 results. The new isolated AccessKit variant has separate provenance
and 33/33 focused accessibility checks; those default-library results are not attributed to a
different library binary. Strict GL/GLES/Vulkan comparison passes unchanged thresholds: GL/GLES
exact ordinary/overlay, Vulkan 0.5104% / 0.5096%, channel tolerance 1, limit 1%. Configured thread
frames match, but actual split recording and broad E2E are not exercised. Optimized performance
acceptance remains open.

Format check passes with the required clang-format 19; lint and UI-text policy pass. The first
format run selected installed clang-format 23 and failed; the explicit tool override corrects that
environment mismatch. Two new Markdown findings were corrected before the passing lint run.

## Actual native evidence

The isolated profiles continue the sixteenth checkpoint's UI-authored workspace by documented
profile copies into fresh XDG roots. No new authoring pass is claimed. The package carrier is
`722911c0021f2b2352c5efc76c2d683088d6cbac`; its cited publication is the different object
`f8ff688c5680ab88c27e4297decb5e7640b86a31`, title document, sequence 2, state a34. These are package
and publication hashes, not two editions of one object.

Baseline app `19792347…` exposes the real no-transport failure but no requested identity or separate
verification fields. Initial candidate `8a38fb2d…` exposes those fields. Its frame 03 is misnamed:
the static form still offered Cancel, so that action does not prove Retry. That unsuccessful attempt
is preserved. Final app `a1ff64ff…` has the polished wording and Publication name label. Final frame
04 captures a refreshed Failed menu before Retry; 05 shows the explicit retry Queued with the same
pin; 06 refreshes the real failure. Frame 07 shows Review's exact citation as Request failed; open
status, and 08 returns the existing failed status without a new queued request.

This is a genuine configuration failure, not remote-content unavailability or signature rejection.
The normal launch has no transport configured. All source operation/file and reader preference
hashes remain unchanged, with one completed visit/current 1 and no references/annotations. Activity
shutdown reading-place metadata may append; full activity files are not claimed immutable. Lossless
frames (including byte-exact gzip-compressed raw PPMs), exact arguments, logs, hashes and decoded
evidence omit profiles, keys and binaries.

Native AT-SPI now registers Xuzz through the isolated AccessKit build in this environment. Its
capability evidence distinguishes actual field reads/native action selection from ordinary keyboard
submission. It does not establish complete assistive-technology journey parity. Installed native
text-entry editing support remains an explicit limitation rather than an internal-tree pass.

## Exact next task

First persist failed, cancelled and interrupted inbox requests, their stable IDs and exact retry
identity across restart. Restore interrupted requests without starting network work; explicit Retry
remains required. Complete the installation/journal recovery contract before treating request
durability as accepted. See [request-persistence-plan.md](request-persistence-plan.md).

Then implement normal governed publication transport configuration using the existing
workers/factories, without the test-swarm mock verifier or privacy relaxations. Preserve
named-node-only DHT, disabled local discovery/trackers, normal distinct-network/address/rate
protections and disabled-by-default traffic. Expose the effective configuration and explicit restart
boundary through ordinary controls. Then supply a real authenticated enrollment/trust-root and
BEP46-to-fingerprint binding path before claiming Verified identity. See
[next-service-plan.md](next-service-plan.md).

Native bad-payload/remote-unavailable/enrollment-positive variants, complete J24/J25, D1–D5 and
S1–S3/Track H, all J1–J32 acceptance and optimized performance remain open. Do not start D2 by
pretending these publication dependencies are complete; no missing service is simulated.
