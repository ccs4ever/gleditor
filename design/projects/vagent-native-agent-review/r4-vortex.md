# Round 4 critique: Vortex & Spool-Mesh Expert (vagent-native-agent.md v0.4)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R3.1 (intentionEnded has no transition to signal): RESOLVED. Derived in
  `AgentExecutor::deriveSchedule` (`agent_executor.cpp:2187`, `:4038-4072`) by diffing live
  top-level intention ids off `d.intention`, with subgoals mapped to root ancestors via a negward
  `d.subgoal` walk at staging (N13, §10.1, §12.4, P20.1, P20.6).
- R3.2 (N13 contradicts curses_keep and §11.6): RESOLVED. N13 introduces explicit `hold_terminal(t)`
  to promote grants from intention scope to agent scope; `curses_keep` and §11.6 hold their
  terminals, matching test step 8 and ledger accounting in step 9 (§10.1, §11.6, §13.1–13.2).

## BLOCKING

None.

## MAJOR

None.

## MINOR

- **P20.1 table summary wording (§14.2 line 1206).** The summary table states "the staging
  intention's id, absent for a handler", whereas N13 (§4) and §10.1 specify that P20.1 resolves a
  subgoal to its root top-level ancestor by walking `d.subgoal` negward when the effect is staged.
  Clarifying the table text to "the staging intention's resolved top-level ancestor id" will align
  the summary row with N13 and §10.1.
- **`deriveSchedule` signal generation site (`agent_executor.cpp:2188`, `:4038-4072`).**
  `deriveSchedule` is called from read-only inspect queries (`readyIntentions:4080`,
  `nextDeadline:4091`) as well as turn commits (`:2188`, `:3961`). Because intention removal only
  occurs within compiled graph turns, caching the prior live top-level set on `State` and diffing
  during `deriveSchedule` is safe and idempotent across between-turn inspect calls.
- **`hold_terminal` idempotency (§10.1).** §10.1 notes that `hold_terminal` on a revoked token fails
  `stale`. It should explicitly specify that calling `hold_terminal` on an already agent-held token
  succeeds as a no-op.

VERDICT: CONVERGED
