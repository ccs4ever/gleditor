# Round 2 critique: agent-oriented programming / BDI semantics (role: aop)

Document: `design/vagent-agent-language.md` v2.0. Code checked: `arena_manifold.hpp:636-650`
(mark/release/discard), `vortex_vm.cpp:944-985` (Choice/Fail/Cut), plus the doc's own citations.

## Status of round-1 BLOCKING/MAJOR findings

| R1  | Topic                                              | Status                                                                                                          |
| --- | -------------------------------------------------- | --------------------------------------------------------------------------------------------------------------- |
| 1   | cross-turn intentions vs checkpoint invariant      | RESOLVED (V2, V4, §5.6, §6.6: intentions are data, alternatives re-derived)                                     |
| 2   | `ArenaSlice` / LIFO marks                          | RESOLVED (V3, one cursor, withdrawn)                                                                            |
| 3   | no belief base / revision                          | RESOLVED in the main (V7, §6.1); duplicates and revision gaps remain, see 4                                     |
| 4   | goal/plan/achieve conflated                        | RESOLVED (§6.2, `plan ... for`, three goal kinds, table)                                                        |
| 5   | no deliberation / intention selection / commitment | PARTLY: cycle and selection functions now stated; commitment is weaker than claimed, see 2 and 6                |
| 6   | `commit;` conflated                                | RESOLVED (removed; positions rules V6, §8.3)                                                                    |
| 7   | plan/goal failure propagation                      | PARTLY: taxonomy good, but the forward-failure rule contradicts the rollback mechanism, see 1 and 3             |
| 8   | epistemic query leaks speculation                  | RESOLVED (`ask` suspends, deadline mandatory, `answer` read-only, late replies dropped)                         |
| 9   | `on` vs goal adoption                              | RESOLVED (`adopt` async, `achieve` plan-only, consume)                                                          |
| 10  | CoW shadow contradictions                          | RESOLVED (withdrawn)                                                                                            |
| 11  | performatives/protocols                            | PARTLY: performatives added; `protocol` deferred with a reason I accept; gaps in the added performatives, see 8 |
| 12  | holonic failure / child death                      | RESOLVED (`ChildDown`, delegation via `achieve ... by`); no cancel, see 8                                       |

## New and remaining findings

### 1. MAJOR - 6.7, V6, 4.2: "a failed plan fails forward, effects stand" is false for arena state

Problem. §6.7 says that after an effect a failing plan's "effects stand". But the step runs under a
base choice point (§6.7 last paragraph) and, per `arena_manifold.hpp:640-646`, `release` undoes
everything since the mark while `discard` of an inner mark only drops the ability to retry (the
bindings stand but remain on the trail of any outer mark). So when a statement fails late in a step
(`drill_core(t); samples := samples + 1; <failing statement>`), backtracking to the base choice
point restores every arena write the step made, including `samples` and any `believe`, while the
hardware effect stands. The agent's model and the world then disagree, which is the exact
inconsistency V6 exists to prevent. The same applies to a handler that calls `halt()` and then
fails: `release(m)` undoes its field writes but not the halt.

Evidence. Doc §6.7 (para 2 and 3), §6.5 pseudocode (`release(m)` on handler failure), §14 ("An
effect is the exception" is stated only for hard failure at turn level); arena header lines cited
above.

Fix. Make the effect a barrier: after a statement-level `effect` call returns, the executor discards
the step's base mark and takes a fresh one (the arena keeps the writes made so far). State this in
§6.7 and §9.1 ("effect call lowering: call; discard base mark; new base mark"). Then forward failure
means what it says: writes before the last effect stand, writes after it in the failing statement
are undone. Say the same for handlers. Add an example in §12.3 (e.g. `samples := samples + 1` placed
after a fallible statement) showing which writes survive.

### 2. MAJOR - 6.4, 6.5: commitment is checked only when an intention runs, so a waiting …

Full title: 2. MAJOR - 6.4, 6.5: commitment is checked only when an intention runs, so a waiting
intention is committed blindly

Problem. Reconsideration "happens only at step boundaries, which are the points where the intention
resumes". An intention waiting on a subgoal, an `ask` or a delegate does not resume until that wait
ends. `Survey` with `drop when not believes mission_active()` is therefore not dropped while its
subgoal `Reach` (possibly long, possibly `yield`ing) runs, and the parent's `succeed when` is not
noticed either. Only `maintain` conditions are polled every turn. This is blind commitment for every
non-leaf intention, not the single-minded commitment §6.4 and the §18 matrix claim.

Evidence. §6.4, §6.5 `continue(i)`, §6.6 table (`SubgoalEnded` is the only resume of a waiting
parent).

Fix. At the end of each turn evaluate `drop when` (and `succeed when`) for every live intention, the
way maintain goals are evaluated, and price it in §15 next to the maintain cost; or restrict the
check to intentions whose clause mentions a predicate touched by a belief event this turn (cheaper,
needs an index on conditions; say so as U-A). Drop of a parent already drops its subgoals
recursively (§9.1), so no new mechanism is needed.

### 3. MAJOR - 4.2, 6.5, 6.7: a hard-failed turn strands the intention whose message it consumed

Problem. On a hard failure (fuel exhaustion, belief-event depth, contract of the dispatcher),
`release(m0)` restores the arena to before the turn, including the outbox, but the mailbox message
has already been consumed. If that message was a `Resume(i)`, `Reply(i,...)`, `Timer(i)` or
`SubgoalEnded(i,...)`, intention `i` is back at its previous step with no message that will ever
wake it, and any `Reply` or `SubgoalEnded` is lost. `TurnAborted` is queued "for the next turn" but
nothing says it re-arms or fails the intention. A deterministic fuel overrun therefore silently
orphans exactly the intention that caused it, and a parent waiting on it waits forever (its
deadline, if any, was also in the rolled-back outbox/timer request).

Evidence. §6.7 "fuel exhaustion" row; §6.5 pseudocode has no recovery arm; §4.2 step 6.

Fix. In §6.5 specify that the executor, after `release(m0)` on a turn that was running intention
`i`, runs a minimal recovery turn that marks `i`'s current plan failed with cause `fuel` (or
`depth`), so the ordinary failure path runs (reselect, or end and notify the parent). State that the
recovery turn must not itself run user code. For event turns, say whether the lost event is dropped
and counted (`dropped_events`).

### 4. MAJOR - 6.1, 7.1: belief base semantics under-specified (set vs bag, duplicate provenance)

Problem. `believe` is `assertz`, which appends. Nothing says `believe p` of a present fact is
idempotent. As a bag, `believes` yields the fact twice (a `for believes ... do` body runs twice, an
`achieve` plan fires twice), `forget` queues one `-p` event per copy, and `findall` counts copies.
§12.1 works around this with a guard (`not believes reported(...)`), which signals the gap. With
`tell`, two senders telling one fact produce two clauses, each with its own `+d.source`, which is
arguably desirable (multiple provenance) but then `believes` cannot distinguish "told by two" from
"told twice by one", and `untell` removing "where the sender is the provenance" is only defined for
one. Belief events `+at/3` have no stated payload: a handler `on +at/3` cannot read the fact's
arguments (the Trigger grammar binds no fields and `occurrence` is not defined for internal events).

Evidence. §6.1, §7.1 table, §8.9 `Trigger ::= ('+'|'-') Ident ['/' IntLit]`.

Fix. State a rule: the belief base is a set per (functor, args) with a provenance set; `believe` of
an existing fact adds no clause and queues no event (Jason semantics); `tell` of an existing fact
adds a provenance cell only, via clone views as V10 requires (many clauses can share a sender
handle, a many-to-one reference). Define the internal belief event cell: fields `functor`, `args`
(positional `a0..an`, since terms are positional) and `source`; show
`on +at/3 as b { ... b.a0 ... }`.

### 5. MAJOR - 1, 6.6, 12.3: no perception model and no asynchronous action; effects …

Full title: 5. MAJOR - 1, 6.6, 12.3: no perception model and no asynchronous action; effects block
the whole turn

Problem. Round 1's "perception" phase vanished with the turn rewrite. Where do percepts come from?
`HazardDetected` in §12.3 is a message, but the document never says who sends it. Skills calling
`mars:hardware` host functions are said to be "outside this document", yet the BDI cycle depends on
them: percepts must become messages (adapter timer/poll or a skill that emits to `self`), otherwise
the agent has no sensing. Second, an `effect` such as `follow(route)` is a synchronous call inside
one turn. A rover leg takes seconds to minutes. During that call the agent processes no message,
which contradicts the §14 claim "An agent never blocks" and defeats the §12.3 step 5 story (hazard
"arriving between turns"; none can arrive between turns of a single long turn, and the `halt()` runs
only after `follow` returns). `yield` cannot help because it sits between statements, not inside an
effect.

Evidence. §6.6 (suspension points are only subgoal, ask, yield), §14, §12.3 steps 4-5, §1 table (no
perception or action-completion row).

Fix. Add a row to §1 and a subsection to §7 for percepts: a skill may `emit ... for [self]` from a
host callback, delivered by the adapter as an ordinary event. Add action completion as a fourth
suspension point: `effect` methods that are long-running are declared `effect async def` and return
after starting the action; completion arrives as an event (`ActionDone{...}`) and the plan waits
with `await ActionDone [guard]` (new suspension point, resumed by a matching event). Reword §14 to
"an agent never blocks except inside a synchronous effect call, whose duration is the skill's
contract".

### 6. MAJOR - 6.6, 8.2: no way to wait on a belief or event except by polling with `yield`

Problem. BDI plans routinely wait for a condition (Jason `.wait`, Jadex `waitFor`, GOAL percept
rules). Here the only waits are subgoal, `ask`, delegate, and `yield`. Waiting for a belief
therefore means `while not believes ready() do yield`, which re-queues a `Resume` and an ack turn
every iteration, so a waiting agent is never idle: its mailbox is never empty, it never reaches a
quiescent boundary, and it never reclaims its arena (§4.4, §15). It also burns fuel and the
4096-message budget (which §11.3 (a)/(b) only widen). This interacts with finding 7.

Evidence. §6.6 table; §4.2 step 7 (checkpoint requires empty mailbox); §11.3 (a), (b).

Fix. Add `await t` (a belief pattern) and `await E [guard] within d`, suspension points whose
`waiting` field holds the pattern; the belief-event round (§6.1) and the dispatcher wake the
matching intentions. Cost: a scan of waiting intentions per belief event, priced in §15.

### 7. MAJOR - 6.3, 6.7: a maintain goal whose restorer fails is re-adopted every turn

Problem. §6.3 adopts a restorer "for each [holds] that fails and has no live restoring intention",
at the end of every turn. §6.2 says a failed restorer is "reported, not fatal" and the goal is
"re-adopted whenever `holds` fails at the end of a turn". If no plan is applicable (battery low and
no charger plan applicable), the restorer fails in the very turn it is adopted (or on its first
`Resume`), is removed, and the next turn adopts another: a perpetual adopt/fail/`GoalFailed` loop
that emits events and self-messages every turn and keeps the mailbox permanently non-empty. Also a
restorer plan that completes without making `holds` true is unspecified (plan success vs goal
success for a maintain goal).

Evidence. §6.2 table (maintain row), §6.3 last paragraph.

Fix. Add a failure back-off: a failed restorer inhibits re-adoption until a belief event touching
the `holds` expression, or until `maintain_retry_interval` (a setting, timer-driven). State that a
restorer's plan completing with `holds` still false counts as plan failure (as for achieve).

### 8. MINOR - 7.1: performative gaps

- No cancellation: `drop i` on an intention waiting on a delegate (§9.1) leaves the delegate running
  with no `cancel` performative; the delegator's late outcome is dropped, but the work and its
  effects continue. Add an `unachieve`/`cancel` kind sent on drop.
- The delegator is not recorded in the intention (§5.6 lists goal, plan, tried, step, waiting,
  frame), yet `continue(i)` ends by sending "the outcome performative to its delegator". Add
  `delegator` (handle + request id) to §5.6.
- "else refuse" has no performative name, so a refused `achieve ... by` waits for the deadline. Name
  `refuse` and make the sender fail the statement with `cause = "refused"` immediately.
- `protocol` declarations remain deferred; fine, but note in §18 that Contract-Net-style
  conversations are by hand until then.

### 9. MINOR - 6.5: the priority example misleads about reactivity

Priority orders handlers inside one turn only (stated), but §12.3 and §8.5 give `priority 100` to
hazard and collision handlers, and step 5 says a hazard "runs at the next turn boundary". The
mailbox is FIFO and each message costs two turns (message plus ack, §15), so the hazard waits behind
every queued `Resume`, ack and event. I accept the refusal of a priority mailbox (§13) but the
document should state the consequence: worst-case reaction latency is queue length times turn time,
not one turn. Add this to §14 and drop `priority 100` from examples where it implies urgency, or
show the adapter's cheap alternative (a high-priority lane in the adapter that reorders its own
pending self-messages, no `mesh_actor.cpp` change).

### 10. MINOR - 6.2, 8.9: goal conditions have no agent scope; reconsideration points in `achieve`

- Top-level `goal Reach ... succeed when position() == location` calls a capacity method bare, but
  bare capacity names resolve through `uses` in an agent or behavior (§8.5). A top-level goal has no
  `uses`. Say that goal clauses resolve in the adopting agent's scope (late-bound), or require
  `goal` conditions to call through a declared `uses` list on the goal.
- §6.2's plan `direct` calls `reserve_floor()` while §12.3 makes `reserve_floor` an agent parameter
  (value, not call). Make one consistent.
- Unhandled events (no handler matches) are silently dropped; state it (and count them).
- When a plan fails, §6.5 says "reselect" but not whether the new plan starts in the same turn. Say
  it starts on the next step turn (a self `Resume`), so one failing goal cannot consume a turn's
  fuel by cycling through every plan.

### 11. MINOR - 18: matrix claims

The "commitment: single-minded" cell is true only with finding 2 fixed. "Failure handling:
`PlanFailed`, reselection, `else`" omits that `PlanFailed` handlers run after the arena rollback of
finding 1. Jason's "overridable" selection is correctly shown as "fixed"; add a line that overriding
selection functions is deferred, with the reason.

## Summary of what remains for CONVERGED

BLOCKING: none. MAJOR: 1 (effect barrier), 2 (waiting intentions never reconsidered), 3 (stranded
intention after hard failure), 4 (belief-set semantics and belief event payload), 5 (percepts and
async effects), 6 (await), 7 (maintain churn). All are specification gaps with local fixes; none
needs new C++ beyond what §11 already lists, except the optional adapter lane in 9.

VERDICT: NOT CONVERGED
