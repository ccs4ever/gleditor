# Nineteenth-increment native validation plan

## Ordinary UI restart smoke

The normal Ctrl+O download route can inspect a persisted request, submit a cited link, and invoke
Retry, but it cannot complete an installation in the normal session. `Session::publicationInbox()`
sets `makeTransport` only when the test-only publication swarm is enabled. With ordinary settings,
`PublicationInbox::download()` therefore reports “No publication connection is configured. Content
availability has not been checked.” A retained installation can be revalidated without a transport,
but the normal UI cannot create that retained installation in this configuration. Opening or
importing a local `.xanadoc` is a separate local-file pathway and will not be represented as an
Inbox install journey.

After the parent freezes the nineteenth-increment executable, use a fresh native run directory and
copy only `data/` and `config/` from the eighteenth-increment candidate profile at
`build/validation-eighteenth/native/candidate18/`. This is a continuation of the prior UI-authored
request; the copied `data/` includes the Inbox journal. Create fresh empty cache, GNUPG home, and
other run outputs. Never copy a key or claim a new publication authoring journey.

Launch the frozen app headlessly under private Xvfb/X11 with software OpenGL, private D-Bus, and all
six isolated environment variables. Use Ctrl+O to inspect the existing full-pin download row and
open its status. Do not Retry in this smoke test. Capture the actual rendered frames, internal
accessibility dump, exact argv/environment, logs, and actual process exit. After graceful exit,
inspect LMDB read-only and strictly decode the `XPI1` journal. Compare request ID, every pin field,
phase, verification state, attempt, and error with the eighteenth-increment checkpoint. The expected
result is the same request and pin, still `Failed`/`Unchecked`, attempt `1`, with no automatic
requeue. This validates restart compatibility and UI visibility only; it cannot exercise successful
installation durability or changed durable-write behavior.

The starting request and expected identity are recorded in
`design/reviews/spatial-ui-eighteenth-increment/native-restart-validation.md` and
`build/validation-eighteenth/native/candidate18/persistence-comparison.json`. Reconfirm them from
the source checkpoint and LMDB decode rather than relying on this plan's expected values.

## Installation durability protocol

The complete-install durability protocol must be validated separately by the engine-owned focused
test and actual-workspace filesystem tracing that the parent is preparing. Use the canonical
nineteenth-increment focused test fixture to drive the Inbox install worker and its real filesystem
writes; trace the actual test workspace syscalls and verify the protocol's required write, sync,
rename, and directory-sync ordering against the implementation. Preserve the exact test command,
fixture identity, trace, logs, exit status, and resulting journal/install metadata. The engine test
is the evidence for fault-injected interruption/recovery behavior; syscall tracing corroborates the
filesystem sequence actually exercised.

Do not claim that this is a normal UI journey, a live network transfer, remote availability, or a
physical power-cut test. Do not enable test-only swarm flags in the user UI, mock a UI transport, or
substitute local-file import for an Inbox installation. A transport-backed native journey remains
outside this increment unless a real ordinary user-configured transport becomes available.

## Freeze and evidence gates

Do not launch the nineteenth-increment app until the parent provides its frozen path and full binary
hash tuple. Do not edit product or build files. Keep native captures and LMDB dumps outside
committed evidence until requested; never archive profile roots, private keys, or binary artifacts.
Record exact app/library/dumper hashes and actual exits, and distinguish any failed or
non-submitting input attempt from a completed interaction. No successful-install or power-loss claim
is permitted from the UI restart smoke or `strace` alone.
