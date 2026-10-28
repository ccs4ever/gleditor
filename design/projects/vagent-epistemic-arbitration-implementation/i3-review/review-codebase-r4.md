# I3-1 revision 4 review: compiler and codebase persona

Read only against apps/common/xanadu/vagent/runtime and vortex; nothing built.

## Round-3 findings, verified

- B1 (`$turn_result` lost on backtrack): RESOLVED. Refusals halt instead of failing. `Fail` calls
  `backtrack()` which releases the choice-point mark (vortex_vm.cpp 254), so the earlier design
  could not work; the new one reads the field on the halt path and releases `m0` in `runTurnOnce`
  (the same release already done for `readOnly` turns, agent_executor.cpp 1699). The nested-halt
  spike is well chosen and likely trivial: `emitDispatch` already ends in `jump(haltOp_)`
  (compiler.cpp 5227) and `jump(haltOp_)` is used at a dozen sites; step 8 of `runTurnOnce` unlinks
  `d.stack` wholesale (about 1707) and step 7 calls `vm.clearChoicePoints()`, so leftover `Call`
  frames cannot survive the turn.
- M2/M3/M4 (Duplicate commits nothing; RefusalNote decided in `drainPosted`; settle only on commit
  path): RESOLVED and feasible. `drainPosted` runs on the owner thread (agent_executor.cpp 2711);
  `PostBox::Item` needs a kind enum instead of `bool percept` (526-533).
- `rtBSet_` call sites: verified exactly two (compiler.cpp 4584 in `beliefChange`, 4757 in peer
  `tell`); `rtBDrop_` has three (4175 inside rtBSet, 4571 forget, 4771 untell). The statement that
  the compiler emits no tell/untell test is feasible (`emitTellTests`). The counter tests and the
  Makefile claim are right: `VAGENT_ACTOR_TEST_SRCS` filters `tests/xuzz/vagent_%` (Makefile 1143).
- `clock_.steady()`/`clock_.wall()` exist on `AgentClock` (agent_executor.cpp 705, hpp 610/621), so
  the steady lease and two receipt stamps are implementable.

No BLOCKING or MAJOR finding remains.

## Findings

1. MINOR: the enumeration path for advance and retire turns is not stated, so the cost claim
   "O(K.H)" (section 4) is unsupported. Finding a principal's supports needs either a walk of every
   clause of every arbitrated predicate (O(clauses x sources), and retire must then reach the clause
   via `d.source` negward edges to call `rtBDrop`) or an index. The natural index already exists in
   the topology: the profile master's `d.clone` rank holds one view per record of that principal, so
   walking it is O(K.H); from a view go negward along `d.values`, `d.vars` to the record and along
   `d.epistemic` to the support, then to its clause and predicate for `rtBDrop`. Say this in section
   3, and state the fuel for negward walks (support to clause is O(sources on the clause)).

1. MINOR: history splice mechanics. `link` of the old head onto the new support's `d.epistemic`
   requires first `breakOp(oldSupport, d.epistemic)` (the head's negative edge would otherwise point
   at two owners), then `linkOp(newSupport, d.epistemic, head)`, all before `rtBDrop` (compiler.cpp
   4089-4115 only unlinks the support on `d.source`, so the spliced list survives). Add a topology
   test for reciprocal edges after the transfer.

1. MINOR: failures that carry no `$turn_result` need a class. `rtSettle_` can `failNow()` past
   `beliefEventDepth_` (compiler.cpp about 4258), and VM statement failures yield `kFailStatement`.
   Section 5 classes only fuel and cycles. State that any failed turn without a result code is
   `Refused{TurnFailed, Retryable}` (or Terminal for settle depth), and logged.

1. MINOR: entry proliferation. The design now needs `observeEntry`, `observeRefusalEntry`, an
   advance entry and a retire entry. Emit them (and `rtObserve*`) only for programs that declare an
   arbitrated predicate, and make the `CompiledAgent` fields `std::optional<CellRef>` (no sentinels,
   per AGENTS). Say so in section 6.

1. MINOR: `retireSource` drops supports "through the ordinary belief path" while the checker refuses
   user writers; name the internal caller explicitly as a third `rtBDrop_` site and update the
   compiler guard text, which currently says only two `rtBSet_` sites.

1. MINOR: `EpistemicBudget` is Retryable but nothing frees budget except `retireSource` or history
   eviction by a newer record; a full ceiling therefore retries forever. State the expected host
   action (retire a source or raise the setting), or class it Terminal until the ceiling changes.

## Size

1a about 600 lines with tests. 1b+1c about 2000 (four entries, `rtObserve`, helpers, executor
adapter, turn result, outcome log, bindings). 1d about 700 plus the spike. The split and gating are
adequate; the doc is long but each piece is now implementable.

## Tally

BLOCKING 0, MAJOR 0, MINOR 6.

VERDICT: PASS
