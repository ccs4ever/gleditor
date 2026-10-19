# Increment 20 backend report

The publication inbox now offers an explicit, worker-owned discard operation for partial attempt
data on Failed and Cancelled requests. Cleanup is transient status (`Pending`, `Complete`, or
`Failed`); it does not alter XPI1, the request identity, pinned citation, original outcome, or retry
counter. Cleanup is serialized with downloads, blocks duplicate cleanup and retry while pending, and
never runs at startup. Retry remains explicit and allocates the next attempt.

Before deleting anything, the worker considers only canonical `<request-id>.attempt-N` directories
where `N` is no greater than the persisted attempt number. It preflights all candidates for path
containment, symlinks, special files, and checked logical-byte totals. Installed request roots,
malformed/future names, and unrelated request paths are left untouched. It then removes regular
files and directories bottom-up and synchronizes the inbox directory. If removal stops partway, the
transient failure reports bytes successfully removed so far; the durable receipt remains unchanged,
allowing another explicit cleanup attempt. On shutdown, cleanup observes the worker stop token and
does not publish a result after the inbox is destroyed.

The tests exercise failed and cancelled requests, two owned attempt directories with an unsafe entry
proving preflight prevents deletion of either, malformed and future names, symlink and FIFO refusal,
duplicate/pending cleanup and retry refusal, a zero-byte repeat cleanup, preservation of a completed
root and the exact request receipt, explicit retry to the next attempt, and restart without
automatic cleanup. The deterministic failed-attempt fixture contained 1,360 logical file bytes
before discard. This is a test measurement, not a production size estimate.

Validation: `make -j$(nproc) xudu_test` completed successfully. A fresh, headless run of
`PublicationInboxTest.*` passed 28/28 with separate XDG data, config, and cache roots. The full log,
XML (including the byte measurement), and numeric exit code are in
`build/validation-twentieth/final-run/`. The first focused run exposed test timing/state setup
issues; those were corrected, and the final full inbox suite passed. `clang-format-19` and
`git diff --check` pass.

This slice does not add a storage budget, Forget operation, durable cleanup status, or injected
filesystem-sync failure test. A process interruption during deletion may leave some attempt files;
the receipt remains and a user can explicitly request cleanup again. Profile filesystem power-loss
guarantees remain those documented for increment 19.

The two initial failing invocations retained XML reports only (`inbox-first.xml` and
`inbox-cleanup.xml`); separate process logs and numeric exits were not saved. Their failures are
visible in those reports. Only the final corrected run has the full log/XML/actual exit tuple, and
the canonical root run independently passes 77/77.
