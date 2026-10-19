# Increment 18 source implementation report

Implemented durable PublicationInbox requests in `publication_inbox.hpp/.cpp` and owning coverage in
`tests/xudu/publication_outbox_test.cpp`.

The inbox now persists a bounded `XPI1` request journal in a new LMDB index under its existing inbox
directory. The journal keeps request IDs, request mode, exact pins, mutable observed pointer floors,
attempts, cancellation/failure diagnostics, and verification status. The codec rejects unsupported
versions and malformed identity/pin/status combinations by typed refusal. It caps the journal at 4
MiB, requests at the configured limit (default 128), title fields at 2 MiB minus 4 KiB (title is
stored in both pin and status), and diagnostics at 4 KiB. Empty publication titles remain valid.

Requests are journaled before queueing. A mutable pointer floor is durable before manifest fetch;
after signed manifest checks, the verified carrier pin is frozen for retry. Shutdown-interrupted
requests restore as Cancelled and require explicit Retry, with no automatic network factory
invocation. Active cancellation holds the phase until the worker acknowledges it, so retry cannot
replace an active attempt. Worker mutations are attempt-guarded. New fetches use request-scoped
`id.attempt-N` roots; completed roots are installed by rename before the journal records Ready.
Startup reconciles a root-before-journal orphan only when the full pin matches. Recognized staged
attempt directories are preserved and never imported as completed snapshots.

Completed retained stores are locally reverified before Ready. Retained root validation rejects
symlinks recursively before reading manifests or stores, including staged roots before reuse.
Invalid retained bytes remain available for inspection. Existing pre-journal completed snapshots can
be imported from their signed manifest and carrier; original mutable request history cannot be
reconstructed when no journal exists.

Tests added or extended cover failed pinned request persistence and explicit retry, interrupted
mutable lower-bound persistence without automatic network, matching-only orphan recovery,
verified-stage hash consistency, staged-manifest crash recovery, retained symlink refusal, journal
cap/unknown version refusal, and active cancellation followed by retry only after worker
acknowledgement. The subscription suite was rerun to check title/retained metadata compatibility.

Validation passed:

- `make -j$(nproc) xudu_test`
- Headless, isolated-XDG
  `build/xudu_test --gtest_filter='PublicationInboxTest.*:PublicationSubscriptionsTest.*'`: 32/32
  passed. Final log: `build/validation-eighteenth/owning-tests-final.log`.
- `clang-format-19 --dry-run --Werror` on the three owned C++ files.
- `git diff --check`.

An earlier combined test run had two fixture-only failures: one asserted the wrong retained manifest
filename, and one copied the publication seed from one directory above the metainfo name. Both
fixture paths were corrected; the final combined run passes. Earlier candidate failures also caught
and corrected a stack-reference issue in journal decoding, a shutdown diagnostic mismatch, and an
overly restrictive 1 KiB title limit that conflicted with the existing subscription rejection test.

Limits: these tests use the existing deterministic transport doubles and establish no native network
proof. LMDB transactions use their default synchronous commit behavior. The filesystem rename that
installs a completed root is atomic for normal process-crash recovery, but no explicit
file/directory `fsync` is added, so this report does not claim power-loss durability for the
installed tree. Product formats for stores and publications are unchanged; `XPI1` is the new strict
inbox-journal format.
