# Round 1 critique: agent-oriented programming / BDI semantics (role: aop)

Scope: sections 3, 5.3-5.5, 6.1, 10, 11 of `design/vagent-agent-language.md`. Code evidence checked
in `mesh_actor.cpp`, `arena_manifold.hpp`, `vicon-goal-directed-language.md` §4.3.

Summary: the document delivers a reactive SARL core and a backtracking-search planner. It does not
yet deliver BDI. There is no belief base with revision, no desire/goal-adoption semantics, no
intention-selection function, no commitment strategy, and two of the central mechanisms (cross-turn
speculative intentions, concurrent intentions in one arena) contradict the substrate they cite.

## 1. BLOCKING - 10.1/10.4/7.3: cross-turn intentions contradict the checkpoint invariant

Problem. A suspended intention is claimed to survive turns with `choiceDepth()==0` and
`outstandingMarks()==0`. But an intention that has executed `<-` (10.3) or sits inside `P1 | P2` (an
unresolved alternative) holds a Mark and a trail by definition; "reifying" it into `+d.intentions`
without the Mark means its `<-` writes are no longer undoable (vicon §4.3 lines 247-250:
reversibility exists only while the Mark is live) and its remaining alternatives are gone. The text
says micro-deliberation stays within one turn, yet 10.3 has `query`/`ask` suspend the intention
until a reply arrives (10.3, "inquiring intention suspends"), and 10.1's rover example `drive(loc)`
is a physical action that realistically spans turns.

Evidence. `mesh_actor.cpp:670-674` rejects checkpoint on `choiceDepth()!=0`,
`outstandingMarks()!=0`, non-empty mailbox, or any incomplete delivery from the pid
(`mesh_actor.cpp:676-680`). A pending `query` is an incomplete delivery, so the agent is
non-checkpointable for exactly as long as an intention is waiting, and a busy agent whose mailbox is
never empty is never checkpointable (the "Ingestion Gate" in 7.3 is invented; grep finds no such
thing in `mesh_actor.*`). `ActorContinuation` (7.3) contains only `completed` request records: it
does not capture the intention graph, so "resumes seamlessly" does not hold unless the intention
graph is promoted to the Store, which is never stated.

Fix. State a rule: an intention that suspends across a turn boundary must first (a) be at a point
with no live Mark, meaning all `<-` have been committed or undone, and (b) have its plan-frame graph
(`d.intentions`, `d.subgoal`, `d.step` position, remaining `d.choice` alternatives encoded as data,
not as VM choice points) promoted through `promote()` so the continuation can rebuild it. Say
explicitly that alternatives are *re-derived* on resume by re-running plan selection, not resumed.
Add a section "Suspension points" listing which statements may suspend (`query`, `ask`, `wait`) and
require them to be outside any speculative region (compile-time error otherwise).

## 2. BLOCKING - 10.3/10.4: concurrent intentions share one arena whose Marks are LIFO …

Full title: 2. BLOCKING - 10.3/10.4: concurrent intentions share one arena whose Marks are LIFO;
`ArenaSlice` does not exist

Problem. Each intention is said to run in "a private ephemeral `ArenaSlice`" whose watermark resets
independently. No such type exists (`grep ArenaSlice apps include src design` hits only this
document). `ArenaManifold::mark/release` is a single stack-disciplined truncation
(`arena_manifold.hpp:166-167`: "every slot above the mark is discarded"), so interleaved intentions
A and B cannot release A's mark without discarding B's later allocations. The claim in 4.1 that
nothing new is asked of the C++ core is therefore false for 10.3/10.4.

Fix. Either (a) specify that intentions are *time-sliced cooperatively with no Mark live across a
slice boundary* (consistent with issue 1) so Marks never interleave, and drop `ArenaSlice`; or (b)
give each intention its own `ArenaManifold` over the agent arena as `base` (the copy-on-write view
the header already supports, `arena_manifold.hpp:268`) and price that in cells. State which, and
remove 4.1's "asks nothing" claim if (b) needs new C++.

## 3. BLOCKING - 3.1, 10 throughout: no belief base, no belief revision, no belief-triggered events

Problem. "BDI" is claimed (10 intro, 11.2) but beliefs are only agent fields (`let mut`) and
`self.x <- v`. There is no belief literal syntax, no query over beliefs (`believes`), no belief
add/delete events (AgentSpeak `+b`/`-b`), no revision policy for conflicting percepts, no closed
world vs open world statement, no provenance of belief source (percept / told / self). Plans are
therefore not triggered by belief change, and `where C` applicability (5.5) has nothing defined to
range over.

Fix. Add a "Beliefs" subsection: a belief is a cell (or small cell cluster) on a new `d.belief` rank
keyed by functor; `+b`/`-b` produce typed events into the agent's own `d.event` ring (internal
events, distinct from percepts); revision is "last writer wins per key, old value retained in
hypertime" (the spool gives AGM-style contraction history for free; say so and cite R8 limits). Add
`believes P` / `where believes P` as a goal-directed generator over `d.belief` using Vlog `unify()`.
Add belief-source annotation as a cell on `d.source`-like dimension, not a tag.

## 4. BLOCKING - 3.1, 5.5, 6.1: `goal` / `plan` / `achieve` are conflated and goal …

Full title: 4. BLOCKING - 3.1, 5.5, 6.1: `goal` / `plan` / `achieve` are conflated and goal kinds
are undefined

Problem. `GoalDecl ::= 'goal' Identifier '(' ParamList ')' Block` gives a goal a *body*, while 3.1
item 9 says a goal is "a declarative specification" with `maintain`/`ensure` contracts, and item 10
says a plan satisfies a goal. In the rover example `goal SurveyTarget` has a body of imperative code
and a plan-like `achieve A & B else {...}`; `achieve TraverseToTarget(loc)` names a *plan*, not a
goal, so plan-vs-goal dispatch (how `achieve G` finds candidate plans for `G`) is undefined.
`plan TraverseToTarget` is itself `PlanDirectPath | PlanRidgeBypass`, i.e. a plan that is a choice
of plans, which bypasses plan selection entirely. Nothing syntax-level distinguishes the goal types:
achieve (drop on success), maintain (re-adopt on violation), perform (no success test), query,
cease, avoid (GOAL/Jadex/Tropos). `maintain`/`ensure` appear only in prose; there is no grammar
production for them (grep `maintain` in 5.5: absent).

Fix. Define: `goal G(params) { ensure Expr; maintain Expr; drop_when Expr }` (declarative, no
imperative body) and `plan P for G(params) [where Expr] { Block }` (procedural, bound to a goal by
name; many plans per goal). `achieve G(args)` = adopt goal, select an applicable plan from the
`for G` set on `d.choice`. Add the goal-kind keyword (`achieve|maintain|perform|query`) with a table
giving success test, failure condition, and re-adoption rule for each. Rewrite the rover example so
`TraverseToTarget` is a goal and `PlanDirectPath`/`PlanRidgeBypass` are plans `for` it.

## 5. BLOCKING - 10.1: no intention selection, deliberation, or commitment semantics

Problem. The four-phase "BDI turn cycle" names perception, preemption, a slice, and quiescence but
specifies none of: (a) the event-selection function S_E (FIFO? by priority?), (b) the
option-selection function S_O (which applicable plan: first in textual order is implied by `|`, but
plan-library plans for one goal are not ordered), (c) the intention-selection function S_I ("the
active intention" in singular: with N intentions on `d.intentions`, which gets the quota?), (d) how
many instructions each gets, (e) the commitment strategy. Without these the language has no
reconsideration policy; "re-verifies its goal invariants" (10.4 item 6) is the only hint and covers
maintain-goals on resume only. `CRITICAL_THRESHOLD` (10.1) is an undefined name; neither
`mesh_actor.hpp` nor the VM has event priorities (grep `priority` in `mesh_actor.hpp`: no hits), so
`with priority` is a new scheduler, contradicting "none" in 1.4. Also "Reactive preemption ...
executes immediately": immediately where, in the same turn, before the quota, mid-slice? Intention
interruption points are not defined.

Fix. Add "10.x The deliberation cycle" with a pseudo-code loop and three named, overridable policies
(all as Vortex routines reading `system://settings`-style cells, no C++): `selectEvent`,
`selectPlan`, `selectIntention` (default round-robin with priority = max of triggering event
priority and goal priority). State the commitment strategy per goal: default **single-minded**
(commit to the intention until it succeeds, the goal's `drop_when` holds, or no plan remains
applicable; reconsider only at slice boundaries and on belief-change events touching the plan's
`where` clause). Offer `blind` and `open` as a goal modifier. Define the preemption point:
instruction-quota exhaustion or a `yield` boundary, never mid-opcode. Either build `priority` into
the mailbox ordering by a documented dimension (`d.priority` rank, not bit-packing) or admit it is
new machinery.

## 6. MAJOR - 10.2/10.3/6.1: `commit;` conflates three distinct things

Problem. `commit;` = `#Cut` = prune alternatives = make `<-` permanent = flush outbox. These differ.
Per vicon §4.3 (lines 240-250) a cut keeps bindings but they are still arena-ephemeral; making state
*durable* is `promote()` (4.3), at event end. Making a `<-` write irreversible after `commit;` while
an enclosing outer `|` choice point is still live is unsound: if the outer alternative later fails,
the "permanent" write must be undone, or else backtracking leaves partial state. The rover example
does exactly this: `PlanDirectPath` commits the energy deduction, then `drive(loc)` fails, `|` falls
to `PlanRidgeBypass`, and the 15.0 deduction stays, which is wrong for a "reserve estimate" and is a
documented pitfall of Prolog cut + side effects. The same plan then has `commit;` before the
physical action `drive`, so a failed `drive` cannot roll back the already-flushed `emit`s.

Fix. Split: `commit` (cut: discard alternatives of the *innermost* goal only), `flush` (release the
outbox, only legal when no enclosing Mark remains, compile-time checked), durable state is only via
end-of-turn `promote()`. Define `commit` as local to the enclosing plan; do not let it escape an
outer `|`. Fix the rover so deductions happen after `drive` succeeds, or model the reservation as
`<-` left uncommitted until the goal achieves.

## 7. MAJOR - 10.2/6.1/10.4: plan failure and goal-failure propagation are under-specified

Problem. Three failure notions are mixed: generator exhaustion (soft), `#Trap`/contract violation
(hard), and plan *execution* failure after a physical action has occurred. BDI needs the third: a
plan failed after side effects, so backtracking is impossible and failure handling must be forward
(compensate/retry/replan). Missing: (a) what `achieve G else {B}` does if `B` itself fails; (b)
failure reason delivered to the else-block (JACK/Jason `-!g` failure-goal handler gets the cause);
(c) whether failed plan is retried (Jason retries other applicable plans only if the event is
re-posted; the doc implicitly retries via `|`, but plan-library alternatives for one goal have no
ordering); (d) what the parent goal sees when a subgoal fails (does a failed `achieve Sub` propagate
failure up to the enclosing plan, as in Jason, or does it just fail the expression, so `&`
backtracks into earlier steps that already actuated?). Also `#Trap` escalates `IntentionFailed` to
"the enclosing holon supervisor" but 6.5 and 10.4 name a different event (`HolonGoalFailed`) emitted
for plan exhaustion, with different routes (`-d.holon` vs `-d.context`). Both events are undeclared
`event` types.

Fix. Define a failure taxonomy table: `NoApplicablePlan`, `PlanFailed(cause)`, `GoalDropped`,
`TrapFailed`. Add `else` binding syntax `else (cause) { ... }`. Make subgoal failure propagate as
plan failure to the parent by default. Specify that an agent that has actuated (`flush`/capacity
call tagged `actuating`) leaves the backtrackable region, so failure thereafter is *forward*
(handler), enforced by a compile-time check. Unify the two failure events into one declared
`IntentionFailed { goal, cause }` with a single documented route, and declare it in `std:agent`.

## 8. MAJOR - 10.3: epistemic query bypass of the outbox leaks speculation and is not …

Full title: 8. MAJOR - 10.3: epistemic query bypass of the outbox leaks speculation and is not
read-only in practice

Problem. `query`/`ask` are sent immediately from inside a speculative branch. (a) If the branch is
later rolled back, the query has still been sent and its response (arriving in a later turn) has no
continuation to land in; the suspended intention may have been aborted. (b) "Strictly read-only ...
no external side effects in the recipient" is unenforceable: the recipient's handler for a query
event is ordinary user code. (c) 11.1 item 2 says synchronous blocking cycles cannot be constructed,
but "intention suspends until ActorDeliverValue arrives" plus two agents querying each other with
mailboxes full is a wait-for cycle at the intention level; the agent as a whole is not blocked, but
those two intentions are, so deadlock freedom holds only for the agent, not for the goals. No
timeout is specified.

Fix. Give `query` a mandatory timeout (`query X within 500ms else ...`, the value a cell in
`system://settings`) and a cancellation semantic (response with unknown `correlationId` is dropped
and counted). Make query handlers a distinct declaration (`on query Q(...) -> Result`) restricted by
the compiler to the read-only subset (no `emit`, no `<-`, no capacity calls tagged `actuating`).
Reword 11.1 to claim agent-level non-blocking and intention-level liveness only via timeouts.

## 9. MAJOR - 5.3/6.4/10.1: event handlers, plans and intentions: no relation between …

Full title: 9. MAJOR - 5.3/6.4/10.1: event handlers, plans and intentions: no relation between `on`
and goal adoption, and no per-event atomicity

Problem. `on MissionCommand ... { achieve SurveyTarget(...) }` is the only bridge. Is `achieve`
inside a handler blocking (handler waits for the goal) or asynchronous (adopt, return)? If blocking,
a long goal starves reactive dispatch inside the same turn; if asynchronous,
`achieve ... else {...}` there has no meaningful continuation. Handlers are also not ordered with
respect to intentions: 6.4 says matching handlers are *sorted by priority and the block executed*
(all of them? the first? Jason runs one plan per event; SARL runs all matching behaviors in
parallel). 5.3 (`TargetDetected` in the Drone plus `ObstacleAvoidance` behavior) is ambiguous. 6.4
also runs the guard in the mark region then, on success, "promote()" per event, so a handler that
adopts a goal promotes before the goal has run.

Fix. Rule: `achieve` in a handler is always asynchronous (adopts and returns); `achieve ... else` is
only legal inside plans. Rule: all handlers whose pattern and guard match fire, in descending
priority then declaration order, each in its own Mark region (SARL semantics, stated), and a handler
may `consume` the event to stop lower ones (Jason-like). Add internal event types (`GoalAdopted`,
`GoalAchieved`, `GoalFailed`, `BeliefAdded`, `BeliefRemoved`) so behaviors can react to the agent's
own deliberation.

## 10. MAJOR - 10.3 CoW shadow cells: semantics of `<-` become contradictory

Problem. Section 10.3 gives two incompatible implementations of `<-`: 6.1 says it writes the new
payload via `value()` and appends the *previous* value to the trail (the vicon mechanism), while the
CoW passage says it does *not* mutate the master and writes a shadow cell, resolving reads through
it. Under the first, a peer intention reads the speculative value (dirty read) the moment it is
written; under the second, `commit;` "writes the shadow into the master", which is a `value()` write
by a different intention than the others have read from, i.e. write-write conflict resolved by last
committer with no detection (a lost update on `energyReserve -= 15`). No isolation level is stated.

Fix. Pick one. Recommended: since only one intention runs at a time per agent (single-threaded
cooperative, see issue 2), `<-` uses vicon's in-place trail and no Mark may be live across a slice;
the shadow-cell machinery is deleted. If real interleaving is wanted, name the isolation (snapshot
isolation) and add a write-set conflict check at `commit;` that fails the plan (soft) when the
master changed since the shadow was taken.

## 11. MAJOR - 3.1/10: interaction protocols and performatives are absent

Problem. The brief claims "performative messages" (10.3 `flush staged performative messages`) but
the language has `emit Event` only: no speech-act vocabulary (`tell`, `ask`, `achieve`, `cfp`,
`propose`, `accept`), no semantic of receipt (a told fact becomes a belief? an `achieve` request
adopts a goal?), no conversation identifier/protocol state machine, no FIPA/Contract-Net, no
treatment of unreliable delivery (`ActorDown`, timeouts) in protocols. The 1.3 comparison asserts
agents differ from actors in cognitive stance, but with events-only communication a vagent is an
actor with guards.

Fix. Add "Performatives": a closed set (`tell`, `untell`, `ask`, `achieve`, `propose`, `accept`,
`reject`, `inform-failure`) as built-in event kinds whose default `on` handlers (in `std:agent`,
Vortex) implement the receipt semantics (`tell` -> belief add with source = sender; `achieve` ->
goal adoption subject to a `trusts` capacity). Add a `protocol` declaration (role-indexed state
machine as cells on a `d.protocol` rank with `conversationId`), with Contract-Net as the worked
example replacing or alongside 9.1. Cite which `MessageType` carries each.

## 12. MAJOR - 10.4/11: holonic failure propagation and supervision semantics are inconsistent

Problem. 6.5 says the sub-agent "automatically emits `HolonGoalFailed` Merkle event up to
`-d.context`, unblocking the super-agent's reactive dispatcher and preventing collective deadlocks":
a reactive dispatcher that is blocked by a child is exactly what 11.1 says cannot exist. 10.4 says
the event goes along `-d.holon`; 6.5 says `-d.context`; 10.2 says escalate to the "supervisor".
Delegation itself (how a super-agent assigns a goal to a child, how acceptance and result return) is
not specified, and nothing says what happens when the *child agent* terminates mid-goal
(`ActorDown`) as opposed to the goal failing.

Fix. One failure route (see issue 7), declared delegation syntax (`delegate G(args) to child`
mapping to the `achieve` performative plus a conversation id), and a stated child-death rule
(`ActorDown` -> `GoalFailed(cause=ChildDown)` into the delegator's intention).

## 13. MINOR - 5.5 grammar and 10.2 operator precedence break goal-directed semantics

- `commit` appears in `PrimaryExpr`, `CommitStmt` and in `a & commit; & b` (10.3 example): the `;`
  inside a conjunction is not derivable (`Expr` does not contain `;`).
  `check_feasibility(dest) & commit; & navigate_to(dest)` does not parse.
- `ReversibleExpr` sits *above* alternation in precedence: `a | b <- c` parses as `a | (b <- c)` is
  not guaranteed; also `=` is lowest, so `x = a | b` assigns a generator. Icon makes `:=` a
  non-generator-capturing operator; state which results are bound.
- `ComparisonExpr` binds tighter than `&`, matching Icon, but `if`/`while` take `Expr` and are
  boolean-tested while plan bodies use success/failure; no rule says when `Expr` is a condition vs a
  generator. `where Expr` is "soft" but is parsed as an ordinary Expr.
- `achieve Expr [else Block]` as both a statement and (in `goal SurveyTarget`) the left of `&` with
  `else` binding to the whole conjunction; ambiguity of where `else` attaches.
- `on Event(p1,...)` patterns bind by position, but 5.2 events declare named fields; `HandlerDef`
  `Pattern` is not a destructuring of the declared fields. `from Expr` is not in 5.3's rules.
- `abort intention` has an optional `Expr` target; `suspend`/`resume` have no lowering row in 6.1.

Fix. Add a production `ConjStep ::= Expr | 'commit'` so `commit` is a `CommitExpr` inside
conjunction; add an explicit precedence table; give `where`/`ensure`/`maintain` a distinct
`Condition` nonterminal that is evaluated as bounded (first solution, bindings kept, no retry); add
lowering rows for `suspend` and `resume`.

## 14. MINOR - 10.1 diagram and 10.4 text disagree on the order of recovery

The 10.1 diagram rolls back to Plan B on soft failure, while 6.1's `achieve` row says "on first
success drops choice points via `#Cut`". Together, a goal with plans [A, B] whose A *succeeds*
cannot later retry B when a *subsequent* conjunct fails (`achieve Traverse(...) & Collect(...)` in
the rover): the achieve cut already dropped B. So the `SurveyTarget` goal cannot fall back to the
ridge route if sampling fails after a successful direct path, which is probably the intent of
"backtrack". State whether `achieve` is bounded (Icon `!`-style, committed) or retriable and
document the consequence; the rover comment "or backtrack to ridge bypass" is true only for failure
inside `drive`.

## 15. MINOR - 11.2 comparison matrix overclaims

"Dual Reactive + BDI Goals" lists SARL as "Pure Reactive Handlers" though SARL has no built-in BDI
but Janus/SARL BDI extensions and JaCaMo/Jason, Jadex, JACK, GOAL, 2APL are the comparison class for
a BDI claim. The matrix compares to actor frameworks only. Add AgentSpeak(L)/Jason and GOAL columns
and rows for: belief base, declarative goals, plan library, commitment strategy, and failure
handling; mark vagent cells "not yet" where issues 3-7 stand.

## 16. MINOR - 11.1 soundness claims are asserted, not argued

"Memory corruption or data races structurally impossible", "deadlock freedom", "deterministic
replay" are stated in three lines. Determinism fails with `with priority` ties, wall-clock
`expiresAt` in `ActorGrant`, `query` timeouts, and message arrival order across a swarm; replay of
an agent's *lifetime* requires the inbound messages to be logged, which the spool does not do (spool
holds committed state, not mailbox history; I did not find message logging in `mesh_actor.*`). Fix:
restrict the claim to "replay from the spool of committed operations reproduces the committed
state", and add inbound-message logging as a cell rank if full replay is wanted.

VERDICT: NOT CONVERGED
