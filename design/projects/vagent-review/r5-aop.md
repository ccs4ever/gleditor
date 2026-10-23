# Round 5 critique: agent-oriented programming / BDI semantics (role: aop)

Document: `design/vagent-agent-language.md` v3.2. I read §3 (V1-V13 as cited), §4.2, §5.3, §5.6, §6
in full, §7.1-7.3, §8.2, §8.6, §9.1, §10, §11, §12.3, §12.4, §14, §15 and the appendix. I
spot-checked `apps/common/xanadu/vortex/mesh_actor.cpp` (`send`, `resume`, the reply path) for the
P2/P4 justifications; the Vortex and mesh lanes belong to other critics and I did not verify P1-P9
beyond that.

## Status of round-4 BLOCKING/MAJOR findings

| R4  | Topic                                        | Status                                                                                                                                                                                                                        |
| --- | -------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | `drop` of a dead id; safety handler undone   | RESOLVED (§8.6 no-op success; `intentions()` top-level only; §6.4 dropping a subgoal fails the parent's `achieve`; §12.3 step 8 says Survey's `else` does not run). Residue: whether the handler's effects work at all, see 1 |
| 2   | end-of-turn events after `d.pending` drained | RESOLVED (§4.2 step 6 and §6.5 settle loop, bounded by `belief_event_depth`, hard failure on overrun)                                                                                                                         |
| 3   | what an `await` captures; guard scope        | RESOLVED (§5.6 `waiting` holds the match, §6.6 copy at match time, §6.4 guards isolated with the intention's frame)                                                                                                           |
| 4   | `revise` has no key; source conflicts        | RESOLVED (`key` parameters, keyed replacement per source, `believes ... from self`, `revise` removes only `self`)                                                                                                             |

Round-4 minors 5-10 are addressed in the text (restorer success is `holds`; the rover adopts on the
first `Battery`; `recharge` awaits with a guard; `ConditionFault` once; `$cancelled`; drop at a
boundary; effect deadline stages `cancel`; no-applicable-plan fails at once). Item 5(d) and 5(b) are
addressed, but the back-off rule that resulted has a defect, see 2.

## New and remaining findings

### 1. MAJOR - 12.3 step 8, 5.3, 6.4: the emergency-stop handler is not shown to stop …

Full title: 1. MAJOR - 12.3 step 8, 5.3, 6.4: the emergency-stop handler is not shown to stop
anything, because `halt()` shares a device with the effect it must interrupt

Problem. The hazard handler stages `halt()` and then drops the intentions. Capacity `Mobility` has
`follow` (10 min, in flight), `halt` and `dock`, all on the one `Chassis` binding. §5.3 says a
device shared by several intentions "is serialised or refused (`EffectFailed{cause: "busy"}`) by its
binding". `halt()` is called from a handler while `follow` is running, so under the stated host
contract it is either queued behind a 10-minute drive or refused as busy. The `cancel` of `follow`
that the drop stages is drained after `halt` (outbox order is emit order: `halt`, then the drops'
cancels; §4.2 step 9), so `halt` meets the device while the drive is still on it. The text of step 8
asserts "`halt()` is staged, ... Dropping `Survey` ... stages the chassis's `cancel`" and never says
the halt takes effect. This is the document's one safety example of "a handler can drop an
intention" (§6.5) and it works only if the binding treats `halt` as pre-emptive, which no rule of
§5.3 says. A second hole of the same kind: the maintain goal is kept, so a restorer already running
`dock()` (or one adopted at the next turn end, because `holds` may fail while stopped) continues to
drive after the emergency; the handler neither drops it nor blocks it (`blocked_until` is not set by
a drop of a goal the handler leaves alone).

Evidence. §5.3 host contract; §12.3 lines for `on HazardDetected` and step 8; §4.2 step 9 (drain
order); §6.4 "stages the host binding's `cancel`".

Fix. (a) Add to the host contract: a capacity method may be declared `urgent` (or
`effect def halt() within 5s preempts`), and a binding receives an urgent call ahead of, and
cancelling, any call in flight on the same device; the checker allows `urgent` only on effect
methods. Otherwise reorder the handler so the drops (and their `cancel`s) are staged before `halt()`
and say that the binding applies `cancel` before the next call. (b) In step 8, say what happens to a
restorer in flight: either the handler also drops restorers (`intentions()` is top-level, so give
`std:agent` `restorer_of(i)` or let the handler drop the maintain goal and re-adopt it on
`EmergencyCleared`), or state that docking may proceed and why that is acceptable.

### 2. MAJOR - 6.3, 12.3 step 7: back-off is cleared by the very belief event that every …

Full title: 2. MAJOR - 6.3, 12.3 step 7: back-off is cleared by the very belief event that every
percept raises

Problem. §6.3: "A restorer that fails or is dropped sets `blocked_until` ... a belief event on a
predicate in `holds` clears it." For `KeepCharged`, `holds` reads `battery`. `on Battery` does
`revise battery(...)`, which raises events whenever the level changes, and a battery level changes
continuously (and a driver re-reports periodically, §7.5). So after a failed `dock()` the next
percept clears the back-off and the next turn end adopts a new restorer, which stages another
`dock()` effect. The back-off the setting `vagent.maintain_backoff` (30 s) names never holds against
the one predicate it is about. §12.3 step 7 repeats it ("re-adoption waits for `maintain_backoff` or
the next `battery` change"), so the rover thrashes at the percept rate when docking fails. Clearing
on a belief event is meant for "something changed that might make the plan applicable", but the
goal's own watched predicate changes for reasons unrelated to the restorer's applicability.

Fix. Clear `blocked_until` only on a belief event for a predicate read by the restorer plans'
`where` clauses and not by `holds` (for `KeepCharged` that is none, so the back-off runs its time),
or drop the clearing rule and keep a pure timer. If the owner wants event-driven retry, make it a
declared clause on the goal (`retry when believes ...`). State the choice in §6.3, correct §12.3
step 7, and say whether back-off is a constant or doubles with consecutive failures (a constant
makes a permanently broken dock retry forever every 30 s, which is a ceiling-free but wasteful loop;
say so in §15).

### 3. MAJOR - 11.2, 9.1, 12.3: the flagship example depends on P1, but P1's "Without …

Full title: 3. MAJOR - 11.2, 9.1, 12.3: the flagship example depends on P1, but P1's "Without it"
and "Used by" do not say so

Problem. P1's fallback is "methods may not be called from speculative positions, so `f(x) & g(y)`
with methods `f` and `g` is a compile-time error". The `travel` plan in §12.3 is

```text
(budget <- level - cost(direct(here, location)) & budget > reserve_floor & direct(here, location)) | ...
```

`cost`, `direct` and `ridge` are `Routes` capacity methods, called as operands of `&` and `|`.
Without P1 the reversible-choice example, which is the document's demonstration of V2/V13, is
rejected by the checker. The same restriction would apply to every condition that calls a pure `def`
next to another operand (§8.2 allows `def` calls in guards, `where` and `succeed when`). The
register's "Used by" column for P1 says "V13, §9.1" only, and §14 properties that cite
statement-level choice points (the failure semantics of §6.7) hold only with P1. The owner's rule
requires each dependency to be cited with a stated fallback; here the fallback exists but silently
invalidates a shipped example.

Fix. Add §12.3 and §8.2 to P1's "Used by". In the "Without it" cell say that §12.3's `travel` and
any guard combining a method call with another operand would not compile, and give the fallback form
(bind the calls first: `let c := cost(direct(here, location))` outside the speculative expression,
with the speculation reduced to comparisons). Mark the example "requires P1" in §12.3's lead-in.
Also add a line under §14 naming P1 and P4 as conditions of "A turn is atomic" and "Every wait is
bounded".

### 4. MINOR - 11.2 P4: "Without it" understates a total loss, and the stated …

Full title: 4. MINOR - 11.2 P4: "Without it" understates a total loss, and the stated impossibility
is not shown

P4's "Without it" reads "No intention runs until a mailbox message arrives". That removes every
wake: ready intentions, timers, effect results and percepts (the whole of §6.6, and §14's "every
wait is bounded"). Say so plainly: without P4 the agent model is reactive-only. The justification
also says the adapter "cannot simulate a wake with a message" because a self-sent message collides
with the actor's `remembered` entry. That reasoning covers a message whose source is the actor
itself; the adapter installs its own loopback `Sender` (§11.1) and could inject a wake as an
`ActorSend` from an adapter-owned source name with a fresh request id. I could not verify from
`mesh_actor.cpp` whether that is refused (the checks I read, `send` at 494-530 and `resume` at
205-260, do not show it). Either show the refusal (cite the line) or record the pacemaker-source
message as the fallback; it costs one extra reply turn per wake, which §15 can price, and would make
P4 an optimisation and not a prerequisite.

### 5. MINOR - 11.2 prerequisites: no fallback for the two prerequisite documents, one …

Full title: 5. MINOR - 11.2 prerequisites: no fallback for the two prerequisite documents, one of
which changes existing stdlib

The header and the note under the register say Vlog §6 and vicon §6's scanning functions are
approved in their own documents. `std:goal` is existing code (the header says it registers only
`scan`, `subject` and `pos` today), so registering `tab`, `move`, `find`, `many`, `upto`, `match`,
`any`, `bal` is a change to the existing standard library on which §12.3 depends
(`reading ? (tab(match("SPEC:")) & tab(upto(";")))`). Say in one sentence what vagent does without
each: without the clause database the belief base has no representation (V8) and the language does
not run; without the scanning functions only the `reading ?` line of §12.3 is lost, and the example
can parse with `std:string` functions. Cite the sibling's section that carries the approval.

### 6. MINOR - 12.4: the example's two claims are wrong or hazardous

(a) "`courier` is `Opt<Agent>`, so if the spawn failed the `achieve` statement fails at once." The
assignment `courier := spawn Courier() in inner` stores the name immediately (§8.6: "the name is
known at once"); a refused spawn is reported later by `SpawnFailed` and fails the staged deliveries
(§8.6 note). So `courier` is not `none` after a failure, and the statement fails when the delivery
is abandoned, not at once. Reword: "if the spawn fails, the adapter fails the delivery and `achieve`
fails with `DeliveryFailed`." (b) `await delivered(item, count)` tests the base first (§6.6), and
nothing forgets the fact. A second `Order` for the same item and count is satisfied by the first
delivery's stale fact before the courier acts. Either forget the fact before `achieve`
(`forget delivered(item, count)`), or key `delivered` by a per-order id. It is a good illustration
of why term awaits see old facts, so say so in the commentary.

### 7. MINOR - 6.2, 12.3: maintain `holds` and `where` read facts of any source

`holds believes battery(?level) & level > floor` yields every source's value, and `&` succeeds if
some value qualifies (§6.1, §8.2). A peer's `tell battery(100)` therefore satisfies the maintain
goal while the rover's own level is low. §6.1 introduces `believes t from self` for this and the
examples do not use it. Use `believes battery(?level) from self` in `KeepCharged` and `travel`, or
add one sentence to §6.2 that goal clauses over percept-fed predicates should say `from self`.

### 8. MINOR - 6.7: "its own writes" for compound statements needs one sentence

The table says a failed statement's "own writes and staged entries are undone". §12.3 step 6 depends
on `emit SurveyFailed{...}` inside the `else` block of `achieve ... else { ...; fail }` standing
after `fail`, though that block is part of the `achieve` statement, which has failed. That holds
because a plan's SFAIL is a `#Jump` and not a backtrack (§9.1), so only the innermost failing
statement's trail is undone, and the enclosing statements' choice points must then be cut by the
plan-failure path. Say so in §6.7 ("own" means the innermost statement; enclosing statements'
earlier writes stand when a nested statement fails in a plan; in a handler or method the failure
backtracks through every enclosing statement), so a reader does not conclude the `emit` is undone.

### 9. MINOR - 6.3, 6.5: a maintain goal intention receives `SubgoalEnded` it never uses

A restorer's end marks its parent ready with `SubgoalEnded` (§6.5, `continue`). A maintain goal
intention "never runs a plan itself", so the ready mark makes it take a wake turn for nothing after
every restorer, and `continue(i)` on it has no defined behaviour (it has no plan, and "if i has no
plan: select one" would select a plan for the maintain goal itself, contradicting §6.3). Say that
`continue` on a maintain intention only runs the checks (`drop when`) and clears `ready`, and that
ending a restorer marks the parent ready only when the parent is not a maintain goal.

## Registry check (new §11.2), within my lane

All BDI machinery (goals, plans, intention scheduling, beliefs) sits in `std:agent` and the adapter,
and needs P1 (statement choice points; see 3), P4 (wakes; see 4) and P9 (module registration). The
other proposals (P2, P3, P5-P8) are runtime resource changes with no effect on the agent model. I
found no BDI behaviour that depends on an unlisted change to existing code, apart from the
`std:goal` scanning registration (5). Every dependency has a number and a "Without it" cell, but the
P1 and P4 cells understate what is lost (3, 4).

## Assessment

The agent model is sound and the round-4 gaps are closed: goal kinds, single-minded commitment, plan
failure with forward staged effects, wait states with captured matches, maintain goals as permanent
intentions with restorers, keyed beliefs with sources. What remains is in the worked example and the
register: the safety handler does not demonstrate a working emergency stop (device contention with
`halt`), the maintain back-off is defeated by its own clearing rule, and the shipped §12.3 example
silently requires P1. Each has a local fix; none needs new mechanism.

VERDICT: NOT CONVERGED
