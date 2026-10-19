# Eighteenth spatial UI checkpoint

Based on `bce65b1f`, this increment preserves publication download requests across process restart.
It follows the seventeenth checkpoint's exact-pin contract and keeps the existing inbox worker,
native forms and view architecture. It does not complete the spatial overhaul.

## Behavior

A new strict `XPI1` journal lives in an LMDB index under the existing inbox directory. Synchronous
transactions retain request IDs, submission origin, exact pins, observed mutable pointer floors,
attempt counters, phase, verification and bounded diagnostics. Submit is committed before worker
wake; pointer advancement precedes manifest fetch; verified manifest identity precedes dependency
work. Failed/cancelled outcomes survive restart. Interrupted work becomes Cancelled and requires
explicit Retry; startup does not instantiate a network transport. Completed retained snapshots
continue local verification on the existing worker before Ready.

A named request resolves under its persisted lower bound until its signed manifest verifies.
Thereafter its verified pin freezes Retry to that immutable edition, preserving checkpoint 17. The
internal submitted-origin flag does not change that retry behavior. Submitting that same full pin
reuses the existing request in any phase. Retry keeps its ID and pin. Empty publication titles
remain valid; the independent subscription title policy is unchanged.

Active cancellation remains pending until its worker acknowledges it, so Retry cannot replace an
active attempt. Attempt guards reject stale worker updates. Cancellation and Ready publication use
the same inbox lock. Persistence errors roll back proposed in-memory changes or expose an explicit
unsaved-state error; they cannot publish Ready from an uncommitted request transition.

New attempts use separate request-scoped directories. Installation renames a completed tree into its
immutable request root before committing Ready. Startup reconciles an installed orphan for a
journaled request only when the full journal pin matches, then revalidates retained content.
Existing completed roots without request records can reconstruct signed snapshot identity; they
cannot reconstruct lost submission history. A crash before the final root rename leaves a recognized
staging directory preserved as interrupted work, never Ready. Retained and staged paths reject
symlinks before use. No earlier installed snapshot is removed by Retry. Unknown journal versions,
malformed identities and inconsistent verified states refuse with `PublicationInboxUnreadable`,
preserving the journal and retained bytes.

The Open dialog now labels downloads with title, exact version, sequence and full carrier hash.
Requests sharing a title and phase can be distinguished before opening their full status. The
existing request ID remains the selection value; inspecting status or retrying never enters a
publication or adds a reader visit.

The C++ changes remain in the existing filesystem/worker and native form owners. LMDB transaction,
thread cancellation and native input interfaces are the reason for C++ rather than a new Vortex
workflow. Store, publication and CompactOpNode formats do not change; fixture regeneration is not
needed.

## Validation

Canonical GNU Make passes. Complete UI 246/246 and Xuzz 107/107 pass with fresh headless XDG roots.
The final expanded owning publication/package suite passes 70/70, including the corruption matrix.
Strict GL/GLES/Vulkan comparison passes unchanged criteria: GL/GLES ordinary and overlay are exact;
Vulkan differs by 0.5104% / 0.5096% under the existing 1% limit and channel tolerance 1. Format,
lint and UI-text policy pass; exact gate results are recorded in `final-checks.json`. Native restart
evidence is documented in [native-restart-validation.md](native-restart-validation.md). The
additional test-only corruption matrix does not change the frozen application source.

Owning tests cover durable failed pins, explicit retry without startup transport, mutable rollback
protection after interruption, cancellation acknowledgement, matching-only installed-orphan
recovery, staged-manifest interruption, nested symlink refusal and strict journal bounds/version.
The matrix mutates valid records into truncated data, duplicate/unsafe IDs, inconsistent pins,
verified identity mismatches and invalid Ready/Complete pairs, checking typed refusal and exact
preservation of journal bytes and retained files. These deterministic transport doubles are not
native network proof.

Default library and library-test binaries remain byte-identical to checkpoint 17; 799/799 and
mip/cache 53/53 are reused on that basis. The plain-editor executable changed during the full build,
so strict backend comparison is rerun rather than reused. No thresholds are relaxed, and broad E2E
and actual split recording remain outside this bounded comparison.

## Remaining work

The journal has a 4 MiB envelope, configured request cap (default 128), bounded diagnostic strings
and bounded title metadata. Accumulated failed-attempt directories have no aggregate retention/disk
policy yet. Filesystem installation uses atomic rename but no explicit file/directory sync, so
process-restart evidence is not power-loss durability proof. Process-kill coverage at every boundary
and injected journal-write failure coverage remain unverified.

Complete the remaining installation/partial-cache lifecycle before enabling normal network traffic,
then implement governed opt-in production connection settings using the existing factories. Keep
named-node-only DHT, disabled local discovery/trackers, normal address/rate protections and no mock
identity verifier. Real authenticated enrollment and BEP46/fingerprint binding are prerequisites for
Verified identity. Remote acquisition/unavailable/bad-content variants, full J24/J25, D1–D5,
S1–S3/Track H, native AT-SPI parity, optimized performance and full J1–J32 acceptance remain open.
