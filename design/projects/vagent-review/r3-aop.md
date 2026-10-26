# Round 3 critique: agent-oriented programming / BDI semantics (role: aop)

Document: `design/vagent-agent-language.md` v3.0. Code checked: `mesh_actor.hpp:107-117`
(owner-thread runtime, `Executor` signature), `vortex_vm.cpp:944-985` (Choice/Fail/Cut), plus the
doc's own citations.

## Status of round-2 BLOCKING/MAJOR findings

| R2  | Topic                                 | Status                                                                                                                               |
| --- | ------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| 1   | forward failure vs rollback           | RESOLVED in the main by staged async effects (V6, §6.7). A residue about what is undone inside a failing segment remains, see 3      |
| 2   | waiting intentions never reconsidered | RESOLVED (§6.4: every live intention at every turn end). A side effect on turn atomicity, see 1, and on achieve-while-waiting, see 2 |
| 3   | stranded intention after hard failure | RESOLVED (§4.2 recovery turn, `fail_step`). Gaps for `ask`/delegation replies, see 9                                                 |
| 4   | belief set semantics, event payload   | RESOLVED (V8, §6.1, term triggers). Minor: spurious events on revision, see 10                                                       |
| 5   | percepts and async effects            | RESOLVED (§7.5, V6). New gaps in effect lifecycle, see 2 and 6                                                                       |
| 6   | `await`                               | RESOLVED (§6.6). Edge semantics unstated, see 8                                                                                      |
| 7   | maintain churn                        | PARTLY: back-off added; the data model of a maintain goal is still undefined, see 4                                                  |

## New and remaining findings

### 1. MAJOR - 4.2 step 6, 6.4, 6.3: end-of-turn deliberation runs user code that can …

Full title: 1. MAJOR - 4.2 step 6, 6.4, 6.3: end-of-turn deliberation runs user code that can abort
the whole turn

Problem. Step 6 evaluates, for every live intention, `drop when` and `succeed when`, and every
maintain `holds`. These are user expressions with guard-like freedom (capacity calls, `findall`,
belief walks). Step 6 runs under the turn's single base choice point and fuel. If one of these
conditions exhausts fuel, trips a contract, or recurses through `belief_event_depth`, the failure is
"hard": the turn is undone and the recovery turn blames "the running intention" or drops the event.
The faulty condition belongs to some other intention that did not run, so the recovery blames the
wrong party, the condition is still there, and the next turn fails identically. Every message turn
then rolls back at step 6 and the agent is bricked, silently dropping all input (`dropped_events`).
This is the price of making reconsideration universal (R2 #2) without isolating it.

Evidence. §4.2 steps 3, 6, 7 and the recovery paragraph; §6.7 "hard failure" row; §15 "Per-turn
deliberation".

Fix. Evaluate each reconsideration condition in its own bounded sub-step with its own choice point
and a fuel slice (settings: `vagent.condition_fuel`). A condition that fails, runs out of fuel or
violates a contract counts as "does not hold" for `drop when`, `succeed when` and `holds`, and fails
that intention's plan with cause `condition` (so selection proceeds), never aborts the turn. State
in §4.2 that step 6 cannot cause a hard failure of the turn.

### 2. MAJOR - 6.4, 6.6, 12.3: an achieve goal ends while its effect is in flight, and …

Full title: 2. MAJOR - 6.4, 6.6, 12.3: an achieve goal ends while its effect is in flight, and an
ending intention cannot cancel its effect

Problem. `succeed when` is tested at every turn end for waiting intentions (§6.4). In §12.3, `Reach`
waits on `follow(route)`; a `Position` percept that matches `location` (or a goal condition true by
coincidence, such as a pre-arrival position inside tolerance) ends `Reach` as achieved while the
drive continues. More commonly `drop i` (the hazard handler, `drop when`, a parent ending) ends an
intention that waits on an effect. Nothing tells the host to stop: `drop` sends `cancel` only for
delegated goals (§6.4, §8.6, §9.1). The host effect keeps running, its result is counted late, and
the agent has no record that the chassis is still driving. The example relies on a second staged
`halt()` winning a race at the host, and `halt` is itself an effect with no completion visible to
anyone (see 6).

Evidence. §6.4 last sentence; §8.6 `drop i` row; §9.1 `drop i` row; §12.3 step 7.

Fix. Treat an in-flight effect as a delegate for dropping: `drop`/end of an intention waiting on an
effect stages an effect `cancel` outbox entry; native skills implement an optional
`on cancel(method)` binding (host contract: best effort, idempotent). Say that `halt()` in the
example is the domain-level stop, `cancel` is the housekeeping. Add a `late_deliveries` note that a
cancelled effect's result is dropped. Alternatively state that `succeed when` is not tested for an
intention whose `waiting` kind is `effect` (the plan owns the action), and price the choice.

### 3. MAJOR - 6.7, 6.5 `continue`: what a failing plan segment undoes is stated two ways

Problem. §6.7 says "writes made after the last suspension, in the failing turn, are undone with the
failed statement". The mechanism cannot do both halves of that. Bounded statements `#Cut` to their
entry depth and keep their writes (V13); only the failing statement's own choice points unwind. A
plan segment is several statements, and the earlier ones have already been cut. Handlers get a
segment-level choice point (§6.5, §9.3), but `continue(i)` in §6.5 and §9.3 shows none for plans. So
either (a) a plan segment is all-or-nothing like a handler, which needs a per-segment choice point
and means a staged `emit` or `tell` before the failing statement is also removed, or (b) only the
failing statement is undone and earlier statements' writes and staged messages stand. Plan authors
need to know which, because `emit SurveyFailed{...}; fail` in §12.3 is only sound under (b) (`emit`
before `fail` must survive for the idiom to work) while §6.7 reads as (a).

Evidence. §6.7 last paragraph; §6.5 `continue(i)`; §9.3 (handler lowered, no plan analogue); §12.3
`survey` plan `else { emit ...; fail }`.

Fix. Choose (b) for plans, and say so: a plan segment runs as a sequence of bounded statements with
no segment-level choice point; a failing statement undoes only its own writes; staged outbox entries
of earlier statements stand. Rewrite the §6.7 sentence accordingly, add a plan analogue to §9.3, and
note that this differs from handlers (all-or-nothing) with the reason (a plan's `else` and
`PlanFailed` do the compensation, a handler has no later step). If (a) is preferred, `fail` after
`emit` in the example must change.

### 4. MAJOR - 5.6, 6.2, 6.3: the representation of a maintain goal and its restorer is unspecified

Problem. The text says a maintain goal "never ends while the agent lives" and that at turn end a
restorer is "adopted" for each maintain goal whose `holds` fails with "no live restorer". It does
not say (a) what the maintain goal is at run time: an intention that is always live? Then it has the
intention fields `plan` and `waiting` but never runs a plan; or a standing registration that is not
an intention? Then there is no cell to hold `blocked_until` and nothing for `intentions()` and
`drop i` to see. (b) How a maintain goal is started: `adopt KeepCharged()`? Until then `holds` is
not monitored? (c) Which plan list the restorer draws on (the plans `for KeepCharged`), and how a
restorer is told apart from the goal intention (for "no live restorer"). (d) What the hazard
handler's `for i in intentions() do drop i` does to a maintain goal: it drops the standing goal for
good, with nothing to re-adopt it, which is probably not what a rover author wants.

Evidence. §6.2 table (maintain row), §6.3 last paragraph, §5.6 attribute list (no `kind` or
`restorer` field), §12.3 (no maintain goal in the example at all, so the weakest-specified mechanism
is the one with no worked case).

Fix. Specify: a maintain goal is adopted like any goal and yields one **goal intention** that stays
in `d.intention` for ever and holds no plan; restorers are its subgoals (on `+d.subgoal`, at most
one, which is exactly "no live restorer"), selected from the plans for that goal. `intentions()`
returns goal intentions and tags them with their kind; `drop` of a maintain goal needs the explicit
call and the hazard example should drop only `achieve`/`perform` goals (filter by kind). Add a
`kind` attribute to §5.6 and a maintain goal with its plan and a back-off case to §12.3.

### 5. MINOR - 6.7 hard failure vs `ask`, `achieve ... by`: the sender is not told

The receiver of an `ask` or a delegated `achieve` that hard-fails produces a recovery turn that
marks nothing (there is no intention yet) and drops the event (§4.2). The sender's adapter sees the
runtime's reply, which is a success result with no answer, so the sender waits to its deadline.
State that the executor returns a typed failure value as the turn result for those kinds, so the
sender's statement fails at once with `cause = "remote_fault"`. Also, `answer` read-only checking
(§7.1) must be transitive through called methods; say so.

### 6. MINOR - 6.5, 8.3, 12.3: handler-staged effects have no completion path

An effect staged in a handler has no intention to resume (V6). `EffectFailed` for the hazard
handler's `halt()` is counted as `late_deliveries` and nobody sees it, so a failed emergency stop is
silent. Raise `EffectFailed{capacity, method, cause}` as an internal event for handler-staged
effects so a handler can react. Also the host binding contract must say that starting an effect
returns without blocking: `MeshActorRuntime` is an owner-thread runtime (`mesh_actor.hpp:107-109`),
so a binding that blocks stalls every agent on the node, which would reintroduce "an agent blocks"
(§14) at node level. Add that to §11.2 (host bindings) and §14.

### 7. MINOR - 4.2 quiescence: in-flight effects are not a checkpoint condition

Checkpoint quiescence (end of §4.2) lists mailbox, unacknowledged sends and undelivered host
deliveries, but not effects the host is still performing. A checkpoint taken during a 10 minute
drive and resumed on another node (or after crash) loses the continuation: the intention waits to
its deadline and the world moved. Add "no effect in flight" to the condition, or have `resume` fail
intentions whose `waiting` is an effect with cause `migrated`. Say which. (The first makes renewal
in §4.5 wait for drives to end; price it next to U-A1.)

### 8. MINOR - 6.6: `await` edge semantics

(a) Does `await believes_term within d` test the base first? State yes (a lost-wakeup otherwise).
(b) Does `consume` in a handler hide the event from an `await E`? State no, awaits are checked
before consume takes effect, or yes, but say. (c) A plan's `await E [guard]` sees only events that
arrive after the suspension; say so. (d) `achieve G by a within d`: when the deadline elapses the
statement fails but the delegate keeps working; stage `cancel` on elapse too, not only on drop.

### 9. MINOR - 7.1: performative table incomplete

`outcome` and `refuse` appear in the glossary, §6.6 and §6.7 but not in the §7.1 table. Add rows:
`outcome{correlation id, status: achieved|failed|dropped, cause}` (sent to the delegator when the
adopted intention ends, receiver completes the suspended `achieve ... by`) and
`refuse{correlation id}`. State what happens when `outcome` arrives for a delegation already
cancelled (dropped and counted).

### 10. MINOR - 6.1, 7.5: revision raises events even when nothing changed

`forget battery(_); believe battery(b.level)` (the canonical percept handler, §7.5, §12.3) retracts
and re-asserts every time, so `-battery` and `+battery` fire on every percept even for an unchanged
value, and any `on +battery` handler runs spuriously; each percept also resets `blocked_until`
back-off (§6.3). Offer a revision primitive, `revise battery(?_) := b.level` or
`believe! battery(b.level)` (replace by functor/arity, raise events only when the value differs),
and use it in the examples.

### 11. MINOR - 6.5, 17: competing intentions on one resource, and plan-end race with percepts

- Intentions interleave at suspension points with shared beliefs and fields, and two of them may
  stage `follow(...)` to one chassis. No selection function resolves it (Jason does not either).
  State that mutual exclusion on a device is the native skill's contract (serialise, or refuse the
  second with `EffectFailed{busy}`), or add an optional per-intention `priority` for ready order.
- §12.3 step 5 tests `Reach`'s condition when `EffectDone` arrives, "from the latest position
  belief". Percepts and `EffectDone` are separate host deliveries in arrival order; the final
  `Position` percept may arrive after `EffectDone`, and then `travel` counts as failed (§6.2 rule),
  `Reach` has no other plan, and the survey fails spuriously. Show the fix in the example
  (`await believes position(location) within 5s else fail` at the end of `travel`) and state the
  hazard in §6.2: a plan for an `achieve` goal that depends on a percept should await it.

### 12. MINOR - 18 matrix claims

"single-minded, reconsidered every turn" is now right but should say "bounded by the cost of §15".
"failure handling: `PlanFailed`, reselection, `else`" should add that no failure plan (`-!g`)
exists; compensation is a `PlanFailed` handler or the next plan, in that order (the handler runs in
the failure's turn, selection in a later one, §6.3). The Jason row "overridable" for plan selection
is correctly "not overridable (U-A12)".

## Assessment

The BDI core now delivers what §6 claims: three goal kinds with separate semantics, plans bound to
goals, explicit selection functions, single-minded commitment, forward failure with staged effects,
belief sets with provenance, wait states, and a recovery path. What remains are specification gaps
at the seams: isolating end-of-turn user code (1), the effect lifecycle on drop and migration (2, 6,
7), plan-segment rollback (3), and the maintain data model (4). Each has a local fix and needs no
C++ beyond the existing list (finding 2 asks for an optional host `cancel` binding at the adapter).

VERDICT: NOT CONVERGED
