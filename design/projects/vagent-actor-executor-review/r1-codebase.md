# R1 codebase critique — `design/vagent-actor-executor.md`

Reviewer lane: citation truth and mechanism-claim verification, per `brief-common.md`. Every
file:line citation and load-bearing mechanism claim in the design was checked against the tree at
review time. Claims are classified `CONFIRMED`, `REFUTED`, or `PARTIALLY`; each finding carries
severity, spec location, the problem, the evidence, and a concrete proposed fix.

Counts: 0 BLOCKING, 4 MAJOR, 6 MINOR, 3 NIT.

Most of the spec's substrate claims are accurate — the mailbox-first pump diagnosis, the
foreign-cursor mark leak, the checkpoint/quiescence contract, the mark/discard/commit mechanics, and
the compileFragment/fragment-head infrastructure all check out. The load-bearing defects are one
wrong storage mechanism (clause weights), an internal milestone inconsistency (M0 lowering scope),
one mis-stated no-marks contract around mid-turn meta verbs, and an unspecced term-cell encoding for
beliefs. All are correctable without changing the design's shape.

## Findings

### F1 — MAJOR — clause weight storage is a `d.vars` record, not a `+d.weight` edge

- Spec location: §2.1 "Weight storage" claim that a clause cell "is a junction" whose
  `setWeight`/`weightOf` hit "the scalar `+d.weight` edge arm — O(1) trailed edge read/write", and
  §3.4's "collect (weight, clause) in one walk (O(1) +d.weight edge reads)".

- Problem: the claim is REFUTED. Clause cells never carry a `+d.weight` edge. `graftPlan`
  (meta.cpp:1396-1407) and `learnPlan` (meta.cpp:1732-1741) mint clause cells with `+d.name`,
  `+d.grab`, `+d.spin` and nothing else. `weightOf` (meta.cpp:602-619) reads `+d.weight` only when
  the edge already exists — the junction arm written for `sproutSynapse` outputs — and otherwise
  falls back to `varsRecord(binds_.agentCell, label)` → `+d.values` → `asDouble`. `setWeight`
  (meta.cpp:2038-2064) has the same split: it writes `+d.weight` only on cells that already have it;
  for a labeled clause it mints/updates a label-keyed `d.vars` record on the agent cell. meta.hpp's
  own contract (:268-271, :431-436) documents the label-keyed record as the clause channel.

- Evidence: meta.cpp:1396-1407, :1732-1741 (clause mint shape), :602-619 and :2038-2064 (dual-arm
  read/write), meta.hpp:268-271.

- Consequence: per-clause `weightOf` is an O(V) `rankAfter` text scan of the agent's `d.vars` rank
  (which also carries `unlearn:`, `fault:`, `meta.serial` and `priority` records), so a selection
  pass over C clauses costs O(C·V), not O(C). The virtual-weight-order design survives — the
  dispatcher can snapshot the rank into a label→weight map in one O(V) walk, then do O(1) lookups —
  but the §3.4 algorithm description and §6's pricing ("~30 lines; same greedy semantics") rest on a
  mechanism that does not exist. Second-order effect: two clauses sharing a `+d.name` label share
  one weight record; distinctness relies on `patch:<n>` labels and qualified declared names.

- Fix: rewrite §2.1's bullet to describe the actual dual-channel model (`+d.weight` edge on junction
  cells for `sproutSynapse`/`set_edge_weight`; label-keyed `d.vars` record on `binds_.agentCell` for
  clauses and all other named cells). In §3.4 specify the one-walk snapshot (or a per-selection
  memo), and state the label-uniqueness assumption explicitly.

### F2 — MAJOR — M0 cannot lower decl bodies: the scalar Emitter refuses every agent statement kind

- Spec location: milestone table M0 ("each plan/on/def/method → clause/symbol cells with
  compileFragment-shaped heads") and the M3 row ("statement-segmented body lowering").

- Problem: PARTIALLY REFUTED scoping claim. The parser and checker do populate everything M0 needs
  as input — `Decl` bodies for plans, handlers, methods, guards, triggers, priorities, bindings,
  goal clauses, predicate/question and record/event declarations (parser.cpp:436-465, 468-499,
  640-662, 664-699; checker.cpp:2490-2513, 2547-2568), and the frontend runs parse → resolve →
  expand → check into `CompileArtifacts` (frontend.cpp:9-45). But the only body-lowering path, the
  scalar `Emitter` (compiler.cpp case list around :680-716 and the default-refusal arm near :1283),
  handles only scalar `ExprKind`s (literals, identifier, fail, unary, binary, block, let, if, cell
  paths, static, code, call). Every agent-language statement —
  `Achieve`/`Tell`/`Believe`/`Spawn`/`Await`/`While`/`For`/`Emit`/`Install`/
  `Mount`/`Stop`/`Return`, plus `Splice` — hits the refusal arm. So under the spec's own milestone
  split, M0's "compileFragment-shaped heads" can only be minted for bodies that contain no agent
  statement, while the statement-segmented lowering those bodies need is scheduled for M3. M0 as
  written either silently produces empty/stub heads for real plans and handlers, or it must absorb
  the first statement-lowering arm itself.

- Evidence: compiler.cpp:680-716 (handled kinds), :1283 area (default refusal); parser.cpp:436-699;
  checker.cpp:2490-2568; frontend.cpp:9-45.

- Fix: pick one and say so — (a) restate M0 as "structure + registration only": clause/symbol/rank
  cells minted, heads left as a declared stub body (e.g. an immediate-fail fragment) with the real
  body emitted at M3; or (b) move the first segment-lowering arm (statement split + scalar segments
  through `compileFragmentBody` + host-verb dispatch records) into M0. Either way, M0 also needs a
  per-decl purity/`closed` derivation: `FragmentInfoMap` is keyed by code-literal `FragmentKey`, so
  descriptor `pure`/`closed` records for decl-body fragments have no existing producer (the
  checker's `derivePurity` is internal and keyed the same way).

### F3 — MAJOR — §3.8's "no-marks rule satisfied by construction" is false inside the turn mark

- Spec location: §3.8, the parenthetical claim that meta verbs run "between segments (cursor
  quiescent — where `mintMetaBindSet`'s no-marks rule is satisfied by construction)".

- Problem: REFUTED as stated. `mintMetaBindSet` does refuse under live marks (meta.cpp:65), but the
  verbs an executor invokes mid-turn — `graft_*`, `learn_*`, `unlearn`, `set_weight` — run inside
  the turn mark `m0` from §3.2 step 1 (held until the `discard(m0)` at step 7), so
  `outstandingMarks() ≥ 1` for the whole dispatch window. The rule that actually bites mid-turn is
  different and worse than a refusal: `resolveDimension` (meta.cpp:818-825) resolves filed names
  fine, but an *unfiled* name falls through to `ensureDimension` (arena_manifold.cpp:78-105), which
  mints a cell and mutates `arenaDims_` — an untrailed member map — with no marks check at all.
  Under `m0`, that means `discard`/`release` rolls the `+d.dims` link back while `arenaDims_` keeps
  a name→cell entry for a cell whose rank edge is gone: a cross-mark invariant break, not a clean
  refusal.

- Evidence: meta.cpp:65 (mintMetaBindSet marks check), :818-825 (resolveDimension),
  arena_manifold.cpp:78-105 (ensureDimension, no marks guard, writes `arenaDims_`);
  genesis.cpp:49-54 (genesis's own no-marks refusal shows the pattern is deliberate elsewhere).

- Fix: restate the contract honestly: mid-turn meta verbs run under a live mark, so dim resolution
  must be resolve-only — every dim name reachable from a reifiable fragment must already be filed
  (the genesis (b) sweep and emission-time `fileDimensionNames` cover registered heads, which makes
  the common case work). Then either add an `outstandingMarks()==0` guard to `ensureDimension`
  (matching `mintMetaBindSet` and genesis) or explicitly spec which call sites may mint under marks.
  Without that, the first `learn_*` that reifies a fragment naming a never-filed dim corrupts the
  dim table on rollback.

### F4 — MAJOR — belief facts need a term-cell encoding the spec never names

- Spec location: §3.1's `d.belief` row ("clause-cells on `d.belief` predicate ranks — same shape
  `d.clause` uses: head term on `+d.grab`"), §3.2's believe/forget/revise step, §3.4's `believes`
  evaluation.

- Problem: PARTIALLY REFUTED by omission. `+d.grab` on `d.clause` cells holds a fragment head — a
  `runFragment`-executable descriptor with `+d.entry` and an arg/arity contract (meta.cpp:664-800).
  A belief fact is a `Term` AST node (checker types `Believes`/`Believe`/`Forget`/`Revise` —
  checker.cpp:1035-1037, 1509, 1580-1582, 2033, 2074-2075 — and the scalar Emitter refuses them, so
  no lowering exists). Asserting `believe p(1, x)` requires emitting the term into cells (a
  functor/args layout `VortexStdLib::unify`/`argumentsOf`/`deref` can consume — the `zigzag::Vlog`
  term shape in vortex_stdlib.cpp), and matching `believes` queries by unification, not fragment
  execution. Putting a term on `+d.grab` where existing consumers expect a head is a same-dim,
  different-payload convention the spec never calls out; nothing in the tree emits vagent AST terms
  to Vlog term cells (the vprolog `lowerTerm` path is a different compiler's encoding).

- Evidence: meta.cpp:664-800 (`+d.grab` payload contract via `runFragment`'s `+d.entry` checks),
  checker.cpp:1035-1037/1509/1580-1582/2033/2074-2075 (typed verbs), vortex_stdlib.cpp:4115+
  (`unify`/`deref`/`argumentsOf`/`Vlog` cell-level term engine, adapter-usable with no second VM —
  that part of the spec is CONFIRMED).

- Fix: spec the term-cell encoding (functor text cell + `d.vars`/args records — the `Vlog` layout or
  an explicit vagent variant), state that `d.belief` clause cells carry it on `+d.grab` by a
  distinct convention from fragment heads (or pick a different dim/edge to avoid ambiguity), and
  name `believes` = deref/unify walk over the predicate's `d.belief` rank as the evaluation rule.

### F5 — MINOR — §4.6 names fields that do not exist under those names

- Spec location: §4.6 bounded-memory section ("`liveCells(turn boundary) ≤ grant.maxLiveCells`", and
  the `ActorOptions` population caps `maxIntentions`/`maxTimers`/`maxPending`/`maxHostDeliveries`/
  `maxClausesPerPredicate`).

- Problem: PARTIALLY. `ActorGrant` has `maxCells`, not `maxLiveCells` (mesh_actor.hpp:59), and the
  arena side is `allocationLimit_`/`cellCount()` (set once at spawn via
  `setExecutionLimits(initial + grant->maxCells)`), enforced by `mintSlot` throwing — so a hard
  ceiling exists, but the doc's names and its turn-boundary measurement/reclaim discipline are new
  work. `ActorOptions` (mesh_actor.hpp) carries mesh/heartbeat/lease limits only; none of the five
  named population caps exists anywhere.

- Evidence: mesh_actor.hpp:59 (`maxCells`), `ActorOptions` member list; arena
  `setExecutionLimits`/`allocationLimit_` use in mesh_actor.cpp.

- Fix: rename the invariant to `cellCount() ≤ grant.maxCells` (or rename the grant field in the same
  breath), and list the five `ActorOptions` fields plainly as new API surface so they are not read
  as existing substrate.

### F6 — MINOR — §4.1's deficit-counter policy is under-specified for the stated bounds

- Spec location: §4.1, the `wakeStreak`/`msgStreak` counters with `B_msg(p)=(K+1)·p`,
  `B_wake(q)≤q+⌈q/K⌉+1`, and the parenthetical "serve wake if
  `wakes>0 && (mailbox.empty() || wakeStreak < K)`, else message and reset".

- Problem: PARTIALLY. The diagnosis (mailbox-front always wins, mesh_actor.cpp:520-531) is CONFIRMED
  and the bounds are plausible, but the counter semantics are ambiguous as written: which counter
  increments on a wake service vs a message service, when each resets, and what happens when
  `wakeStreak ≥ K` while wakes are still queued. The formula only holds under one specific
  discipline (message service resets `wakeStreak`; `wakeStreak` counts consecutive wake services
  since last message).

- Evidence: mesh_actor.cpp:520-531.

- Fix: write the counter discipline as two lines of pseudo-code (increment X on wake service, reset
  on message service, reset Y on wake service) so the implementer cannot pick the mirror-image
  policy that starves the other side.

### F7 — MINOR — §3.5's intention-record field list drops `delegator`

- Spec location: §3.5, the parenthetical quoting the spec'd record shape as
  "`id,kind,goal,plan,tried,step,waiting,ready,blocked_until,faulted,frame`".

- Problem: NIT-level inaccuracy — the cited source, vagent-agent-language.md:596-615, lists eleven
  fields including `delegator` ("a peer cell and correlation id"); the doc's list omits it.

- Evidence: vagent-agent-language.md:596-604.

- Fix: add `delegator` to the parenthetical (or drop the parenthetical and just cite the range).

### F8 — MINOR — §4.7's "meta.cpp internal maps get one sweep" is vacuous as scoped

- Spec location: §4.7 determinism bullet.

- Problem: the scoped sweep finds nothing to do — meta.cpp uses only ordered `std::map`/`std::set`
  and vectors (`fresh` :875, `captureFor` :2101, `seen`/`impure`/`program`/`consumed`/`internal`
  sets); meta.hpp includes `<unordered_map>` but declares no unordered member. The actual unordered
  containers live next door and are membership/dedup-only as claimed: `vortex_vm.cpp`'s
  `unordered_set`s (:828, :874, :1384, :1464) are membership/duplicate-detection; `vortex_stdlib`'s
  `varMap`/ `routineBindings_` maps (vortex_stdlib.hpp:52, :503; vortex_stdlib.cpp:118-145,
  :1380-1402) are lookup/freshening only; `mesh_evaluator.cpp`'s `unordered_set`/`unordered_map`
  (:790+, seen/active sets) are cycle/dedup. None iterates into observable order.

- Evidence: container audit above; mesh_actor.hpp:287-291 + mesh_actor.cpp:51 confirm the mesh-owned
  maps are `std::map` — the core of the determinism claim, CONFIRMED.

- Fix: reword the work item to "audit unordered containers in the vm/stdlib/meta path for iteration
  into observable order" — true as scoped but the risk (and the one grep-worthy path) is
  vortex_stdlib, not meta.cpp.

### F9 — MINOR — `apps/vagent` already exists; "no binary and no tests" is stale for the row, true for the executor

- Spec location: sibling-doc claim the spec inherits (agent-language P11 row) and the doc's own
  "everything the spec calls 'the adapter' is greenfield".

- Problem: PARTIALLY. `apps/vagent/main.cpp` exists — AGENTS.md lists `vagent check` plus an
  iteration-3 runner — so "no `vagent` binary" is stale. What is true, and what the doc needs:
  nothing under `apps/vagent` touches `outbox`, `d.intention`, `d.subgoal` or `believes`; the
  actor-executor adapter, decode/encode, outbox drain, settle loop and `$turn` binding are unbuilt
  (grep over apps/vagent returns nothing for those names; the plasticity test marks the boundary
  gated, tests/xuzz/vagent_plasticity_test.cpp:1247+).

- Evidence: `ls apps/vagent` (main.cpp only); AGENTS.md program table;
  vagent_plasticity_test.cpp:1247-1260.

- Fix: state it as "the `vagent` binary exists as checker/runner shell; the actor-executor adapter
  inside it is greenfield" — keeps the work item, drops the stale binary claim.

### F10 — NIT — §3.5 cites meta.cpp:1850-1851 for both repoints

- Spec location: §3.5, "re-pointed `+d.spin` (`graft_body`/`learn_body` repoint at a fresh installed
  head — meta.cpp:1850-1851)".

- Problem: :1850-1851 is `learnBody`'s repoint; `graftBody`'s equivalent is at :1345-1346. The claim
  itself is CONFIRMED for both verbs (reify copy → `link(clause, +d.spin, installed)`).

- Fix: cite ":1345-1346, :1850-1851".

### F11 — NIT — §3.1's `d.seen` opcode cite stops mid-function

- Spec location: §3.1 `d.seen` row, "Link/Break/Linked opcodes suffice (vortex_vm.cpp:745-829)".

- Problem: the range covers `Link` (:745-767) and `Break` (:768-782) fully but cuts `Linked` off
  mid-body (:783-851); appending also needs `New` (:853-898). All three behaviors check out; the
  range is just sloppy.

- Fix: cite ":745-851" (and `:853-898` if `New` counts).

### F12 — NIT — pumpOne reply wording glosses the failure arm

- Spec location: §2.1, "replies to every `ActorSend` with the executor's returned string".

- Problem: on executor failure the reply is an `ActorDown` envelope, not the string
  (mesh_actor.cpp:595-610). The mechanism claim is otherwise CONFIRMED (auto-reply to every
  `ActorSend`, `std::optional<ActorMessage>` for wake turns, exceptions →
  `ActorError::ExecutionFailed`).

- Fix: "replies to every `ActorSend` — the returned string on success, an `ActorDown` on failure".

## Confirmed claims (no action needed)

Mesh: `ActorExecution.message` is `std::optional<ActorMessage>` (mesh_actor.hpp:89-97). `pumpOne`
selection at mesh_actor.cpp:510-621 — mailbox-first at :520-531, wake decrement/`wakes`/no-reply
shape confirmed, exceptions → `ActorError::ExecutionFailed`, `ActorSend` auto-reply at :595-610.
`wake()` at :781-793 increments the counter and refuses missing/ stopped actors. `checkpointView` at
:829-855 refuses on every named condition: running/stopped, nonempty mailbox, pending wakes,
`choiceDepth() != 0`, `outstandingMarks() != 0`, stateRoot containment failure, incomplete
deliveries. `GenesisHook` fires at spawn (:200-210) and resume (:333-347). Mesh-owned actor tables
are `std::map` (mesh_actor.hpp:287-291, mesh_actor.cpp:51) — deterministic key order as claimed.

Meta: `runFragment` at :664-800 — requires the operand be a head with `+d.entry`, validates
descriptors/arity, binds args on the cursor's `+d.vars`, pushes a Halt-landing frame on `+d.stack`
(:730-734), runs `vm_.run` on the same cursor (:776), restores scope (:768-783); only spawns a
cursor when none is passed (:720). `clausesOf` tombstone filter at :210-220. `clauseHost` at
:1167-1192 resolves names through the module's `d.vars`. `graftPlan`/ `learnPlan` mint exactly the
`+d.name`/`+d.grab`/`+d.spin` clause shape at :1396-1407/:1732-1741; learned clauses join the
`d.clause` rank and thus the next enumeration. `learn_body`/`graft_body` reify + repoint `+d.spin`
(:1850-1851/:1345-1346). `varsRecord` (records.hpp) is the name-keyed `d.vars`/`+d.values`
convention, with direct precedent for keyed multi-field records in `mintPatch`'s
`field(descriptor, ...)` calls (:1658-1664) and the `priority` clause record (:1455, :1787) — the
intention-record shape has a sound in-tree template.

Vortex VM: the foreign-cursor choice-point defect is CONFIRMED — `backtrack` (vortex_vm.cpp:315-332)
pops a foreign cursor's choice point without `release(cp.mark)`, leaving the mark outstanding; only
same-cursor or null-cursor points release. `Link`/`Break`/`Linked` (:745-851) and `New` (:853-898)
behave as the spec needs; `spawnCursor` appends via `rankTail`, which returns the home cell on an
empty rank (:242-245) — empty-rank spawning confirmed. `backtrack(nullopt)` matches any cursor's
point — confirmed, which is what makes the leak reachable.

Arena: `consumeWork()` throws on exhaustion (arena_manifold.hpp:481-487) and is called on
allocation/intern paths. `commitStateSide` is the `ref > extent ? Clears : Additive` predicate
(:278-281). `release` is well-nested trail replay, decrementing marks (:843 contract). `discard`
clears the trail when it releases the last mark — already implemented
(arena_manifold.cpp:1606-1608), so sibling-doc P13 is ahead of the table. Promotion discovers the
frontier bidirectionally (:1794-1796), skips `d.dims` edges (:1779-1781), and applies
`Suppress`/`Clears`/`Additive` per owned persistent shadowed cell (:2054-2113); `holdsOwn` is
`denseOf().has_value()` (:270-272), so a minted-at-genesis or shadowed `home` promotes as §3.1
requires.

Compiler/frontend: `imageFloor_` is once-per-compiler (compiler.hpp:200-208); preflight refusal at
compiler.cpp:716; pointer-keyed tables at :1622-1643 — `Expr*` identity holds for the compile's
lifetime, acceptable for in-session tables. `compileFragment` exists (fragment.hpp:166-168).

Lineage/dims: `kAgentDimensionNames` (lineage.hpp) includes `d.outbox` as filed vocabulary;
state-side `extraRoots` at lineage.cpp:190-205; coverage assertion in tests at
vagent_lineage_test.cpp:107-127. `resolveDimension` (meta.cpp:818-825) and genesis's no-marks
refusal (genesis.cpp:49-54) are as described. `GenesisRoots` already carries `ownModule`
(genesis.hpp:83-92), so M0's `clauseHost` bare-name requirement is satisfiable through the bind set.

Verifier/language claims: belief verbs are checker-typed, scalar-lowering- refused, and absent from
`MetaRuntime` — confirmed on all three legs. `vagent-agent-language.md:596-615` is exactly the
intention-record section; its P4/P8 rows corroborate the wake-starvation diagnosis; §9.2's
self-tightening example and the `vagent-metaprogramming.md:2808-2880` cite resolve. `WorkerContext`
is the arena/core/vm/stdlib bundle the spec assumes (worker_context.hpp:51-74).
`unify`/`deref`/`argumentsOf`/`functorOf`/ `renderTerm` exist adapter-side over a cell-level `Vlog`
with no second VM. `ActorMessage.sequence` exists and defaults to 0 (mesh_actor.hpp:326) — the
"sequence==0 for `ActorSend`" decode convention is consistent with it.

VERDICT: NOT CONVERGED
