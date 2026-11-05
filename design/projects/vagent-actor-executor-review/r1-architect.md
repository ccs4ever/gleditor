# R1 Review — Architect lane (milestone ordering / deliverability)

**Verdict: NOT CONVERGED.** Spec `design/vagent-actor-executor.md` v0.1 §8 read against
`apps/common/xanadu/vortex/mesh_actor.{hpp,cpp}`,
`vagent/{ast,compiler,checker,frontend,fragment,meta,genesis,lineage}.{hpp,cpp}`,
`vagent/prelude/meta.vagent`, `Makefile`, `tests/xuzz/vagent_plasticity_test.cpp`,
`vagent_guide_test.cpp`, `docs/vagent-user-guide.md`, and the reference specs
`vagent-agent-language.md` / `vagent-metaprogramming.md` /
`vagent-metaprogramming-implementation.md`.

Counts: **2 BLOCKING, 6 MAJOR, 3 MINOR, 1 NIT.**

Lane question: does §8's milestone order tell the truth — can each ship green and leave a coherent
product? Headline answer: the architecture (lower → turn skeleton → beliefs → selection → intentions
→ gate) is sane, but §8 as written cannot ship green at M2/M3, leaves the one mechanism the exit
gate needs (§3.8 host steps) unowned, and carries an unlowerable construct into the only worked
examples it is gated on.

______________________________________________________________________

## Findings

### 1. M3's own acceptance tests need M4's machinery; the §3.8 host-step seam is owned by no milestone

**Severity: BLOCKING.** The milestone order is falsified twice, on the same root cause: statement-
segmented emission + the host-step runner is scheduled for M3 "with M4's consumers" (§8 :375-377)
while earlier milestones already need it, and M3's own tests need M4's.

(a) **M3's tests cite mid-suspension scenarios** (§10 :416-418): "`body_replaced` fault on
mid-suspension `learn_body`; detached-clause (`unlearn` mid-suspension) fault." Suspension needs
M4's deliverables — intention records, `waiting`/outbox staging, the wake→resume path — none of
which exist at M3. There is no stated stand-in (a hand-minted intention record? whose shape is
itself M4's work). As written, M3's acceptance list is unimplementable at M3.

(b) **M2's verbs are unreachable from bodies until the seam exists.** `believes`/`believe`/
`forget`/`revise` are §3.8 host routines "synchronous between segments" (:249-256). The emitter that
would produce the segments is the scalar kernel: it refuses every `Call` but `run`
(`compiler.cpp:641-648`) and ~30 of ~55 `ExprKind`s via the default arm (`compiler.cpp:714-716`).
M1's dispatch is `runFragment` on a monolithic body head (§3.2 step 4, :153-154). So at M2 a body
cannot contain `believe`/`revise`/`forget` at all — M2's test "believes/believe round-trip *through
a turn*" (:413-414) either means C++-harness invocation (then say so) or needs the seam that lands
no earlier than M3.

(c) **The seam's scope is under-stated in both directions.** §3.5 scopes segmented lowering to
*plan* bodies (:211-214); the gate's verb calls live in `on` handler bodies
(`on Attack`/`on Sighting`/`on Meal`, `vagent-metaprogramming.md:2823-2826,2855-2870`) — whether
`on` bodies segment is unstated. And the gate's calls sit in **expression** positions —
`if believes sees("food","water")`, `for c in clauses_of(...)`, `let w := weight_of(c)` — while §3.8
names only "statement-boundary" host steps. A call inside an `if` condition or a `for` collection
ends the segment mid-statement; the emitter must then carry `if`/`for` control flow *across* the
segment spine, a materially harder emission contract than "the emitted segment ends at the call".

**Proposed fix — decide once and write it down:** either (a) every clause body (`on`/`plan`/`def`)
emits as a segment spine from M0 — a monolithic body is a one-segment spine; M1's runner already
steps segments with zero host verbs wired; M2 wires the belief verbs; M3 owns step-dispatch +
selection; M4 owns persistence — in which case M3's item reads "step-path dispatch + resume guards"
and M2's verbs are reachable; or (b) bodies emit monolithically through M2 and M3 rewrites the
emission contract **and** M1's dispatch — price the rework and admit M2's verbs stay harness-only
until then. Either way: name §3.8's two halves (emitter boundary rules *including expression
positions*; the executor-side host-step runner + result binding) as an explicit work item, and move
M3's mid-suspension tests to M4 or the gate bullet.

### 2. The §9.2 gate's central input cannot lower as written — `believes` inside `code"…"` contradicts §3.4 *and* the reference spec

**Severity: BLOCKING.** §9.2's handler calls
`learn_where(plan_named("demo:Guard.cautious"), code"believes threat(?t) & t > 0.9")`
(`vagent-metaprogramming.md:2825`). The argument is a `code"…"` literal — a fragment, reified to a
`+d.grab` test head the selector *runs* as a graph.

- §3.4 (:189-191): "`believes`/`believe`/`forget`/`revise` are host-routine verbs (§3.8), never
  opcodes", with unification "adapter-side". Under that ruling, `believes` inside a fragment has no
  realization — a fragment cannot suspend for a host step; there is no segment inside a `code"…"`
  body for the executor to step through unless fragment emission also segments, which §3.5/§3.8
  never say.
- The reference spec says the opposite: `believes t` lowers as "clause walk with `#Unify`, one
  `#Choice` per clause" (`vagent-agent-language.md:1500`), and `Unify`/`IsVar`/`MakeVar`/`Deref`
  opcodes exist (`vortex_vm.hpp:80-84`). If that is still the intent, §3.4's "never opcodes" claim
  is simply false — and then `believes` in body `if` conditions needs no host verb either.

Either reading leaves §9.2 unlowerable *under this document's own text*: host-verb `believes` cannot
enter a fragment; graph `believes` contradicts the stated ruling. The same hole swallows
`if believes …` inside the §9.3 handler bodies (:2857) — a condition-position call that is neither a
stated host step nor a stated graph lowering.

**Proposed fix:** pick the channel and name it — (a) keep the §9.1:1500 graph lowering for
`believes` (fragment and body-condition positions alike), narrowing §3.4's host-verb set to the
writes (`believe`/`forget`/`revise`); or (b) rule that *all* emission — bodies and `code"…"`
fragments — segments at host-verb calls, and that every evaluator that runs such a head (the `where`
selector, §6.4 condition isolation, host steps themselves) is the §3.8 segment runner — then amend
§9.1:1500 in the same commit. Assign the fragment-level segmentation contract to a milestone; today
it belongs to nobody and the exit criterion depends on it.

### 3. M0's enumeration misses load-bearing work items — and its premise about Makefile wiring is wrong

**Severity: MAJOR.** Spec §8 :360-365. Checked against `ast.hpp:184-230` (18 `DeclKind`s),
`checker.cpp:2428-2552` (all kinds checked), `lineage.hpp:46-71`, `genesis.cpp:472-612`,
`fragment.hpp:73-135`, `Makefile:675,731-743,1049`.

Missing or under-specified:

- **Per-`DeclKind` disposition.** M0 names `agent`/`event`/`goal`/`predicate` symbols and
  `plan`/`on`/`def`/`method` clauses. The checker accepts the other ~11 kinds (`Record`, `Question`,
  `Capacity`, `Skill`, `Behavior`, `Answer`, `Field`, `Uses`, `Install`, `BehaviorUse`, `Belief`,
  `Lifecycle`) — M0 never says whether each lowers or takes a named refusal. `Field`
  (`var count: Int := 0`) is the most load-bearing omission: no agent state exists without it, and
  its initializer is an `Expr` needing emission + init-time evaluation. The clause→agent **ownership
  edge** is also unenumerated: clauses attach to goal/event symbols but an `on`/`plan` belongs to
  one agent decl; multi-agent modules (`§12.1`'s `Worker` + `SearchCoordinator`) need the owner
  recorded for dispatch filtering.
- **The statement-kind emission schedule.** `Emit` (M1's "outbox staged" test, :412),
  `Believe`/`For`/`While`/`FindAll`/`ForBelieves` (M2, the gate), `achieve`/`adopt`/`ask`/`await`/
  `drop`/`install`/`spawn`/`yield`/`terminate` (M4) — which ExprKinds gain emitters in which
  milestone is never decomposed; today all of them hit `compiler.cpp:714-716`.
- **`std:agent` prelude authoring.** Only `prelude/meta.vagent` exists; the language spec's package
  table (`vagent-agent-language.md:1546-1555`) owes genesis/dispatcher/deliberation (`fail_step`,
  the step-failure routine §4.2:334-336's base CP names)/beliefs/events — and §3.1's `d.seen` writer
  is "packaged as a std:agent fragment". No milestone owns the prelude file.
- **Image-side vs agent-side split.** Modules/symbols/clause skeletons live in the sealed image
  band; the agent cell, per-predicate `d.belief` hosts, the `d.seen` ring and `$`-fields are
  per-agent mints at spawn/genesis — and the channel mapping a spawn request to *which* agent decl
  (a module can declare several) is never named. M0 lists the pieces but not which side of the
  lineage each lands on.
- **Entry point, TU names, artifact.** The decl-walk API on `Compiler` (or a new type), the
  §11.1-named files `agent_executor.{hpp,cpp}`/`program.{hpp,cpp}`/`host_bindings.{hpp,cpp}`
  (`vagent-agent-language.md:1597-1607`), and the sealed-artifact/`programRef` minting
  (`:1609-1622`; `spawn` requires `grant.programRef == request.programRef`, `mesh_actor.cpp:150`)
  are all unnamed.
- **The `$`-reserved seed set** — `$turn`/`$stats`/`$initialized`/`$peers`/`meta.serial`/
  `$spawn_count` (:1502) — deserves enumeration, not "seeds" as a lump.

**Makefile wiring — premise corrected:** none is needed for a TU under `apps/common/xanadu/` — the
glob (`Makefile:675`) feeds `XUDU_CORE_OBJS`, which `xuzz_test` (:1049) and every `vagent_*_test`
binary (:1223,:1234,:1248,:1262,:1270) already link; a new `tests/xuzz/*.cpp` is auto-globbed
(:681). `vagent_runtime_test` deliberately excludes all vagent TUs (:743) and `VAGENT_FRONTEND_SRCS`
is a fixed list (:733-735) — both correct as-is for an executor TU.

### 4. M0.5's "ActorOptions tunables" puts adapter-level knobs in the mesh struct; the actual settings mechanism (P9) has no landing milestone

**Severity: MAJOR.** Spec §8 :366-368.

`ActorOptions` is the *mesh* runtime's option struct (`mesh_actor.hpp:67-76`). Of the listed names,
only `ready_per_message` is a mesh concern (it drives §4.1's source selection — and needs ctor
validation, `mesh_actor.cpp:80-89`). `journal_size`, `condition_turn_fuel`,
`maxClausesPerPredicate`, `reclaim_deadline` and the population caps (`maxIntentions`, `maxTimers`,
`maxPending`, `maxHostDeliveries`) are vagent-level — the adapter is greenfield
(`vagent-metaprogramming-implementation.md:36-38`), so *no options type exists for them*; the spec
names no owner struct.

The real landing mechanism exists and is already approved: P9 — `defaultSettingSpecs()` entries with
schema/notes pages (`system_docs.cpp:538`, `:2306`, `:2599`; table row at
`vagent-agent-language.md:1675`) for every `vagent.*` setting in the §10 table (`:1563-1576`). None
of the new names appear in that table, and `condition_turn_fuel` is not just an addition — §4.2's
"one aggregate account" *changes the semantics* of the existing `vagent.condition_fuel`
(per-condition slice, `vagent-agent-language.md:789`) without saying whether the per-candidate slice
survives beside it.

**Proposed fix:** split the tunable list by layer — `ready_per_message` (+ any mesh-level counter
caps) into `ActorOptions` at M0.5; the adapter set into a named `AgentExecutorOptions`/ settings
read, with the P9 `defaultSettingSpecs()` + schema/notes-page + §10-table work item assigned to a
milestone. Say whether `condition_fuel` (per-candidate slice) coexists with `condition_turn_fuel`
(aggregate) or is renamed outright.

### 5. `pumpOne`'s callback surface suffices for arbitration — but §4.7's "arbitration counters" have no channel; ActorExecution/ActorOptions need unstated deltas

**Severity: MAJOR.** The question's premise checks out on the narrow point: source selection happens
inside `pumpOne` before the callback (`mesh_actor.cpp:512-531`), and `ActorExecution.message`
already distinguishes wake vs message (`mesh_actor.hpp:89-97`) — the Executor signature (`:148-149`)
needs no change *for selection*. Counters (`wakeStreak`/ `msgStreak`) and the P8 scan position are
private `Actor`/runtime state (`:32-58`).

The gap is observability: §4.7 (:311-315) promises a replay record carrying "work source + item
identity + **arbitration counters**" — and `ActorExecution{pid, grant, message, cursor, stateRoot}`
cannot carry them. The adapter writes the record; the counter values at choice time are invisible to
it. Either `ActorExecution` grows fields (work-source kind + counter snapshot) or the mesh exposes a
per-actor getter — a surface change M0.5 does not enumerate. Without it the §11.3 replay claim is
undeliverable even though the M0.5 starvation test can observe service order externally.

Secondary notes worth one line each: the P8 rotating cursor should store the last-served `pidKey`
(erasure-safe once P7's deferred `erase` lands — an `actors_` iterator dangles), and under deficit
arbitration a wake served with mail queued leaves `soleQueued` false (`mesh_actor.cpp:534`) — the
P16 contract already reads correctly, but the interaction should be named.

### 6. §3.2's nine steps silently diverge from the §4.2 turn contract — M1 builds a different turn and no milestone owns the reconciliation

**Severity: MAJOR.** §3.2 :146-161 vs `vagent-agent-language.md:324-351` (the ratified ten-step
contract M1 claims to skeletonize). Dropped or contradicted contract elements:

- Step 4's **base choice point** (alternative = `std:agent`'s step-failure routine) + Dewey reset —
  absent from §3.2; see the vortex-lane report's BLOCKING on what this does to the failure path and
  to step 7's `choiceDepth==0` motivation.
- **Scope unlink** (`+d.vars`/`+d.stack`) + `compact_every` (contract step 8) — absent; without it
  the cursor's scope grows for the agent's life.
- **Drain order** (contract step 9): spawns → messages → cancels → `preempts` effects → other
  effects → timers, then unlink `$turn`'s event cell + each entry. §3.7's drain is a `rankAfter`
  walk — insertion order, not kind order — and step 9 omits the unlink.
- §3.2 step 6 runs ready-intention service inside *every* `m0`; the contract runs ready work as the
  subject of a wake turn (`continue(i)`, `:827-844`). Merging ready-service into message turns also
  sits oddly beside the doc's own rejection of turn batching "for merging failure domains" (:347).

M1 (:369-372) implements "steps 1–9 of §3.2" — i.e., the divergent list — and no milestone owns
either amending §4.2 or reconciling §3.2 to it.

**Proposed fix:** state plainly whether §3.2 supersedes §4.2 (then amend §4.2 in the same series —
see finding 8) or conform §3.2's steps to it; specifically address drain ordering (sort/group by
`kind` at drain or stage kind-ordered sub-ranks) and the scope-unlink step.

### 7. §5.1's fix is unowned; the ordering risk is flagged only as an avoidance rule with unenumerated sites

**Severity: MAJOR.** §5.1 (:319-325) declares the `backtrack` defect "a separate fix (vortex patch +
test), not part of the executor" — it appears in **no** milestone; §10's regression bullet (:423)
floats unattached. So the ordering risk is real but only partially flagged:

- §4.5's rule bans interleaved second-cursor speculation under open actor CPs — but no milestone
  enumerates the sites it governs: per-candidate `where` evals, §6.4's own-CP condition isolation
  (`vagent-agent-language.md:789`), `believes` reads if implemented via solve, and §3.8 host steps
  entered while a segment run left residual CPs (`runFragment` contractually preserves caller choice
  depth, `compiler.hpp:172` — a pending-alternatives residue is legitimate input to the next host
  step unless the emission forces a drain).
- The defect is worse than "mark leak": a popped foreign CP either releases a mark with no cursor
  update (silent rollback) or, via the `!cp.cursor` arm, *consumes* another cursor's CP and hijacks
  the backtracking cursor to a foreign alternative (`vortex_vm.cpp:315-333` — the vortex-lane report
  documents this; I verified the lines).

**Proposed fix:** attach the ~15-line patch + mark-leak test to M0.5 alongside the mesh seams (same
"sibling-substrate" character), or state explicitly that §4.5's rule + §4.4's post-turn assert is
deemed sufficient — and enumerate the speculation sites the rule must cover in M1–M4.

### 8. Spec-amendment paperwork has no milestone

**Severity: MAJOR.** The doc asserts amendments it never schedules; every one must land in the
reference specs for the corpus to stay true:

- §6.3's "first applicable in rank order" (`vagent-agent-language.md:758-759`) → virtual
  weight-order (spec :204-205 says "is amended" — the source text is unamended).
- §6.4's per-condition `vagent.condition_fuel` slice (:789) → the aggregate account.
- §10's settings table (:1563-1576) → ≥8 new settings (finding 4) + P9 entries.
- §11.1's arbitration paragraph (:1644-1657) and the approved P4 row (:1670) → the deficit-counter
  arbitration — the spec itself says "needs a P4 amendment" (:260) but assigns the edit to nobody.
- §9.1:1500's `believes` graph lowering if finding 2 lands as host-verbs; §4.2's turn steps if
  finding 6 lands as replacement.
- `docs/vagent-user-guide.md` — its runtime sections become live behavior once the executor lands;
  `vagent_guide_test.cpp` compiles every ```` ```vagent ```` block, so the guide is test-visible,
  not decorative.

**Proposed fix:** one line per amendment with its milestone owner (e.g., "P4-row + §6.3 + §10 edits
land with M0.5/M3"), or an explicit "spec edits travel with the implementing milestone" rule with
the list attached.

### 9. `reclaim_deadline`'s host-duty driver and `$stats` escalation channel are unowned

**Severity: MINOR.** §4.6 (:301-308) makes "quiescence inside `vagent.reclaim_deadline` … a named
host duty escalated via `$stats`" the enforcement path for the memory invariant — but the driver
(who watches the deadline, pauses ingress, wakes to quiescence, checkpoints+resumes) and the
escalation channel (how `$stats` surfaces pressure to the host) appear in no milestone. The doc
honestly strikes "bounded memory" without it — so either name the owner (the launcher? the §11.3
harness? an adapter watchdog?) or restate §4.6's enforcement as observational.

### 10. M1's "§11.3 harness shape" is under-enumerated

**Severity: MINOR.** The hooks exist — `setSender`/`setAuthorizer`/`setGenesisHook`
(`mesh_actor.hpp:166-169`), injectable `wallNow`/`now` params — but the deliverables deserve a list:
loopback transport + deterministic issuer (§11.1 :1639-1642), a mock host-binding registry (§11.1's
`host_bindings.{hpp,cpp}` — itself unowned, finding 3), the staged-outbox/`$stats` exposure API
(§3.2 step 9 names it; the access path — the executor's own exposed state — should be named),
run-until-idle with a turn cap (`vagent-agent-language.md:1696-1697`), the §4.7 replay record writer
(blocked on finding 5), and the harness's home (`tests/xuzz/` auto-globs into `xuzz_test`,
`Makefile:681,1049`).

### 11. The §9.2/§9.3 gate needs no M4 machinery — its placement hides the true critical path

**Severity: MINOR.** Neither worked example uses effects/awaits/intentions — `patrol()` and
`navigate_to()` are `def`s (`vagent-metaprogramming.md:2818,2853`); no `Emit`, no `achieve`, no
`await`. The gate's real prerequisites are M0 + M1 + M2 + M3 *plus the §3.8 seam* — placing it after
M4 is only honest if the ordering doesn't imply intentions are load-bearing for it. Say so, or pull
the gate to end-M3 so an M4 slip can't mask gate readiness.

### 12. Nits: `vagent run`, M1's staging channel

**Severity: NIT.** `apps/vagent/main.cpp:15-17` ships `vagent check` only — §11.1's `vagent run` is
unlisted in §8 (fine for the gate, which is test-driven — but say whether the CLI gains a run mode).
And M1's test "outbox staged" should say whether entries are staged by a real `emit` statement
(needs the `Emit` emitter — finding 3) or adapter-minted cells.

______________________________________________________________________

## Verified-as-accurate claims (for the record)

- §2.1's mesh claims: `pumpOne` mailbox-first/wake-decrement ordering (`mesh_actor.cpp:510-531`),
  `Executor` signature + try/catch → `ExecutionFailed` (`:542-551`), `ActorExecution.message`
  optional (`mesh_actor.hpp:89-97`), `wake`/`pendingWakes` (`:780-793`), `GenesisHook` at
  spawn+resume (`:200-210`,`:333-347`), `checkpointView`'s quiescence battery (`:829-855`),
  auto-reply (`:595-610`).
- §2.2's gap claims: decls parsed+checked, nothing consumed below the checker — the scalar kernel's
  refusal wall is real (`compiler.cpp:714-716`); no `agent_executor`/`host_bindings` TU exists;
  `std:agent` has no prelude file (only `prelude/meta.vagent`); `believes`/`believe` absent from
  `MetaRuntime`.
- Clause-bank claims: `clausesOf` tombstone-filtered (`meta.cpp:210-220`), `clauseHost`
  (:1167-1192), `graftPlan`/`learnPlan` mint the named shape (`:1396-1407`,`:1732-1741`),
  `learnBody` repoints `+d.spin` (`:1850-1851`); `mintMetaBindSet`'s no-marks refusal
  (`meta.cpp:65-68`); agent dims filed (`lineage.hpp:67-71` — `d.agents`/`d.seen`/`d.evidence`
  correctly absent pending M0).
- §5.1's mechanics verified at `vortex_vm.cpp:315-333`; `ActorOptions` ctor validation at
  `mesh_actor.cpp:80-89`; mesh maps are `std::map` (`mesh_actor.hpp:287-291`).
- Settings machinery: `defaultSettingSpecs` exists and is P9's named site (`system_docs.cpp:538`);
  the §10 `vagent.*` table is at `vagent-agent-language.md:1563-1576`.

VERDICT: NOT CONVERGED
