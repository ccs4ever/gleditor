# Round 3 critique: Vortex & Spool-Mesh Expert (vagent-native-agent.md v0.3)

Tally: 0 BLOCKING, 2 MAJOR, 4 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R2.1 (effects and native skills do not lower): RESOLVED as prerequisite Q1 (§1.1), scope exact.
  Gate adequacy: see minor 1.
- R2.2 (checkpoint vs grants): RESOLVED. §12.3 and §12.4 agree; `checkpointed` removed.
- R2.3 (control-plane death): RESOLVED. `PR_SET_PDEATHSIG`, `_exit` on EOF, epoch-keyed sweep, gate.
- R2.4 (live upgrade): RESOLVED. `upgraded(pid, oldRef, newRef)` with digest/manifest rule.
- R2.5 (handler drops read result): PARTLY. The ReadKey goal fixes the dropped value, but interacts
  badly with N13 (M1 below).

## MAJOR

### 1. (N13, P20.6 `intentionEnded`) The executor has no intention-end transition to signal

**Problem:** P20.6 justifies itself with "only the executor sees these transitions", and §12.4 makes
`intentionEnded(pid, intention)` the sole revocation path for N13. But the executor does not see an
intention end. Intentions end inside the compiled graph: the adapter code removes the intention from
`d.intention` in the settle loop (agent spec: "the intention then ends as dropped in the settle
loop"), including drops that cascade to subgoals and `drop when`. The existing `IntentionObserver`
(`agent_executor.cpp:2045-2046`) fires at the start of a wake turn that continues an intention,
never at its end. `grep` finds no end/drop hook in `agent_executor.cpp`; the only ends it observes
are delegated-outcome staging. The design does not say how P20.6 detects ends (diff of `d.intention`
after every turn? an outbox/percept the graph emits?), what it costs per turn, or what it does for
an intention that ends in a failed-hard turn recovery.

**Second half:** an effect's recorded intention is the one that staged it. For a plan reached
through `achieve`, the staging intention may be a subgoal, while N13 and §10.1 say "top-level
intention". Nothing maps a request's intention to its top-level ancestor (`d.subgoal`, `$parent`).

**Fix:** Specify the detection: after each committed turn P20.6 compares the live intention id set
with the set it knew (price it against C8), and states how a sub-intention resolves to its top-level
root, or scope the grant to the staging intention and say that a plan's `achieve` subgoal opening a
terminal loses it when the subgoal ends. Add the failed-hard and drop cascade cases to the N13 gate.

### 2. Intention-scoped grants contradict the tests and the ReadKey example

(N13 vs §13.1 `curses_keep`, §13.2 steps 8-9, §11.6)

**Problem:** `curses_keep` "keeps its terminal in an agent `var screen`" and "does not close it".
Under N13 the grant is revoked with `intention_ended` the moment the `Greet` intention ends (its
plan completes). Step 8 then claims "the original's terminal still works" and step 9 expects the
revocation cause `agent_ended` after the original is stopped. Both cannot hold: the terminal is
already revoked and the cause would be `intention_ended`. The same defect affects §11.6: `ReadKey`
reads `screen_terminal`, an agent `var` opened by some other intention; that intention has long
ended, so `read_key` fails `stale`. The doc names the escape (a long-lived maintain intention) but
neither the `curses_keep` module nor the §11.6 example uses it.

**Fix:** Make `curses_keep` and the §11.6 example open the terminal inside a `maintain` goal (or
state that terminals in agent vars require one), and show it in the source. Add the test that a
`var`-held terminal is stale after its opening intention ends with cause `intention_ended`, which is
also the case N13 makes interesting.

## MINOR

- **Q1 gate (§1.1).** Adequate for staging, but it stops at the outbox entry. Add: the entry's
  result is decoded for a record result, the plan resumes at its step path after the result, and
  `ANativeSkillIsRefusedByName` is replaced in the same change. State that the gate is Q1's
  acceptance test, owned by the vagent plan, so this design does not carry a second one.
- **Upgrade signal source.** Say `upgraded` fires from `AgentExecutor::upgrade` after the quiescence
  check succeeds, not before, so a refused upgrade does not restart a host.
- **Q2 wording.** "Once Q2 lands, native modules gain the slot" leaves N13's "statement-level else
  not in the front end" as the only reason for intention scoping; say N13 stays after Q2 as defence
  for non-local exits.
- **Ledger cause on drop.** N13 lists "success, failure or drop" but §10.3's vocabulary has one
  `intention_ended`; say drop and failure do not change the cause.
- **`one armed per grant` (§12.5).** Readiness is disarmed on the percept; say what happens to an
  armed watch when the grant is revoked while the host thread is inside `poll`.

VERDICT: NOT CONVERGED
