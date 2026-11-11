# R2 codebase critique — `design/vagent-actor-executor.md` v0.2 (freeze pass)

Reviewer lane: citation truth and mechanism-claim verification. Round-2 scope per
`r1-disposition.md`: verify my r1 dispositions landed, then verify every *new* file:line
claim in v0.2 — the weight-snapshot mechanism, the directory-cell promote story, the
named-identity resume check, the full §3.2 contract vs agent-language §4.2, the
`+d.verb` ABI, and the milestone re-base.

Counts: **0 BLOCKING, 2 MAJOR, 5 MINOR, 4 NIT.**

Bottom line: the r1 rewrites I dispositioned landed and are code-true — the weight
channel is now described correctly (verified end-to-end below), the marks contract is
honest, `maxLiveCells` is stated as owed, and `delegator` is back in the record list.
The new defects are concentrated in the **rewritten §3.2 turn sequence**: its failure
bullet names `clearChoicePoints()` while describing `backtrack`-drain semantics, and
its step ordering inverts the sibling spec's mark nesting. One r1 MAJOR (belief
term-cell encoding) was never dispositioned and is now load-bearing for v0.2's new
fragment-position `believes` lowering.

## r1 disposition landing check

| r1 finding | status |
| --- | --- |
| F1 (clause weights are `d.vars` records) | **LANDED.** §2.1:80-85 and §3.4:232-243 describe the real channel; mechanism verified consistent (F-new-1 below). |
| F2 (M0 can't lower bodies) | **LANDED.** M0:458-459 — single-segment stubs, "real segmentation is M2's"; M2:469-473 owns segmentation + ABI. |
| F3 (no-marks claim false mid-turn) | **LANDED.** §3.1:111-117 names `resolveDimension`'s refusal (:820-824) as the gate and `ensureDimension`'s missing check (arena_manifold.cpp:78-105) as the hazard; §3.8:308-313 replaces "quiescent" with the trailed-writes basis; `d.clone` filed-at-genesis requirement added and is code-true (meta.cpp:34-38,:967). |
| F4 (belief term-cell encoding) | **NOT LANDED / not dispositioned.** §3.4 still says "head term on `+d.grab` — same shape `d.clause` uses" with no named encoding — and v0.2's *new* §3.8 fragment-position lowering now depends on it. Re-raised as Finding 2. |
| F5 (`maxLiveCells`) | **LANDED.** §4.6:380-387 "owed, not cited" + `mesh_actor.hpp:55-65` cite — verified, no such field exists. |
| F6 (counter semantics) | **Not landed** — §4.1 policy text unchanged; `msgStreak` still named-but-unused, "reset" unqualified. Carried MINOR (Finding 6). |
| F7 (`delegator`) | **LANDED.** §3.5:248-250 lists it; cite `vagent-agent-language.md:596-615` verified (`delegator` at :602). |
| F8 (meta.cpp sweep) | **Not landed** — §4.7:393 still scopes "meta.cpp internal maps get one sweep". Carried MINOR (Finding 7). |
| F9 (vagent binary stale) | Resolved — v0.2 makes no "no binary" claim; the greenfield quote is attributed to the implementation doc (:25, :36-38 verified). |
| F10 (repoint cite) | Not landed — §3.5:263 still cites only `meta.cpp:1850-1851`. Carried NIT (Finding 8). |
| F11 (`d.seen` opcode range) | Not landed — still `vortex_vm.cpp:745-829`. Carried NIT (Finding 9). |
| F12 (ActorDown gloss) | Not landed — §3.2:201-202 still glosses the failure arm. Carried NIT (Finding 10). |

## Findings

### Finding 1 — MAJOR — §3.2's failure path mis-describes `clearChoicePoints()` and inverts the mark/CP ordering

- Spec location: §3.2 step 0 (:170-172, "push the turn's base choice point …
  establish per-turn scope") preceding step 1's `m0 = mark()`; the failure bullet
  (:191-197): "`clearChoicePoints()` (**backtrack-drain to the step-0 base CP, which
  releases every per-statement mark**), then `release(m0)` … `fail_step` runs against
  the **surviving base CP**".

- Problem: three false mechanism claims in newly-added text.

  1. **`clearChoicePoints()` is not a backtrack-drain and leaves no base CP.** It is
     `cut(0)` (`vortex_vm.cpp:360` → `:336-354`): pops **every** choice point —
     including the step-0 base — calling `discard(cp.mark)` (:340), not
     `release(cp.mark)` (`backtrack` does that at `:319`). It performs no state
     restore and lands no alternative. The "backtrack-drain to base" idiom the
     parenthetical describes is `while (choiceDepth() > base && backtrack(cursor))`
     — the different mechanism `vagent-metaprogramming.md` §6.4:1516 prescribes,
     which *stops* at base and *releases* marks. The spec mixes the API name of one
     protocol with the semantics of the other.
  2. **No "surviving base CP" exists** under the named call — `cut(0)` empties the
     stack. The sibling spec's actual hard-failure arm is "`clearChoicePoints()`,
     `release(m0)`, **reset the cursor's opcode**, and run the recovery"
     (`vagent-agent-language.md:341-343`) — recovery lands via opcode reset, not via
     backtracking onto a base CP.
  3. **The step ordering inverts the sibling's nesting.** Agent-language §4.2 takes
     `m0 = mark()` at step 1 (:324) and pushes the base CP at step 4 (:334-336) —
     CP marks sit *inside* `m0`, so `cut(0)` pops floors strictly LIFO and
     `release(m0)` covers every turn write. v0.2 pushes the base CP and mints the
     per-turn scope edges (`cursor +d.vars`/`+d.stack` links) *before* `m0`. Two
     consequences: (a) `cut(0)` discards the outer base-CP mark while `m0` is still
     outstanding — `discard` ignores the mark's identity (`std::ignore = m`,
     `arena_manifold.cpp:1596-1602`) and pops `floors_.back()`, i.e. **m0's floor**
     — a transient floors_ desync benign only because nothing trails before
     `release(m0)`; (b) `release(m0)` rolls back only to `m0.trailSize`, so the
     step-0 scope links **survive a failure rollback** — the exact per-turn scope
     leak step 9 (:185-187) exists to prevent, on the path where step 9 is not
     stated to run.

- Evidence: `vortex_vm.cpp:336-354` (`cut` discards), `:360` (`clearChoicePoints`
  = `cut(0)`), `:311-334` (`backtrack` releases + lands alternative), `:279-302`
  (`pushChoicePoint` holds `arena().mark()`); `arena_manifold.cpp:1596-1610`
  (`discard` ignores `m`, unconditional `floors_.pop_back()`), `:1612-1699`
  (`release` replays by the mark's recorded sizes); `vagent-agent-language.md:324,
  334-336, 341-343` (sibling ordering and recovery arm); `vagent-metaprogramming.md`
  §6.4:1510-1526 (the drain idiom and the well-nested rule it cites,
  `arena_manifold.hpp:843`).

- Fix: adopt the sibling's order verbatim — `m0 = mark()` first (fuel re-arm
  m0-relative), *then* push the base CP inside it — so CP marks nest under `m0` and
  `release(m0)` covers all turn writes including scope setup. Restate the failure
  arm to the sibling's actual protocol: `clearChoicePoints()` (it *discards* CP
  marks; say so), `release(m0)`, **reset the cursor's opcode to the fail-step
  routine** — drop "surviving base CP". Alternatively, if a base-CP landing is the
  intended design (its mark doubling as the pre-turn floor), spec the real idiom —
  `while (choiceDepth() > base && backtrack(cursor))` then one more `backtrack` —
  and say the base CP's mark is consumed by it.

### Finding 2 — MAJOR (carried from r1-F4, now load-bearing) — belief term-cell encoding is still unnamed; the new §3.8 lowering depends on it

- Spec location: §3.4 (:221-225, "head term on `+d.grab` — the same shape `d.clause`
  uses"); §3.8's new fragment-position bullet (:298-302): `believes` inside a
  fragment "lowers to the inlined clause-walk graph — the pure-arena belief-match
  subcircuit".

- Problem: r1-F4 was not dispositioned and the gap persists — and v0.2's new
  fragment-position lowering makes it load-bearing. The clause-walk lowering it
  cites (`vagent-agent-language.md:1500`: "`believes t` | clause walk with `#Unify`,
  one `#Choice` per clause") runs `#Unify` against whatever `+d.grab` holds. On
  `d.clause` cells `+d.grab` is a *fragment head* — a `runFragment`-executable
  descriptor with `+d.entry` (`meta.cpp:677-684`) — not a term `#Unify` can consume.
  A belief fact needs a term-cell shape (the `Vlog` layout `VortexStdLib::unify`/
  `deref`/`argumentsOf` operate on, `vortex_stdlib.hpp:223-225`), and nothing in the
  tree emits vagent `Term` AST to term cells — the scalar Emitter refuses `Term`/
  `Believes`/`ForBelieves` outright (`compiler.cpp:714-716`; kinds at
  `ast.hpp:63-65`). "Head term" quietly papers over the difference; the two
  consumers (fragment execution vs `#Unify`) want different payloads on the same
  edge name.

- Evidence: `meta.cpp:677-688` (`+d.grab` payload contract), `compiler.cpp:714-716`
  (default refusal), `ast.hpp:63-65` (`Term`/`Believes`/`ForBelieves` kinds),
  `vagent-agent-language.md:1490,:1500` (`#Unify` needs copied *terms*),
  `vortex_stdlib.hpp:223-225` (`deref`/`unify`/`argumentsOf` exist adapter-side —
  that half of §3.4 stays confirmed).

- Fix: name the term-cell encoding for `d.belief` fact records (functor text cell +
  ordered arg chain — the `Vlog` shape or an explicit vagent variant), state that
  fact `+d.grab` carries it by a different convention from clause `+d.grab`
  fragment-heads (or move facts to a different edge to remove the ambiguity), and
  add "term emitter" to M2 — the clause-walk graph needs both the encoding and the
  emitting path, and the host-side `believes`/`believe` verbs need it to read/write.

### Finding 3 — MINOR — §3.8's `+d.verb` dim is in no filed-dim list

- Spec location: §3.8 host-step ABI (:315-316, "the segment's final cell carries
  `+d.verb → <verb-name>`"); M0's filing enumeration (:454-455) names
  `d.agents`/`d.dirs`/`d.seen`/`d.evidence`/`d.clone`/recipe dims.

- Problem: `+d.verb` exists nowhere — not in `kAgentDimensionNames`' 13 names
  (`lineage.hpp:67-71`), not in `kMetaDimensionNames`/`kGenesisDimensionNames`
  (:46-62), and absent from M0's own filing list. Under §3.1's operative gate
  (:111-117) every dim a fragment or verb needs must be filed at genesis; an
  unfiled session-minted `d.verb` hits the promoted-twin re-mint hazard
  `vagent-metaprogramming.md` §6.6 names (:1622-1625) — after checkpoint→resume the
  rebound `d.verb` DimRef differs and host-step dispatch silently misses on
  persisted segment cells.

- Evidence: `lineage.hpp:46-71` (three filing lists, no `d.verb`); repo-wide grep
  `"d.verb"` → zero hits; spec M0 filing list (:454-455).

- Fix: add `d.verb` to the M0 filing enumeration (and to `kAgentDimensionNames`
  when M2 lands). Side note on the same line: "recipe dims in `kAgentDimensionNames`"
  is loose — the recipe dims are already filed via `kMetaDimensionNames` and
  `d.clone` via `kGenesisDimensionNames`; the intent is "filed somewhere", so
  say "file … alongside the existing k* vocabulary" or name the array that grows.

### Finding 4 — MINOR — the clause-walk cite points at the wrong document

- Spec location: §3.8 (:299-302): "the pure-arena belief-match subcircuit the
  reference spec prescribes for `where` evaluation (`vagent-metaprogramming.md`
  §9.1's clause-walk lowering)".

- Problem: `vagent-metaprogramming.md` §9.1 (:2790-2806) is "A parameterized
  fragment, spliced" — no clause walk. The lowering table is
  **`vagent-agent-language.md` §9.1 "Cross-walk"** (:1479-1511); the `believes`
  row is at :1500. (Metaprogramming's own scope note — fragments containing
  `believes` "lower through the same executor lowering the artifact itself uses,
  which vagent §11.1 owes", :528-531 — punts this exact item *to this spec*.)

- Fix: cite `vagent-agent-language.md:1500` (and optionally `:1490` for
  `for believes`/`findall`).

### Finding 5 — MINOR — "the 18 DeclKinds" miscounts the enum

- Spec location: M0 (:456-457): "every one of the 18 `DeclKind`s gets an explicit
  lower/stub/refuse ruling".

- Problem: `enum class DeclKind` (ast.hpp:184-202) has **19** members: Event,
  Record, Predicate, Question, Capacity, Skill, Behavior, Agent, Goal, Plan,
  Method, Answer, Handler, Field, Uses, Install, BehaviorUse, Belief, Lifecycle.

- Fix: "all 19 `DeclKind`s".

### Finding 6 — MINOR (carried r1-F6) — §4.1 counter discipline still ambiguous

- Spec location: §4.1 (:329-333).

- Problem: unchanged from r1 — `msgStreak` is named in the change list but never
  used in the stated policy; "serve wake if `wakes>0 && (mailbox.empty() ||
  wakeStreak < ready_per_message)`, else message and reset" doesn't say what
  increments `wakeStreak` or what "reset" clears. The provable bounds only hold
  under the one intended discipline.

- Fix: two lines of pseudo-code (increment `wakeStreak` on a wake service, reset it
  on a message service; `msgStreak` symmetric or dropped).

### Finding 7 — MINOR (carried r1-F8) — §4.7's sweep still scopes the wrong file

- Spec location: §4.7 (:392-394): "`meta.cpp` internal maps get one sweep before
  M1 lands".

- Problem: meta.cpp's containers are all ordered (`std::map`/`std::set`/vector);
  the unordered containers in the path live in `vortex_vm.cpp` (membership sets)
  and `vortex_stdlib` (`varMap`/`routineBindings_` lookup maps). The work item as
  scoped finds nothing.

- Fix: "audit unordered containers in the vm/stdlib/meta path for iteration into
  observable order".

### Finding 8 — NIT (carried r1-F10) — §3.5's repoint cite still names only learn_body's lines

- Spec location: §3.5 (:263): "(`graft_body`/`learn_body` install a fresh named
  head — `meta.cpp:1850-1851`)".

- Problem: :1850-1851 is `learnBody`'s repoint; `graftBody`'s is :1345-1346. Also
  "install a fresh named head" is loose for `graft_body` — `installable`
  (:1222-1228) returns the argument unchanged when it isn't `d.code`-registered,
  so a grafted head need be neither fresh nor `+d.name`-carrying.

- Fix: cite `:1345-1346, :1850-1851`; "re-point `+d.spin` at an installed head".

### Finding 9 — NIT (carried r1-F11) — `d.seen` opcode range still cuts `Linked` mid-body

- Spec location: §3.1 (:156): "`Link`/`Break`/`Linked` opcodes suffice
  (`vortex_vm.cpp:745-829`)".

- Problem: `Link` :745-767, `Break` :768-782, `Linked` :783-851, `New` :853.
  `:745-829` stops mid-`Linked`; ring append also needs `New` (:853-898) to mint
  the journal cells.

- Fix: cite `:745-851` (plus `:853-898` for `New`).

### Finding 10 — NIT (carried r1-F12) — auto-reply still glosses the failure arm

- Spec location: §3.2 (:201-202): "`pumpOne` replies to every `ActorSend` with the
  executor's returned string".

- Problem: on failure the reply is an `ActorDown` envelope carrying a reason, not
  the string (`mesh_actor.cpp:595-610` — `ActorDeliverValue` vs `ActorDown` at
  :598).

- Fix: "— the returned string on success, an `ActorDown` on failure".

### Finding 11 — NIT — "actor stops forever" cite covers the mapping, not the stop

- Spec location: §3.2 (:199-200): "`pumpOne` maps it to `ExecutionFailed` and the
  actor stops forever (`mesh_actor.cpp:549-551`)".

- Problem: :549-551 is the `catch` → `ExecutionFailed` mapping; the stop is
  `actor->stopped = true` at :612-617.

- Fix: cite `:549-551,:612-617` or `:542-617`.

## Confirmed new claims (no action)

**§3.1 dim/marks gate (all verified):** `resolveDimension` refuses unfiled dims
under live marks at `meta.cpp:818-825` (find → `dimensionNamed` → marks refusal at
:820-824 → `ensureDimension`); `ArenaManifold::ensureDimension` at
`arena_manifold.cpp:78-105` performs **no** marks check and mutates the untrailed
`arenaDims_` (:89) — the spec's "ban lives at the vagent wrapper" is exact.
`d.clone` lazy-mints outside marks (`meta.cpp:34-38`) and is resolved by name
inside `reify` at :967 — the filed-at-genesis requirement is correctly derived.
Promote's frontier is bidirectional — `discover(edge.pos)` *and*
`discover(edge.neg)` both expand (`arena_manifold.cpp:1794-1796`), and `d.dims`
edges are skipped (:1779-1781).

**§3.4 weight-snapshot mechanism — verified end-to-end:** `setWeight`
(`meta.cpp:2038-2064`) mints/updates the record via
`varsRecord(binds_.agentCell, *label)` where `label = labelOf(clause)` = the
clause's `+d.name` text verbatim (:222-226, :2050-2053). `varsRecord`
(`records.hpp:69-79`) keys records by the record cell's **text**
(`textOf(record) == key`, `makeCell(key)`), value on `+d.values`. So the §3.4
snapshot — one `d.vars` walk building label→weight, then pairing each clause's
`+d.name` label in O(1) — is implementable exactly as described and preserves
`weightOf`'s semantics (:602-619) provided the snapshot uses the same
first-text-match rule (the rank shares keyspace with `meta.serial` —
`kMetaSerialKey`, meta.hpp:65 — `unlearn:` (:328,:334) and field records; the
aliasing is inherited, not new). O(V+C) claim holds. `+d.weight` junction arm
confirmed only for `d.synapse` cells (`sproutSynapse`, :2066-2083).

**§3.8 ABI consistency:** `runFragment` binds args name-keyed on the cursor's
`+d.vars` — variable cell per arg (text = param `+d.name`), `+d.values` → value
(`meta.cpp:736-756`) — the spec's marshal claim matches the existing convention
verbatim; rank save/restore at :762-783. `mintMetaBindSet`'s marks refusal
(:65-68) "applies only to its callers" — confirmed.

**§3.5 identity/replay:** `reify:` serials re-draw under genesis (d) replay —
`genesis.hpp:30-33` ("each install copy's own `reify:<src>:<serial>` still draws";
`patch:<n>` verbatim re-mints :33-34) — so recorded registered-head names are the
right stable identity; `learnBody` repoint :1850-1851 and `learnPlan` clause mint
:1732-1741 confirmed. `clausesOf` tombstone filter :210-220; `clauseHost`
:1167-1192; `graftPlan` clause shape :1396-1407.

**§5.2 accretion:** `spawnCursor` appends to the tail of `home`'s `+d.cursors`
rank via `rankTail` (`vortex_vm.cpp:242-245`); mesh spawns the cursor (:195)
before `genesisHook_` runs (:200-210); resume's hook at :333-347.
`commitStateSide` is `ref > extent ? Clears : Additive`
(`arena_manifold.hpp:278-281`) — home is a low base ref → never `Clears` → under
Additive **adds commit, clears don't emit** (`arena_manifold.cpp:2083-2087`), so
each incarnation's cursor append accretes durably. Confirmed defect, correctly
analyzed; `Suppress` exists as a policy. Directory-cell fix is consistent:
`home +d.dirs` is one add under Additive; a `+d.dirs`-only dirCell expansion
drags nothing else; tombstone-record delisting rides Clears on the state-side
agent cell (adds emit under Clears too — :2083-2087).

**§3.2/mesh mechanics:** `pumpOne` :510-621, mailbox-first :520-531, executor
callback :545-548, catch→`ExecutionFailed` :549-551, auto-reply :595-610;
`wake`/`pendingWakes` :781-793; `checkpointView` refusal conditions :829-855
(running/stopped/mailbox/wakes/`choiceDepth()!=0`/`outstandingMarks()!=0`/
containment/incomplete deliveries — V2-for-free confirmed);
`ActorExecution.message` `std::optional` (mesh_actor.hpp:89-97); `ActorGrant`
fields :55-65 — **no** `maxLiveCells` (spec's owed-not-cited correction is true);
`ActorOptions` :67-76 — no population caps.

**§3.2 turn contract vs sibling §4.2:** sibling ordering is mark→fuel→`$turn`→
base-CP→dispatch→settle→commit/undo→scope→drain→derive
(`vagent-agent-language.md:324-351`); all contract items appear in v0.2
(`compact_every` :187, scope unlink :185-187, base CP, Dewey reset,
`clearChoicePoints→release`, non-intention arm :197) — the *items* landed; the
ordering/defect is Finding 1.

**Emitter/milestones:** the scalar Emitter's `default:` refusal at
`compiler.cpp:714-716` ("construct is not supported by the scalar lowering
iteration") — all agent statement kinds (`ast.hpp:38-83`) hit it, so M0's "the
scalar Emitter refuses agent statement kinds" is true. `meta.hpp:196-614`
(`class MetaRuntime`), `fragment.hpp:166-168` (`compileFragment`),
`compiler.hpp:200-208` (`imageFloor_` set-once guard), `ast.hpp:184-230`
(`DeclKind`/`Decl`), `vagent_lineage_test.cpp:107-127` (vocabulary coverage),
`kAgentDimensionNames` 13 names (`lineage.hpp:67-71`), `seedProgramImage`
(:123-124), `finalizeProgramImage` (fragment.hpp:180), `fileDimensionNames`
(lineage.hpp:175), `defaultSettingSpecs` (system_docs.hpp),
`ActorExecutorBoundaryRemainsGated` (vagent_plasticity_test.cpp:1247),
`VortexStdLib::unify`/`deref`/`argumentsOf` (vortex_stdlib.hpp:223-225),
`bencode::decode` (bencode.hpp:113), `clearChoicePoints` exists
(vortex_vm.hpp:178), `pushChoicePoint` public (:174), `setCurrentDeweyPath`
(:185). Spec cites `vagent-metaprogramming.md:2808-2880` for §9.2/§9.3 — correct
(§9.2 :2808, §9.3 :2841-2880).

**§6.6 nuance (not a finding):** the executor's "§6.6 states the invariant the
naive form violated" is a fair gloss — §6.6's letter is the `d.code`-rank
reachability rule (`vagent-metaprogramming.md:1590-1595`); the naive
`home +d.agents` form violated the same bidirectional-frontier principle, which
§6.6 does state. Defensible.

VERDICT: NOT CONVERGED
