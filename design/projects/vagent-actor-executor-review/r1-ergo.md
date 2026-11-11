# R1 Review — Ergo lane (implementer usability)

**Verdict: NOT CONVERGED.** Spec `design/vagent-actor-executor.md` v0.1 read against
`apps/common/xanadu/vortex/mesh_actor.{hpp,cpp}`, `vortex/vortex_vm.{hpp,cpp}`,
`vortex/vortex_stdlib.hpp`, `zigzag/arena_manifold.hpp`, `identity/identity_layout.hpp`,
`vagent/{meta,compiler,fragment,genesis,lineage}.{hpp,cpp}`, and the reference specs
`vagent-agent-language.md` / `vagent-metaprogramming.md` / `vagent-runtime-grounding.md`.

Counts: **1 BLOCKING, 4 MAJOR, 8 MINOR, 0 NIT.**

Lane question: can an engineer implement M0–M4 from this document alone, without re-deriving the
design? Headline answer: the *architecture* is implementable and the code citations mostly check
out, but the doc is not self-contained where it matters most — §3.2 silently drops four of the
eleven turn mechanics the source spec carries, the two load-bearing new conventions (host-step ABI,
segment-spine dispatch) are named but never specified, and one code claim (clause weight storage) is
false in a way that silently breaks the §9.3 gate if implemented literally. An M1 engineer would
write a turn loop that leaks scope cells forever, a dispatcher that cannot find handler priorities,
and a selector that reads weights from an edge that is never written.

mdl and `mdformat --check --wrap 100` both pass on the spec as-is; findings below are content, not
formatting. Any fix touching prose must keep the wrap-100 reflow and `MD007` two-space list indent.

______________________________________________________________________

## Findings

### 1. Clause weights do not live on `+d.weight`; the stated sort input reads a never-written edge

**Severity: BLOCKING.** §2.1 :79-80 and §3.4 :199-202.

The doc asserts: "A clause cell *is* a junction: `setWeight`/`weightOf` hit the scalar `+d.weight`
edge arm — O(1) trailed edge read/write" (:79-80), and §3.4 directs selection to "collect
`(weight, clause)` in one walk (O(1) `+d.weight` edge reads)" (:200-201).

False for clauses. Every clause-minting path — `graftPlan` (meta.cpp:1396-1407), `graftHandler`
(:1446-1462), `learnPlan` (:1732-1741) — links only `+d.name`, `+d.grab`, `+d.spin` (plus a
`priority` `d.vars` record for handlers). No `+d.weight` edge is ever minted. `setWeight`'s junction
arm fires only when a `+d.weight` edge *already exists* on the cell (meta.cpp:2045); for a clause it
always takes the "clause arm" — a `d.vars` record **on the agent cell** keyed by the clause's
`+d.name` label (:2050-2059). `weightOf` mirrors this: edge check, then the keyed-record fallback
(:602-618). A clause is therefore *not* a junction; `+d.weight` junctions are the `d.synapse`
machinery (:2066-2076), a different feature.

Consequence: an implementer who follows §3.4 literally collects `+d.weight` edges per candidate,
finds none, and every clause ties at whatever default they picked — `set_weight`/`weight_of` then
appear inert, and the §9.3 Hebbian gate silently produces rank-order-only selection. The O(1) claim
is also wrong: the real read is a `d.vars`-record lookup on the agent cell (O(vars) per candidate),
which changes the cost arithmetic the doc used to kill `d.worder` (:338).

**Fix:** describe the real channel — selection calls `weightOf(clause)` per candidate (two-arm
read), or pre-loads the agent's weight records once per selection — restate the per-candidate cost
honestly, and define the missing-weight sort key (finding 9). If the design instead intends clause
weights to move onto `+d.weight` edges, that is a `set_weight` clause-arm change and must be stated
as work.

### 2. §3.2's nine steps silently drop four turn mechanics §4.2 carries

**Severity: MAJOR.** §3.2 :146-166 versus `vagent-agent-language.md` §4.2 :324-372.

An engineer implementing steps 1–9 as written produces a turn that is missing:

- **The base choice point and Dewey reset** (:334-336): `setCurrentDeweyPath({})` plus a base choice
  point whose alternative is `std:agent`'s step-failure routine. This is what makes "no remaining
  choice points" unreachable (:360-361) and gives `fail_step` its landing pad. The doc says
  "`fail_step` runs as a short turn with reserved fuel" (:164-165) but never says the turn must
  *contain* the CP that receives it.
- **`clearChoicePoints()` before `discard(m0)`/`release(m0)`** (:341-343): §3.2 step 7 asserts
  `choiceDepth()==0 && marks==0` but never says how depth reaches zero. Live CPs pin marks (§5.1's
  `release(cp.mark)` mechanics, vortex_vm.cpp:315-331), so without the clear the assert cannot hold
  and `checkpointView` (`wakes`/`choiceDepth`/`outstandingMarks` checks, mesh_actor.cpp:833-837)
  refuses forever — the exact §5.1 symptom, re-created adapter-side.
- **Scope unlink** (:344-345): the cursor's `+d.vars`/`+d.stack` ranks carry every turn's activation
  frames (§5.8 :645-653) and grow for the agent's life if not unlinked — a per-turn cell leak inside
  the one structure the doc's own §4.6 memory invariant is measuring.
- **`vagent.compact_every`** (:345-347): the only dead-link-run reaper. Unmentioned anywhere.

The failure note (:163-166) also covers only the intention arm — "the intention records the named
fault" — while M1's entire scope is handler turns, where no intention exists. §4.2's other arms are
absent: a non-intention event failure is dropped and counted in `$stats` (:366-369); a hard failure
inside genesis/`Initialize` is *fatal* — `ChildStopping{cause:"init"}` plus a stopped actor
(:369-372); an `ask`/delegated-`achieve` turn owes a typed-fault reply (:366-368).

**Fix:** restore the four mechanics as named steps (or rule them out with reasons), and restate the
failure matrix for non-intention turns — at minimum the `$stats`-drop arm and the fatal-
initialization arm, since both fire inside M1's scope.

### 3. §3.8 names the host-step seam but never specifies its ABI

**Severity: MAJOR.** §3.8 :247-256.

The convention is one sentence: a meta-verb call "lowers as a statement-boundary host step: the
emitted segment ends at the call, the executor runs the `MetaRuntime` verb synchronously between
segments, binds results to the continuation's inputs, and resumes at the next segment." To write
either side of that boundary an engineer needs, and does not get:

- **Verb naming** — what the emitter leaves at the boundary that names the verb: a `d.vars` record?
  a `+d.name`-labelled cell? an enum? `MetaRuntime` is a C++ method surface (meta.hpp:196-307) with
  no name dispatch; the mapping table (source name → member) does not exist anywhere.
- **Argument marshaling** — evaluated argument cells reach the C++ signature how — a staged list
  head? positional `+d.values`? on which cell?
- **Result binding** — "the continuation's inputs" presumably means the next segment's `+d.values`
  params bound by name on a fresh frame, i.e., `runFragment`'s convention (meta.cpp:664-800) —
  plausible, never stated.
- **Halt discrimination** — how the executor tells "segment ended at a host step" from a normal
  halt, an M4 effect suspension, or fuel exhaustion.

No existing host-step convention exists in code to crib from; §11.1's adapter file list
(`agent_executor`, `host_bindings`, vagent-agent-language.md:1604-1605) is the landing zone but
specifies no record shape. (The r1-architect report covers the milestone-*ownership* facet; this
finding is the missing contract itself — once owned, there is still nothing here to implement.)

**Fix:** specify a host-step record (e.g., a `d.vars` record on the suspension frame: `verb`, `args`
list, `result` target), a static verb→member table, the result-binding rule, and a halt-class field
distinguishing host-step from suspension/exhaustion.

### 4. The tunables are mis-homed in `ActorOptions` and split the `vagent.*` naming convention

**Severity: MAJOR.** M0.5 :366-368; §4.1 :265-267; §4.2 :274-281; §3.1 :133-135; §4.6 :305-306.

`ActorOptions` (mesh_actor.hpp:67-76) is the **mesh** struct — runtime-wide tables (actors,
mailboxes, deliveries, monitors, heartbeat/lease). Of M0.5's list only `ready_per_message` is
mesh-level — it feeds the §4.1 `Actor` deficit counters, which genuinely are a mesh change.
`journal_size` (the `d.seen` K), `condition_turn_fuel`, `maxClausesPerPredicate`,
`reclaim_deadline`, and the population caps `maxIntentions`/`maxTimers`/`maxPending`/
`maxHostDeliveries` all govern executor-internal turn machinery — adapter territory, whose
established home is the `system://settings` table read by launcher/adapter once P9 lands
(vagent-agent-language.md:1561-1576). "ActorOptions tunables" as written sends an M0.5 engineer to
grow the wrong struct.

Naming also splits three ways: bare `ready_per_message` (the published name is
`vagent.ready_per_message`, :1575); camelCase `maxClausesPerPredicate`/`maxIntentions`/… against the
`vagent.snake_case` convention; and `vagent.condition_turn_fuel` silently **replaces** the published
`vagent.condition_fuel` row (:1570, "1,000 steps … each condition") — §4.2's "one aggregate account,
not 1000 steps per condition" (:275-276) is a semantic amendment of a published default, presented
as a fresh name with no amendment marker.

**Fix:** split the knob list by owner — `ActorOptions::readyPerMessage` alone on the mesh; an
adapter settings struct / `system://settings` rows for the rest — spell every knob `vagent.*`, and
mark `condition_fuel` → `condition_turn_fuel` as an amendment of the :1563-1576 table.

### 5. The event-cell schema ships three incompatible implicit-field lists

**Severity: MINOR.** §3.2 :148-150; §3.3 :174-175; versus vagent-agent-language.md §5.5 :569-581.

- §3.2 gives implicit `source/space/time/ordinal` with "+d.name type, d.vars fields"; §3.3 gives the
  provenance triple `(source/ordinal/requestId)` — `space`/`time` vanish, `requestId` appears.
  §5.5's four implicit fields (:572-574) match §3.2, not §3.3 — so is `requestId` on the cell, or
  only wire-level correlation?
- §5.5 puts the event type name in the cell's *content* (:571); §3.2 puts it on a `+d.name` edge —
  an unflagged divergence (uniform with clause labels, but unstated as an amendment).
- The wire dict's `kind`, `floor`, and `correlation id` keys (:576-578) get no cell mapping — `kind`
  consumed by decode? `floor` stored, dropped, or applied to `remembered`?

**Fix:** one schema table, wire key → cell location; flag the `+d.name` choice as a §5.5 amendment
if intended.

### 6. The dispatch shorthand hides four inputs step 4 actually needs

**Severity: MINOR.** §3.2 :152-154.

"(priority→mount→position merge)" compresses §5.7 :636-639 but never says: priority lives in a
clause-side keyed `d.vars` record (`"priority"`, meta.cpp:1453-1458; §5.7 :633); the merge walks
`d.extends` supertypes per source (§5.5 :593-594 — absent entirely here); awaits match *before*
handlers (§6.5 :847); `consume` stops lower handlers (§6.5 :857) — the doc does not mention
`consume` at all. For M1 (no behaviors/awaits) the contract reduces to priority→position over the
type + supertype ranks — say so, and say where the priority record lives.

**Fix:** restate M1's merge contract with each input's cell location; mark `consume`/awaits as
deferred semantics or include them.

### 7. The intention record enumeration drops `delegator`

**Severity: MINOR.** §3.5 :209-211 versus vagent-agent-language.md:598-605.

The inline list `id,kind,goal,plan,tried,step,waiting,ready,blocked_until,faulted,frame` is
presented as "the spec'd record shape" but omits `delegator` — the field `continue(i)` needs to
stage outcomes to a delegating parent (:843-844). Per-field schemas legitimately require the cited
cross-read (`waiting`'s kind/key/deadline/matched sub-record, `ready` = `$turn_count` value,
`faulted` = condition set), but an enumeration presented as complete must be complete or marked
abbreviated.

**Fix:** add `delegator`, or write "…" and pin the citation as canonical.

### 8. The step-path *value* is spec'd (§6.6); the segment-spine emission scheme is waved

**Severity: MAJOR.** §3.5 :207-223; M3 :376-377.

§6.6 :904 specifies the persisted value — "the list of statement ordinals down to it" — plus the
`while`/`for` resume rules. What the doc owes and never gives: how a lowered body exposes "the
segment spine" — is each segment a `+d.entry` head? does entry dispatch via an ordinal-keyed
`d.vars` table, a `+d.step` chain walk, or a first-segment dispatcher? where do boundaries fall
relative to host steps (finding 3) and the §6.6 suspension-point table (:862-870)? The deferred
alternative names the missing artifact exactly: labeled segments "need a new emission contract for
no current requirement" (:222-223) — the adopted spine needs the *same* contract minus labels, and
M0's lowering can't emit what isn't specified.

**Fix:** add the emission contract — segment-head shape, ordinal dispatch mechanism, boundary rules
at suspension/host-step/nested-control edges.

### 9. The virtual sort has no rule for a weightless clause

**Severity: MINOR.** §3.4 :199-205.

`weightOf` returns `std::optional` and disengages (meta.cpp:618); the §9.3 example itself treats a
missing weight as routine (`else set_weight(c, 0.1)` first-seed, vagent-metaprogramming.md:2868).
Descending sort needs a key for absent weights — 0.0? sort last? rank-position only? — and the
tie-break rule should fold the absent-weight case in.

**Fix:** state the default (e.g., absent → `0.0`, ties by rank position).

### 10. The `d.seen` ring's bound check and eviction relink are unspecified

**Severity: MINOR.** §3.1 :132-138; §3.2 :151.

"Append posward, evict the head at capacity K": append = `rankTail` + `Link`, evict = relink the
agent's `+d.seen` to the second member — `Link`/`Break`/`Linked` suffice as claimed
(vortex_vm.cpp:745-829). But the size test's source (a `#Linked` count per append — O(K) — or a
counter record, and if a record, on which cell) and the writer fragment's shape ("packaged as a
std:agent fragment") are unnamed.

**Fix:** name the size source, the eviction relink, and note append is O(K) bounded by
`journal_size`.

### 11. `msgStreak` is declared and never used; "arbitration counters" conflates two counter sets

**Severity: MINOR.** §4.1 :264-270; §3.2 :156; §4.7 :312-313; M4 :380.

The stated rule uses only `wakeStreak` (`wakeStreak < ready_per_message`, "else message and reset");
`msgStreak` has no role — needed for the `B_msg` proof? replay? vestigial? Separately, §3.2's
"bounded by arbitration counters", §4.7's replay record, and M4's "arbitration-counter consumption"
blur mesh deficit counters with adapter-side ready/timer service bounds.

**Fix:** give `msgStreak` a role or drop it; name the two counter sets distinctly.

### 12. The outbox schema and drain order are thinner than the §4.2 contract they serve

**Severity: MINOR.** §3.7 :237-245; §3.2 :158-159.

`kind,target,payload,id` omits what §4.2's six-class ordered drain implies (:348-350 — spawns →
messages → cancels → `preempts` effects → other effects → timer changes): per-kind fields (effect
args, deadline, cancel target, `preempts` flag) are unspecified. Who mints an entry (an in-graph
record under `m0`, presumably a `std:agent` fragment — unstated) matters for §4.4's post-commit
invariant. The requestId layout (16+16+4 hex, :584-586) is referenced as "the deterministic triple"
without a cite.

**Fix:** per-kind field table; restate the drain order; name the staging writer; cite :584-586.

### 13. The fault vocabulary is scattered and the §3.5 ladder compresses out real arms

**Severity: MINOR.** §3.2 :163-166; §3.5 :224-227; §3.7 :243-245; §4.2-4.3.

Fault terms — `body_replaced`, detached-clause, `condition`, `fuel`, `fail_step`, `faulted`,
`blocked_until`, `drop when`, `ActorDown`, `EffectFailed{cause:"duplicate"}` — appear inline with no
closed enumeration or mapping to §6.7's table (:916-931) and `fail_step`'s cause set
(`fuel`/`cycles`/`depth`, :365-366). The ladder sentence drops `contract`, `remote_fault`,
`refused`, `dropped`, and the ConditionFault-raised-once rule (:789-793). Recoverable by
cross-reading, but the doc claims convergence-level completeness.

**Fix:** one fault table (name → where recorded → next state), or an explicit "vocabulary = §6.7
plus body_replaced, detached-clause, duplicate" line.

______________________________________________________________________

## What I would need answered to write M1's first 200 lines

Concrete questions the doc leaves open at the `Executor` signature
(`std::expected<std::string, ActorError>(const ActorExecution &, WorkerContext &)`,
mesh_actor.hpp:148-149):

1. Where the callback gets the agent cell / program root / dims — `mintMetaBindSet` takes both roots
   by argument (meta.hpp:137-139); whether genesis-hook state, `home +d.agents`, or `stateRoot`
   supplies them at turn start is unstated.
1. The full pre-dispatch ritual — base CP, Dewey reset, `$turn` record shape — (finding 2).
1. Which implicit-field list and wire-key mapping is authoritative (finding 5); how the `as x`
   binding reaches the body (frame `d.vars` param per `runFragment`, presumably — unstated).
1. Where handler `priority` lives and whether supertype ranks merge (finding 6); how `+d.grab` guard
   vs `+d.spin` body each run under the handler CP.
1. What stages an outbox entry and the drain order (finding 12).
1. What a soft turn fault returns — reply-string encoding, `$stats` write, or `ActorError` (findings
   2, 13).
1. The `d.seen` writer's size source and cost (finding 10).
1. The adapter delivery queue's shape — M1 is "wake + handler dispatch", and §3.2's "wake → adapter
   delivery queue drain" names a greenfield queue whose entry shape (decoded event cells? delivery
   records?) is unstated.

VERDICT: NOT CONVERGED
