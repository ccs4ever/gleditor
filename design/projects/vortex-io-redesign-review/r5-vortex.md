# Round 5 critique: Vortex & Spool-Mesh Expert (vortex-io-redesign.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R4.1 (Actor restore cannot establish the no-resubmission claim): RESOLVED — v0.6 requires an
  off-thread durable attempt record and flush acknowledgement before service submission, checks the
  checkpoint causal floor against that ledger before actor restore/replay, and refuses absent,
  unknown, or unreconciled proof with a typed outcome (§4, lines 238–253). Checkpoints stay
  non-quiescent during attempt admission and unresolved service work; the bounded ledger retains
  attempt/receipt metadata until checkpoint-floor and causal acknowledgements permit retirement. P5
  names `mesh_checkpoint.hpp/.cpp` and a versioned `mesh_actor_io_journal.hpp/.cpp`, lists retained
  bytes, flush, recovery-scan, and reconciliation costs, and refuses actor-profile I/O and
  restore/replay until proof is available (§12, line 676). U-6 requires crash injection around
  flush, submission, receipt, checkpoint, and old-floor restore, plus version refusal and bounded
  retention (§13, line 692). The durable journal is proposed as new actor-specific infrastructure;
  no assumption is made that Evidence's journal can be reused.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

VERDICT: CONVERGED
