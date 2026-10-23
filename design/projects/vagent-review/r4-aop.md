# Round 4 critique: agent-oriented programming / BDI semantics (role: aop)

Document: `design/vagent-agent-language.md` v3.1. This round I checked the document against its own
text (cross-section consistency of §4.2, §6, §8, §9, §12); the code citations I had verified in
round 3 are unchanged and I did not re-verify the others (Vortex/mesh lanes belong to other
critics).

## Status of round-3 BLOCKING/MAJOR findings

| R3  | Topic                                                          | Status                                                                                                                                       |
| --- | -------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | end-of-turn conditions can abort the turn                      | RESOLVED (§6.4 isolation, fuel slice, `ConditionFault`). Residue: ordering of the raised events, see 2; repeated fault events, see 6         |
| 2   | achieve ends while effect in flight; drop cannot cancel effect | RESOLVED (§6.4 exemption, `cancel` staged on drop). Residue: elapsed effect deadline, see 9                                                  |
| 3   | plan-segment rollback stated two ways                          | RESOLVED (§6.7 last paragraph, per-statement semantics, consistent with §12.3)                                                               |
| 4   | maintain goal representation                                   | PARTLY: goal intention + restorer subgoal is now specified; the restorer's success test and the start-up behaviour of `holds` are not, see 5 |

Round-3 minors 5-12 are addressed in the text (remote_fault, handler `EffectFailed`, in-flight
effect blocks checkpoint, await edges, performative rows, `revise`, device contention, rover awaits
the position percept). `revise` is addressed only in name; see 4.

## New and remaining findings

### 1. MAJOR - 6.5, 8.6, 12.3: `drop` of an ended intention is unspecified, and in the …

Full title: 1. MAJOR - 6.5, 8.6, 12.3: `drop` of an ended intention is unspecified, and in the
hazard handler it undoes the emergency halt

Problem. `intentions()` walks subgoal chains (§5.6), so it returns `Survey` and its subgoal `Reach`.
The handler in §12.3 iterates that list and calls `drop i`. Dropping `Survey` drops `Reach` (§6.4),
so the later `drop Reach` names an intention that has already ended. `Intention` is a scalar id
(V11), so this is a lookup that finds nothing. The text defines the failure of an `Opt<T>` read
(§8.2) but not of `drop` on a dead id. If it fails, the `for` fails (§8.2: "the loop fails if `body`
fails"), the handler fails, and §6.7 says a failed handler is all-or-nothing: `halt()`, every drop
and `EmergencyHalted` are released. The safety handler then does nothing in exactly the case it
exists for (a drive in progress, `Survey` first in list order). If it does not fail, the text must
say so.

Evidence. §5.6 (`intentions()` follows subgoal chains), §6.4 (drop drops subgoals), §6.7 handler
failure row, §8.2 `for` bullet, §12.3 hazard handler, §10 `intentions()`.

Fix. State in §8.6: `drop i` on an id that is not live succeeds with `()` and does nothing (an
intention can end between listing and dropping, including by the same loop). Also make
`intentions()` return only the intentions a drop would not already cover, or say it returns subgoals
too and that is why `drop` is idempotent. Add a worked check to §12.3 step 8 that dropping `Survey`
and then `Reach` is harmless. Decide and state what dropping a subgoal alone does to its parent
(`SubgoalEnded` with status dropped, so the parent's `achieve` fails and its `else` runs: that would
emit `SurveyFailed` during an emergency; say whether that is intended).

### 2. MAJOR - 4.2 step 5, 6.5: end-of-turn deliberation raises events after `d.pending` was drained

Problem. The cycle in §6.5 drains `d.pending` and then runs "reconsider every live intention; check
maintain goals". Reconsideration ends intentions and raises `GoalAchieved`, `GoalFailed`,
`GoalDropped`, `ConditionFault`, and adopts restorers, so it appends to `d.pending` after the
dispatch loop has finished. §4.3 and §5.1 say `d.pending` is "emptied before the turn ends" and "no
(always empty)" in the checkpoint root, and §14 relies on a quiescent agent. As written either the
events are dropped (a `GoalAchieved` handler never runs for a goal ended by `succeed when`, which is
the commonest way an `achieve` goal ends) or the invariant is false. Conversely a handler run by
those events can change beliefs, which can make another intention's `succeed when` or a `holds` true
or false, and nothing reconsiders again.

Evidence. §6.5 pseudocode order (`while d.pending ...` before `reconsider`), §4.3 row "Internal
events", §6.4 "ending i ... raise GoalAchieved".

Fix. Make the end of the turn a bounded fixpoint: repeat (drain `d.pending`; reconsider) until
reconsideration changes nothing, stopping at `vagent.belief_event_depth` rounds with the existing
hard-failure rule. Say that rounds count both loops together, and rewrite the §6.5 block and §4.2
step 5 accordingly. Price it in §15 ("per-turn deliberation" is then a multiple of rounds).

### 3. MAJOR - 6.5, 6.6: what an `await` captures when it matches is unspecified, and …

Full title: 3. MAJOR - 6.5, 6.6: what an `await` captures when it matches is unspecified, and the
match runs without the waiting intention's scope

Problem. Two defects of one mechanism. (a) The dispatcher marks an intention ready when its `await`
matches (§6.5), but ready intentions run one per wake turn, later (§6.5, `ready_per_message`). The
event cell is unlinked at step 8 of the matching turn, and a matched fact may be retracted before
the wake turn. Yet `await E as e` "has the event as its value" and `await t` "binds `t`'s variables"
(§6.6). Nothing says the match result is copied into the intention (its `waiting` attribute or its
frame) at match time, so an implementer following V4 and §4.3 would resume with a dangling event, or
with bindings that no longer hold. (b) The guard `g` of `await E as e [g]` is evaluated by the
dispatcher in the event's turn. A useful guard mentions the waiting plan's locals
(`await Reply as r [r.id == request_id]`). The dispatcher has no frame for that intention pushed.
§6.6 does not say which scope the guard sees, and the isolation rules of §6.4 (own choice point,
fuel slice) are not extended to await guards, which are user code run in the event turn for every
waiting await ("per event, every waiting `await`", §15). A faulty await guard is a hard failure of
an unrelated event's turn, the same defect R3 #1 closed for reconsideration conditions.

Evidence. §6.5 line "mark every intention whose await matches e ready", §6.6 last paragraph, §5.8,
§15 per-turn deliberation bullet, §4.2 step 8.

Fix. State: at match time the dispatcher copies the event (or the unified bindings) into the
intention's `waiting` cell, so the resumed statement reads that copy; the guard runs with the
intention's frame pushed, under its own choice point and `vagent.condition_fuel`, and a faulting
guard counts as "no match" and raises `ConditionFault`. Add `waiting` payload to the §5.6 attribute
list.

### 4. MAJOR - 6.1, 17: `revise` has no key, so it cannot revise a many-valued …

Full title: 4. MAJOR - 6.1, 17: `revise` has no key, so it cannot revise a many-valued predicate,
and there is no conflict policy between sources

Problem. `revise battery(b.level)` "replaces every fact of `battery` whose only source is `self`".
That is revision by functor. It is right for `battery(level)`, and wrong for the predicates the
document itself declares elsewhere: `at(who, x, y)` in §8.5. `revise at(w, x, y)` for one entity `w`
would delete the agent's self-sourced beliefs about every other agent, because the replacement set
is the whole predicate. Nothing lets the author say which arguments identify the thing being
revised. Related: sources are a set, so a peer's `tell battery(10)` coexists with the agent's own
`battery(50)`. `holds believes battery(?level) & level > floor` takes the first binding, so which
value wins depends on rank order. The document's belief-revision story (§3 V8, §17 "a fact set with
sources") is therefore a set with no revision policy beyond manual `forget`, and the one primitive
offered is too coarse for a typed multi-entity predicate. Also unspecified: a fact with sources
`self` and a peer, and `revise` of that predicate: is the self source removed and the fact kept?

Evidence. §6.1 third bullet, §8.5 `predicate at(who, x, y)`, §12.3 `revise position(p.at)`, V8, §17
beliefs row.

Fix. Let a predicate declare a key: `predicate at(who: Agent -> x: Float, y: Float)`; a keyed
predicate holds at most one value per key and source (the last `believe`, `revise` or `tell` from a
source replaces that source's value), and `revise at(w, x, y)` replaces only the facts with key `w`.
A predicate with no key keeps the present set semantics, and `revise` on it replaces the whole
predicate, as now. State that `revise` removes only the source `self` from a fact that other sources
also assert. For conflicting values from different sources, either state that `believes` yields all
of them in source order (and `holds` therefore means "some value satisfies"), or offer
`believes t from self`. Cost: one cell per key declaration (a dimension of arguments, in the house
style) and a key comparison on assert.

### 5. MINOR - 6.2, 6.3, 12.3: the restorer's success test, and `holds` before the first percept

(a) A restorer is "an `achieve` intention for the same goal" (§6.3), but a maintain goal has no
`succeed when` and §6.2 says achieve intentions succeed on `succeed when`. State that a restorer's
success test is its goal's `holds` (tested at its step boundaries and turn ends, and not while an
effect is in flight, per §6.4), and that this is the same rule as "completes while `holds` still
fails counts as failed". (b) In §12.3, `on Initialize { adopt KeepCharged(reserve_floor) }` runs
before any `Battery` percept. The base is closed-world, `holds believes battery(?level) & ...`
fails, and the turn end adopts a restorer: the rover docks at start-up. Say what an unknown `holds`
does: for example, a maintain goal is monitored only after `holds` has been true once (or after
`adopt ... when believes ...`), or the example adopts it from the first `Battery` handler. (c)
`recharge` completes when `dock()` returns, and the `battery` percept can arrive after `EffectDone`
(the race the rover's `travel` plan now awaits). Then the restorer counts as failed and back-off
starts although the dock worked. There is no way to write the wait: `await` takes a term (or an
event), not a condition on a bound value, and `await battery(?l)` succeeds at once on the existing
fact. Add an optional guard on term awaits (`await battery(?l) [l > reserve_floor] within d`), use
it in `recharge`, and note that `Reach`'s success by exact `position(location)` equality is a
modelling shortcut a real host would turn into a tolerance test. (d) `drop` of a restorer by the
hazard handler is not a failure, so no back-off is set and the next turn end adopts a new restorer:
the rover starts docking right after an emergency halt. Say whether dropping a restorer sets
`blocked_until`.

### 6. MINOR - 6.4: a permanently faulty condition raises `ConditionFault` every turn

`ConditionFault` is raised each time a condition faults, and the state stays "unchanged", so the
same fault recurs at every turn end, appending an event and running any handler each time (and every
wake the adapter schedules). Raise it once per intention and condition until the condition next
evaluates without a fault, keeping a flag on the intention, and count repeats (`condition_faults`)
as the document does for dropped events.

### 7. MINOR - 6.4, 7.1: `cancel` before `achieve` leaks a delegated intention

Delivery is unordered (§7.3), and a message can be refused with `MailboxFull` and resent after a
later one was accepted. A `cancel` that overtakes its `achieve` finds no intention for its
correlation id and is dropped; the `achieve` then arrives and adopts a goal nobody wants, which may
act on a device until it ends (the delegator drops its `outcome` as cancelled). State that a
`cancel` for an unknown correlation id is remembered until the `achieve` with that id arrives or
`vagent.send_retries` times `vagent.send_retry_interval` has elapsed. Separately,
`Accepts(from, goal: Str)` (§10) passes the goal name only, so a delegate cannot refuse on
arguments; pass the arguments as a record or say that is intended.

### 8. MINOR - 6.5, 8.6: `drop` and `fail` of the running intention or its ancestors

A plan can call `drop i` on itself or on an ancestor through `intentions()`. State the effect: the
current statement completes, then the segment ends and the intention is ended as dropped at the
turn's reconsideration (not mid-statement), and the staged entries of the segment stand, as with any
statement boundary. Also record that between suspension points a plan segment runs without
interleaving, so beliefs and fields cannot change under it. That is the concurrency model between
intentions and deserves one sentence in §14 ("no data race between intentions: they interleave only
at suspension points").

### 9. MINOR - 6.6: an elapsed effect deadline leaves the effect running

§6.6 stages `cancel` on a delegated `achieve` timeout; the same holds for an effect whose deadline
elapses (`follow ... within 10min`): the statement fails, the plan is abandoned or retried by
another plan, but the host may still be driving, and the next plan's `follow` collides with it
(contract of §5.3: "serialised or refused with busy"). Stage the binding's `cancel` on elapse too,
as on drop, and count the eventual result as late.

### 10. MINOR - 6.3, 6.7: goal failure is final, with no wait-for-applicability

When no plan is applicable at selection time (for example `travel`'s `where` fails because the
battery belief is momentarily low), the goal fails at once and `tried` is never cleared; GOAL and
Jadex would wait or retry. This is a defensible Jason-like choice and the comparison table in §17
says "first applicable untried". Say in §6.3 that an `achieve` goal with no applicable plan fails
immediately, that the author expresses waiting with `await` inside a plan or with a maintain goal,
and that an adopter wanting a retry re-adopts from a `GoalFailed` handler.

## Assessment

The seams I flagged in round 3 are closed, and the agent model is now internally coherent: goal
kinds, plan selection, single-minded commitment, forward failure with staged effects, wait states,
maintain goals as permanent intentions. The remaining defects are specification gaps that an
implementer would hit immediately: dropping a dead intention (and what the safety handler does if
that fails), the order of end-of-turn event raising against `d.pending`, what an `await` keeps when
it matches, and a revise primitive that cannot revise a many-valued predicate. Each has a local fix
and no new C++.

VERDICT: NOT CONVERGED
