# R2 Review — Ergo lane (freeze pass on v0.2, implementer usability)

**Verdict: NOT CONVERGED.** Spec `design/vagent-actor-executor.md` **v0.2** re-read against
`apps/common/xanadu/vortex/{vortex_vm,mesh_actor,vortex_core}.{hpp,cpp}`,
`zigzag/arena_manifold.{hpp,cpp}`, `vagent/{meta,lineage,records,genesis,fragment,compiler,ast}`
`{hpp,cpp}`, `identity/identity_layout.hpp`, `system_docs.hpp`, `bencode.hpp`, and sibling spec
`vagent-agent-language.md` §4.2/§5.5–§5.7/§10.

Counts: **0 BLOCKING, 3 MAJOR, 6 MINOR, 2 NIT.**

Headline for the lane: v0.2 landed nearly all of my r1 dispositions — the weight channel is now
described truthfully (verified end-to-end), §3.2 carries the full ten-step contract skeleton, §3.8
specifies a real ABI whose marshal convention matches `runFragment`'s established binding shape, and
the tunables moved to the right home. But the newly-rewritten §3.2 **exit protocol is wrong in three
compounding ways** — the commit step drops the `clearChoicePoints()` its own assert requires, the
failure bullet describes `backtrack` semantics while naming `cut(0)`, and the step-0 ordering writes
scope/base-CP state *below* `m0` where `release(m0)` cannot reach it — and the failure matrix's new
"init/fatal → `$stats`-drop" arm contradicts the sibling spec's fatal-init arm outright. An engineer
implementing the turn exits *literally as written* produces an actor that can never quiesce and a
recovery path pointed at a landing pad the named call just destroyed. Separately, §3.8's two call
positions leave expression-position calls (which the gate's own statements use) unclassified.

______________________________________________________________________

## Part A — did my r1 dispositions land

| r1 finding                                                  | v0.2 status                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| ----------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1 BLOCKING — clause weights on `+d.weight`                  | **LANDED.** §2.1 :80-85 + §3.4 :235-243 describe the real channel — one `d.vars` weight-record walk → label→weight snapshot → O(1) pairing, O(V+C). Verified against `meta.cpp:602-619` (`weightOf` two-arm read), `:2038-2064` (`setWeight` clause arm → `varsRecord`), `records.hpp:69-79` (text-keyed records).                                                                                                                                                   |
| 2 MAJOR — §3.2 dropped four mechanics                       | **PARTIALLY LANDED, with new defects.** Base CP, Dewey reset, per-turn scope (step 0, :170-171); scope unlink + `compact_every` (step 9, :185-188); `clearChoicePoints()`+`release(m0)` idiom (:191-195); `$stats`-drop arm (:197) — all added. But the exit protocol is wrong three ways → **finding 1**; the init/fatal arm is misstated → **finding 2**; the ask typed-fault arm (`:366-368`) and six-class drain order (`:348-349`) still absent → findings 2/9. |
| 3 MAJOR — §3.8 ABI unspecified                              | **LANDED in substance.** `+d.verb` naming, `+d.vars` arg marshal, `+d.vars` result writeback, refused→`fail_step`, segment-descriptor halt discrimination (:315-321). The marshal convention matches `runFragment`'s verified name-keyed `+d.vars` binding (`meta.cpp:736-756`). Residual gaps → **finding 3**.                                                                                                                                                      |
| 4 MAJOR — tunables mis-homed in `ActorOptions`              | **LANDED.** §4.2 :348-352 "Settings home (r1-corrected) … never `ActorOptions`"; M0.5 owns `defaultSettingSpecs` rows (:462-463); caps renamed `vagent.max_*` (:345-347). Read channel still unnamed + naming residue → **finding 4**.                                                                                                                                                                                                                               |
| 5 MINOR — three incompatible event-field lists              | **PERSISTS.** §3.2 step 2's implicit four (`source/space/time/ordinal`, :174 — matches `:572-574`) vs §3.3's "provenance triple (`source/ordinal/requestId`)" (:209) — `requestId` is an *envelope* field (`identity_layout.hpp:317`), not a cell field. Wire keys `kind`/`floor`/correlation (`:576-578`) still unmapped → **finding 5**.                                                                                                                           |
| 6 MINOR — dispatch shorthand hides inputs                   | **PERSISTS.** Step 4 (:177) still doesn't name where handler `priority` lives (clause-side `d.vars` scalar, `:633`; `meta.cpp:1453-1458`) or whether `d.extends` supertype ranks merge (`:593-594,:636-639`) → **finding 6**.                                                                                                                                                                                                                                        |
| 7 MINOR — `delegator` dropped from intention record         | **LANDED.** §3.5 :248 lists `delegator`, cites `:596-615` (verified — `delegator` at :602).                                                                                                                                                                                                                                                                                                                                                                          |
| 8 MAJOR — segment-spine emission waved                      | **PARTIALLY LANDED.** Now an owned M2 work item (:469-473) + the ABI is specified; but the spine's own contract — segment-head shape, `step`-path→segment dispatch, descriptor shape, nested-control boundaries — is still implementer-invention → **finding 7**.                                                                                                                                                                                                    |
| 9 MINOR — weightless-clause sort key                        | **LANDED.** "absent weight → `0.0`" (:238) + "rank position breaks weight ties" (:240).                                                                                                                                                                                                                                                                                                                                                                              |
| 10 MINOR — `d.seen` bound/eviction mechanics                | **PARTIALLY LANDED.** Ring closes through the agent cell (:157-159) and `vagent.journal_size` names K (:155); the size-test source (O(K) count walk vs counter record) and eviction relink still unnamed → **finding 8**.                                                                                                                                                                                                                                            |
| 11 MINOR — `msgStreak` roleless                             | **PERSISTS.** §4.1 :330-332 still uses only `wakeStreak`; "reset" unqualified → **finding 9**.                                                                                                                                                                                                                                                                                                                                                                       |
| 12 MINOR — outbox schema/drain thinner than §4.2's contract | **PERSISTS.** Per-kind fields (effect args, deadline, cancel target, `preempts`), the six-class drain order (`:348-349`), and the entry writer all unstated → **finding 9**.                                                                                                                                                                                                                                                                                         |
| 13 MINOR — fault vocabulary scattered                       | **PARTIALLY LANDED.** Enumerable now — `fuel`, `fail_step`, `faulted`/`blocked_until`, `drop when`, `ActorDown`, `body_replaced`, detached-clause, `condition`, `duplicate`, `$stats`-drop, `ExecutionFailed` — but still no table; `remote_fault`/`contract` arms absent → folded into findings 2/9.                                                                                                                                                                |

______________________________________________________________________

## Part B — new findings in v0.2 text

### 1. §3.2's exit protocol cannot be implemented as written — three compounding errors

**Severity: MAJOR.** §3.2 steps 0/1/7 (:170-172,:182) and the failure bullet (:191-200).

Three defects in the same exit protocol, each independently fatal to a literal implementation:

- **Step 7's commit omits the drain.** "commit: `discard(m0)` → writes keep; assert
  `choiceDepth==0 && marks==0`" — but the base CP pushed at step 0 (plus any surviving per-statement
  CPs from step 4) still sit on `choiceStack_`, each holding an arena mark (`pushChoicePoint` calls
  `arena().mark()` at `vortex_vm.cpp:302`; `outstandingMarks_++` at `arena_manifold.cpp:1592`).
  `discard` ignores its mark argument and pops exactly one floor (`arena_manifold.cpp:1596-1610`),
  so after step 7 `choiceDepth ≥ 1`, `outstandingMarks ≥ 1`, the same-line assert can never pass,
  and `checkpointView` refuses forever (`mesh_actor.cpp:833-838`). The prescribed sibling idiom —
  "On success: `clearChoicePoints()`, then `discard(m0)`" (`vagent-agent-language.md:341`) — appears
  in v0.2 only on the failure arm. (The vortex lane rates this BLOCKING; for the
  implementer-usability question the effect is identical: the step list as written fails its own
  assert.)
- **The failure bullet describes `backtrack` while naming `clearChoicePoints()`.**
  "`clearChoicePoints()` (backtrack-drain to the step-0 base CP, which releases every per-statement
  mark)" (:191-192): `clearChoicePoints()` is `cut(0)` (`vortex_vm.cpp:360`), which pops *every* CP
  — including the base — calling `discard(cp.mark)` (:340), not `release`, and lands no alternative.
  The described drain-to-base idiom is a different call (`backtrack`, `:311-334`, which releases
  marks and fires alternatives). Consequently "`fail_step` runs against the **surviving** base CP"
  (:196) names a pad `cut(0)` just destroyed — and the bullet never supplies the actual entry (the
  sibling's is a separate short turn after `release(m0)` plus a cursor-opcode reset,
  `:342,:363-366`). An implementer must choose between two real recovery designs the sentence fuses;
  neither is written down.
- **Step 0 orders state *below* `m0`.** The base CP's mark and any step-0 scope writes
  (`cursor +d.vars/+d.stack` links, :170-171) precede `m0 = mark()` (:172). `release(m0)` replays
  the trail only to `m0.trailSize` (`arena_manifold.cpp:1657-1661`), so on the failure path the base
  CP's mark stays outstanding and the step-0 scope edges **survive the rollback** — the same
  per-turn accretion step 9 (:185-186) exists to prevent, on the path where step 9 is not stated to
  run. The sibling avoids this by taking `m0` first (`:324`) and pushing the base CP inside it
  (`:334-336`); v0.2's inversion is unmotivated unless the base CP's mark is meant to double as the
  pre-turn floor a terminal `backtrack` releases — which only works if the pad is actually
  backtracked into (impossible under the named `cut(0)`).

**Fix:** pick one protocol and write it literally. Sibling-verbatim: `m0` at step 1, scope/`$turn`
writes and the base CP inside `m0`; success = `clearChoicePoints()` → `discard(m0)`; failure =
`clearChoicePoints()` → `release(m0)` → `fail_step` short turn (say so, and say the cursor opcode is
reset). Or keep v0.2's order and write the real idiom: `cut(1)` → `release(m0)` →
`backtrack(cursor)` — which lands the base CP's alternative, rolls the scope edges back for free,
and consumes `mB`. Either way: add the clear to step 7, drop "surviving base CP" or define it, and
say what the base CP's alternative op *is* (`std:agent`'s step-failure routine —
`vagent-agent-language.md:335-336`).

### 2. The init/fatal arm is folded into `$stats`-drop — init failure is *fatal*, not counted

**Severity: MAJOR.** §3.2 :197.

The sibling spec's recovery matrix has three non-intention arms, and v0.2's new sentence collapses
the wrong two. A hard failure inside a turn running genesis/`Initialize` is **fatal**, not
`$stats`-dropped: "`$initialized` is still unset, so every later turn would run the failing
initialisation again. It is fatal instead. The adapter stages `ChildStopping{cause: "init"}` … and
returns an error from the executor, which stops the actor" (`vagent-agent-language.md:369-372`). An
implementer following v0.2 literally produces an actor that stays alive, re-runs the failing init,
and fails forever — where the correct behavior is `stopped`. This is M1's scope (Initialize is an
event dispatched through step 4). The third arm — an `ask`/delegated-`achieve` turn owes a
typed-fault reply so the sender's statement fails with `remote_fault` (`:366-368`) — is still absent
from the doc entirely.

**Fix:** split the arms: "non-intention event failures drop + count in `$stats`; a hard failure in a
genesis/`Initialize` turn is fatal — stage `ChildStopping{cause:"init"}` and return the executor
error (`pumpOne` stops the actor); `ask`/delegated turns owe the typed-fault reply."

### 3. §3.8 leaves expression-position calls unclassified — the gate's own statements nest them

**Severity: MAJOR.** §3.8 :296-306.

The split names fragment position (→ inline clause-walk) and statement position (→ host step), and
the worked examples it gates use a third form the split never classifies: verb calls nested in
operand position — `learn_where(plan_named("demo:Guard.cautious"), …)` (`vagent-metaprogramming.md`
§9.2), `for c in clauses_of(symbol_named("demo","Feed"))` and `if believes …` /
`let w := weight_of(c)` (§9.3). "The emitted segment ends at the call" cannot run `learn_where`
whose argument is an unevaluated host verb, and no hoisting/ANF rule or expression-continuation rule
is stated — so the emitter has no rule for the most common syntactic form in the gate. (This
corroborates the architect lane's finding from the implementer side: until a rule is picked, the
`+d.vars` arg marshal cannot be emitted for any call with a nested verb.) Remaining marshal gaps,
each MINOR individually but listed so M2 prices them once:

- **`d.verb` is in no filed-dim list** — not `kAgentDimensionNames`/`kMetaDimensionNames`/
  `kGenesisDimensionNames` (`lineage.hpp:46-71`) and absent from M0's own filing list (:454-455).
  Emission-time minting is mark-free, but an unfiled name re-mints as a disconnected `DimRef` across
  fold/resume (`meta.cpp:808-825` explains the two-table hazard) — the executor's `+d.verb` dispatch
  would silently miss persisted segments. Add it to the filing enumeration.
- **Result arity**: `clausesOf` returns `std::vector<CellRef>` (`meta.hpp:226`) — "result names"
  doesn't say the list-marshal shape (rank head? `+d.values` chain?).
- **Verb-name cell shape**: is `<verb-name>` a content string cell, a `+d.name`-labelled symbol, or
  an interned name resolved like `codeNamed`? And the static name→member table is implied, not
  declared (which C++ owns it — `host_bindings`?).
- **Reentrancy**: whether a host step may itself drive `runFragment` (a verb that runs a fragment
  mid-turn under `m0`) is unstated; structurally it composes, but the doc should say.
- **Async/varargs**: verified non-issues — every `MetaRuntime` verb is fixed-arity and synchronous
  (`meta.hpp:196-325`); no verb is async (host deliveries are the only async path and they arrive
  via `d.pending`/wakes, not the seam).

**Fix:** add the third position rule (recommended: hoist nested calls to prior statement-boundary
host steps — leaves the ABI and spine unchanged), walk one §9.2/§9.3 statement through it, file
`d.verb`, name the result-marshal shapes and the dispatch table's owner.

### 4. The settings read channel exists but is never named — plus knob-naming residue

**Severity: MINOR.** §4.2 :345-352; §4.1 :330-332; §3.2 :179-180,:187-188.

"Actor-visible reads flow through whatever read channel the settings layer already exposes to
adapters" (:351-352) — the channel exists and is citable: `xanadu::getSetting(const Store &, name)`
(`system_docs.hpp:945-946`) or `SystemStoreModel::find`/`getDouble`/`getInt64`/`getBool`/
`getString` (:897-917), seeded by `defaultSettingSpecs` rows (:979). The doc should name it and say
*when* the executor reads — per-turn `fromStore`/`fromManifold` parses are real work, so the answer
is presumably "cached at spawn/genesis, re-read on a settings-change event" — but silent today.
Residue: `ready_per_message` (:331), `belief_event_depth` (:180), `compact_every` (:187) are used
bare where the published rows are `vagent.ready_per_message`/…/`vagent.belief_event_depth`/
`vagent.compact_every` (`vagent-agent-language.md:1569,:1575-1576`); `ready_per_message`'s storage
is unresolved ("Policy is runtime-owned and versioned", :333 — an `ActorOptions` field? a settings
row? a constant?); and `vagent.condition_turn_fuel` (:340) still lacks an explicit "amends
`vagent.condition_fuel` (`:1570`, per-condition 1,000 steps → one aggregate account)" marker — the
semantic change is argued but the amendment isn't flagged.

**Fix:** name `getSetting`/`SystemStoreModel` + the read cadence; prefix the three bare knobs; say
where `ready_per_message` lives; flag the `condition_fuel` → `condition_turn_fuel` amendment of the
`:1563-1576` table.

### 5. M0's "single-segment stub" has two readings — refuse-the-body or emit-a-placeholder

**Severity: MINOR.** M0 :458-459.

"each body lowering starts as a single-segment stub (the scalar Emitter refuses agent statement
kinds — real segmentation is M2's)". Reading (a): emit one `compileFragment`-shaped segment per body
and let the Emitter's default refusal (`compiler.cpp:712-716`) reject any body containing
`send`/`achieve`/`learn_*`/`believes`-statements — coherent, but M0 then can't lower a realistic
body at all (fine for M1's scalar harness handlers, if that's the intent — say so). Reading (b):
emit a degenerate placeholder segment per body — M1's "handler fires" then passes on a no-op. Also
unstated: whether the stub is a *one-segment spine* (so M2's runner dispatches it unchanged) or a
plain fragment M2 replaces; and whether `on`-handler bodies segment too (§3.5 scopes spines to
*plan* bodies, :250-251, but §3.8 puts host steps in "a handler or plan body", :304 — handler bodies
must segment for the gate). The DeclKind table covers decls; the per-`ExprKind` emission schedule
and the interim refusal semantics are its missing twin (~30 of ~55 `ExprKind`s hit the default
refusal). **Fix:** name the reading, name the interim refusal rule, and give each statement-kind
group its milestone.

### 6. Event-cell schema and dispatch inputs — r1 minors persisting verbatim

**Severity: MINOR.** §3.2 :173-178; §3.3 :208-217.

- **Schema**: §3.2's implicit `source/space/time/ordinal` matches the sibling's four (`:572-574`),
  but §3.3's "provenance triple `(source/ordinal/requestId)`" (:209) substitutes the envelope's
  `requestId` (`identity_layout.hpp:317`) for `space`/`time` — two lists still disagree, and
  `requestId`'s home (envelope vs cell field) stays muddy. Wire keys `kind`, `floor`, correlation id
  (`:576-578`) still get no cell mapping. The `+d.name` type edge (:173) still isn't flagged as the
  §5.5 amendment it is (content carries the type name, `:571`).
- **Dispatch**: step 4's "(priority→mount→position merge)" still doesn't locate priority
  (clause-side `d.vars` scalar, `:633`; `meta.cpp:1453-1458`) or mention the `d.extends` supertype
  merge (`:593-594,:636-639`) — for M1 the contract reduces to priority→position over the type +
  flattened supertype ranks; say so and mark `consume`/awaits explicitly deferred.
- **Ordinal source**: "sequence==0 for ActorSend" (:174) is wire-true (`:578`), but what fills the
  event cell's `ordinal` per ingress kind (mailbox `ActorSend` with `sequence` 0 vs a coalesced host
  delivery vs a timer replay) is unstated.

**Fix:** one wire-key→cell-location table; flag the `+d.name` amendment; state M1's reduced merge
contract; name `ordinal`'s producer per ingress.

### 7. The segment spine is owned now but its contract is still invention

**Severity: MINOR.** §3.5 :250-251; §3.8 :320-321; M2 :469-473.

"Entry dispatches on the persisted `step` path into the segment spine" names the intent; the
implementer still invents: segment-head shape (a `+d.entry`-bearing `compileFragment` descriptor per
segment? — the natural reuse, unstated); the ordinal-path→segment mapping (an existing precedent is
unremarked: the program image's `ops` rank is already a `+d.step`-chained int64 rank under a
`d.vars` record — `vortex_vm.hpp:153-156` — a spine descriptor could mirror it); where "the segment
descriptor the emitter wrote" (:320) lives (per-segment `d.vars` record? one body record?); and
boundary rules at nested control (`§6.6`'s `while`/`for` resume rules interact with segment
identity). **Fix:** say which existing shapes the spine reuses and that the residual format is M2's
deliverable, or specify the spine.

### 8. `d.seen` size source and eviction relink still unnamed

**Severity: MINOR.** §3.1 :154-160; §3.2 step 3 :176.

"Bounded K, closed through agent" is landed, but the writer fragment still has no stated size test —
an O(K) `#Linked` count per append, or a counter record (on the agent cell's `d.vars`? the ring
head?) — and no eviction relink description (break `agent→first` + `first→second`, link
`agent→second`; journal cell keeps its `+d.evidence` edges — §3.6's "ring eviction drops only the
index edge" is consistent, but the mechanics belong to whoever writes the fragment). **Fix:** name
the size source and the three-link eviction.

### 9. Carried cluster — each small, each still an implementer question

**Severity: MINOR.** (a) `msgStreak` still roleless and "else message and reset" (:331-332) doesn't
say what increments or what resets — the provable bounds need the discipline stated. (b) Outbox:
per-kind fields (effect args, deadline, cancel target, `preempts`), the six-class drain order
(`:348-349` — spawns → messages → cancels → `preempts` → effects → timer changes), and the entry
writer (which `std:agent` fragment or adapter path mints entries) all unstated (:283-284,:183-184).
(c) Delivery-queue entry shape (§3.3 :216-217) — decoded event cells? delivery records? — needed by
M1's wake path. (d) `$turn`'s record fields unstated (the sibling gives them: turn time + the
decoded event cell, `:332-333`). (e) The executor's `programRoot`/`agentCell` provenance —
`mintMetaBindSet` takes both by argument ("the harness owns which cells play program and agent",
`meta.hpp:135-139`); whether the genesis-bound `MetaRuntime` is retained per actor or the roots are
re-walked (`home +d.dirs/+d.agents`) per turn is unstated. (f) `as x` binding convention —
presumably a frame `d.vars` param per `runFragment`'s rule, unstated. (g) Miscites: step 5's
"condition-fuel account (§4.3)" — the account is §4.2's first bullet, §4.3 is the deferral rule;
step 2's "(coalesced, §4.1)" — coalescing is §3.3's ratified block; M1's shorthand
"commit/**clearChoicePoints→release**" (:467) inverts the success arm. (h) §10's M1 test asserts
"outbox staged → committed" (:519) while outbox staging is M4's work item (:478) and the drain is
listed in both — name which machinery proves it.

**Fix:** one line each; fold (b)'s drain order into §3.7 and (c)–(f) into §3.2's step text or a
turn-constants block.

### 10. NIT — "the 18 `DeclKind`s" miscounts the enum; `d.clone`'s motivating example is stale

**Severity: NIT.** M0 :456-457; §3.1 :116-117.

`DeclKind` has **19** members (`ast.hpp:184-202`: …, `Belief`, `Lifecycle`). And §3.1's "`d.clone`
is minted lazily 'outside marks' today" rationale — `d.clone` is filed at `VortexCore` construction
(`vortex_core.cpp:57`, `mintNamedDimension` on the `d.dims` rank), so `resolveDimension` takes the
filed arm (`meta.cpp:818`) and never reaches the lazy mint the `meta.cpp:34-38` comment describes;
the genuinely-unfiled new dim to cite is `d.verb` (finding 3). The rule stands; the example doesn't
prove it.

______________________________________________________________________

## Brief-question summary

1. **M1 alone?** Steps 0–9 are complete and each names real APIs
   (`pushChoicePoint`/`setCurrentDeweyPath`/
   `mark`/`setExecutionLimits`/`bencode::decode`/`runFragment`/`clearChoicePoints`/`discard`/`release`/
   `rankAfter`/`compact` all verified). Remaining invention: event-cell decode helper, `$turn`
   shape, priority location, delivery-queue shape — all MINOR. **But the exits are wrong, not thin**
   — findings 1–2 are the blockers.
1. **§3.8 ABI?** Implementable for flat statement-position calls (marshal matches `runFragment`'s
   verified convention); expression position unclassified is the load-bearing hole (finding 3);
   varargs/async verified non-issues; result arity/`d.verb`/reentrancy are MINOR gaps.
1. **Tunables read channel?** Exists — `getSetting`/`SystemStoreModel` getters
   (`system_docs.hpp:945,:897-917`) — but unnamed in the doc; read cadence unstated (finding 4).
1. **M0 "single-segment stub"?** Two readings (refuse-body vs emit-placeholder); interim `ExprKind`
   refusal semantics unstated (finding 5).
1. **Failure vocabulary closed?** Enumerable but scattered; `init/fatal` misfiled (finding 2),
   `remote_fault`/`contract` absent; no table.
1. **New revision ambiguities?** The step-0/`m0` ordering, "surviving base CP", the `$stats`-drop
   collapse, expression-position gap, `ready_per_message` storage — findings 1–4. Snapshot timing
   (§3.4) is fine: per-selection snapshot, stated.

VERDICT: NOT CONVERGED
