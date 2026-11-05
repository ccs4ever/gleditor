# R3 codebase critique — `design/vagent-actor-executor.md` v0.3

Reviewer lane: citation truth and mechanism-claim verification. Round-3 scope per
`r2-disposition.md`: verify the corrected §3.2 exit protocol end-to-end (the round's one piece
of invented mechanism), the three new §3.8 rulings, the M0/M2 contract additions, and every
new file:line claim in the v0.2→v0.3 diff.

Counts: **1 BLOCKING, 2 MINOR, 1 carried-MINOR batch (unlanded nits).**

Bottom line: most of the new text is now code-true — the `cut`/`backtrack`/`pushChoicePoint`
API names and pop/barrier mechanics are exactly right, the two re-pointed sibling cites
(`:369-372`, `:1479-1500`) land, `signatureOf`/arity supports the `$step` ruling, the Vlog
term shape is genuinely `#Unify`-consumable, and the DeclKind count is fixed at 19. The one
remaining hard defect is in the piece of mechanism this round invented: the failure bullet's
`cut(1) → release(m0) → backtrack` sequence is presented as `agent-language:341`'s "drain
idiom" — but :341-342 prescribes the *opposite* arm (`clearChoicePoints()` + `release(m0)` +
opcode reset), and the mandated ordering runs `release(m0)` over a live inner mark, which the
metaprogramming spec's own drain rule forbids and which is safe only under an unstated
zero-writes invariant.

## r2 disposition landing check (codebase items)

| r2 finding | status |
| --- | --- |
| F1 (§3.2 failure arm: name/semantics conflation, inverted m0/CP ordering) | **PARTIALLY LANDED.** The ordering is fixed (m0 at step 1, base CP inside it — its mark now nests correctly under m0) and the API names are right. But the rewrite's provenance claim is false and the sequence rests on an unstated invariant — Finding 1. |
| F2 (belief term-cell encoding unnamed) | **LANDED.** §3.8:342-344 names it (`+d.grab` term heads, `#Unify`-consumable); M2:500-501 owns the emission contract; Vlog shape verified — Confirmed claim 5. |
| F3 (`d.verb` in no filed-dim list) | **LANDED.** §3.8:325 + M0:479 file `d.verb`; `d.verb` verified absent repo-wide. |
| F4 (clause-walk miscite) | **LANDED.** §3.8:305 now cites `vagent-agent-language.md:1479-1500` — verified correct (§9.1 Cross-walk :1479; `believes` row :1500; `for believes`/`findall` :1490). |
| F5 (18→19 DeclKinds) | **LANDED.** M0:481 says 19; `ast.hpp:184-204` counts 19. |
| F6–F11 (carried minors) | **Not landed** — text unchanged; re-listed under Finding 3. |

## Findings

### Finding 1 — BLOCKING — §3.2's failure bullet claims false sibling provenance, forbids the ratified arm, and runs `release(m0)` over a live inner mark under an unstated invariant

- Spec location: §3.2 failure bullet (:194-201); M1 (:492, same `cut(1)→release→backtrack`
  contract).

- Problem: four defects in the new text.

  1. **"This is `vagent-agent-language.md:341`'s drain idiom" — false.** :341-342 reads
     "On success: `clearChoicePoints()`, then `discard(m0)` … On a hard failure:
     `clearChoicePoints()`, `release(m0)`, **reset the cursor's opcode**, and run the
     recovery". There is no drain idiom at :341, no surviving-CP landing, and the
     turn-end table (:1511) repeats `clearChoicePoints(); discard(m0)`. The drain idiom
     is `while (choiceDepth() > base && backtrack(cursor))` — `vagent-metaprogramming.md`
     §6.4:1516-1519, the stdlib's `vortex_stdlib.cpp:3789-3801,:3829-3843`. The v0.2 text
     cited both sources; the revision kept only the wrong one.
  2. **"`clearChoicePoints()` is *never* used on the failure path" — false of the cited
     contract.** The sibling prescribes exactly `clearChoicePoints()` on hard failure
     (:342). The executor this spec designs is the §11.1/§4.2 contract's implementation;
     telling implementers the proven, ratified arm is prohibited on the authority of a
     line that says the opposite is a wrong-code-claim, not a stylistic deviation.
  3. **The mandated ordering runs `release(m0)` over a live inner mark — the hazard its
     own parenthetical mis-names.** After `cut(1)`, the base CP survives with its mark
     `mB` still the innermost floor (`floors_=[F_m0,F_mB]`). `release(m0)` therefore runs
     "under live CP marks" — the thing the bullet's own clause forbids — and the named
     wedge is the wrong defect: §5.1 is `backtrack` *dropping* a foreign CP without
     `release` (elevated `outstandingMarks_`, `vortex_vm.cpp:315-318`). The real hazard of
     releasing over an inner mark is the one `vagent-metaprogramming.md:1510-1515` spells
     out — "marks are assumed well nested" (`arena_manifold.hpp:842-843`), both `release`
     and `discard` pop `floors_.back()` unconditionally (`arena_manifold.cpp:1695-1698,
     :1596-1610`), so `release(m0)` consumes `mB`'s floor while applying `m0`'s rollback,
     and the surviving `mB` is left with recorded extents `≥` the truncated arena. The
     follow-on `backtrack` → `release(mB)` (`vortex_vm.cpp:319`) then resizes
     `slots_`/`links_`/`content_`/`scratch_`/`trail_` **upward** into ghost entries.
     It escapes corruption here **only** because nothing writes between `m0 = mark()`
     (step 1) and `pushChoicePoint`'s internal `mark()` (`vortex_vm.cpp:302`) — so
     `E_mB == E_m0` and `release(mB)` is a resize no-op that pops `F_m0`. That
     zero-writes invariant is load-bearing and nowhere stated or asserted: the sibling's
     own step order writes `$turn` *before* its CP push (`:332-336`), so anyone who
     re-fuses steps 2–4 to match the sibling silently breaks the arena (phantom cells,
     stale `liveLinks_`/`liveContent_`, a poisoned `trail_` for the next release).
  4. **Pre-CP-push failure gap + wording.** "Failure inside 2–7" includes failures before
     the pad exists — a throw during step-2's early writes leaves `backtrack(cursor)`
     returning `false` with no landing (the sibling's opcode-reset arm works regardless).
     And twice the bullet says `cut` "releases" marks (:184 commit arm, :195) — `cut`
     calls `discard(cp.mark)` (:340), which abandons the saved floor without rolling
     back; the discard/release distinction was the substance of r2-F1 and the text still
     blurs it.

- Evidence: `vortex_vm.cpp:336-354` (`cut` discards above the barrier), `:360`
  (`clearChoicePoints = cut(0)`), `:311-334` (`backtrack` releases `cp.mark` + lands
  `cp.altOp`), `:279-309` (`pushChoicePoint` records `.mark = arena().mark()` at :302);
  `arena_manifold.cpp:1572-1699` (`mark`/`release`/`discard` all pop `floors_.back()`
  unconditionally; `release` truncates *to the argument's* recorded extents);
  `arena_manifold.hpp:842-843` (well-nesting assumption);
  `vagent-agent-language.md:341-343,:1511` (the real arm); `vagent-metaprogramming.md`
  :1510-1524 (the drain idiom + "a `release(m0)` may run only when `m0` is the only
  outstanding mark"); `vortex_stdlib.cpp:3789-3801,:3829-3843` (the drain in code).

- Fix: either adopt the sibling verbatim — `clearChoicePoints()` → `release(m0)` →
  `setCursorOpcode(cursor, fail_step)` → recovery on reserved fuel (this makes :341 a
  true cite and removes the inner-mark release entirely) — or, if the pad-landing is
  kept, restate the order as `cut(1)` → `backtrack(cursor)` (its `release(mB)` both
  rolls back to the CP-mark extents *and* lands) → `fail_step` on reserved fuel →
  `discard(m0)`, and state explicitly that **no arena writes may occur between `m0` and
  the base-CP push**. Either way: cite `vagent-metaprogramming.md` §6.4 (not
  agent-language:341) for the drain rule, name the real hazard instead of "the §5.1
  wedge", say `cut` **discards** marks, and note that a failure before the CP push has
  no pad to land on.

### Finding 2 — MINOR — `d.owner` is a taken dim name; the M0 filing list also re-files already-filed dims

- Spec location: §3.8 (:343-345, "each clause carries `+d.owner → <agent symbol>`"); M0
  (:478-480, "file `d.agents`/`d.dirs`/`d.seen`/`d.evidence`/`d.verb`/`d.owner`/`d.clone`/
  recipe dims in `kAgentDimensionNames`").

- Problem: the clause-ownership *edge* is genuinely new — nothing today models
  clause→agent ownership (`clauseHost`, `meta.cpp:1167-1192`, resolves clause *names* to
  host symbol cells; `agentOwner`, :1194-1203, resolves owner-name strings for graft
  targeting; neither puts an edge on the clause). But the dim **name** `d.owner` already
  exists in the store layer as a link-package property: `store.cpp:1973`
  `ensureDim("d.owner")` + :2020-2035 `linkProperty(link.owner, dimOwner)`;
  `manifold.cpp:1634-1641` decodes it off `d.links`; `manifold.hpp:568` documents "owner
  on `d.owner`". Filing "d.owner" in `kAgentDimensionNames` is idempotent-by-name
  (`lineage.hpp:156`) so no DimRef twin arises, and the two uses never share a cell —
  but the spec presents it as a fresh dim without noting the overload. Separately in the
  same list: `d.clone` is already filed via `kGenesisDimensionNames` (`lineage.hpp:48`)
  and the recipe dims via `kMetaDimensionNames` (:58-62) — listing them "in
  `kAgentDimensionNames`" is wrong-array (harmless, still idempotent, but the r2
  disposition already corrected the `d.clone` rationale).

- Fix: note `d.owner` is taken and the reuse deliberate (same ownership concept on a
  different cell kind — or pick `d.agent-owner`), and restate the filing as
  "`d.agents`/`d.dirs`/`d.seen`/`d.evidence`/`d.verb`/`d.owner` join the filed
  vocabulary; `d.clone`/recipe dims are filed already via `kGenesisDimensionNames`/
  `kMetaDimensionNames`".

### Finding 3 — MINOR — carried r1/r2 minors still verbatim in v0.3

All verified unchanged; still worth folding into the next pass:

- §4.1 (:355): `msgStreak` named but unused; what increments `wakeStreak` and what
  "reset" clears remains unstated — the provable bounds hold only under the intended
  discipline.
- §4.7 (:417): "`meta.cpp` internal maps get one sweep" — the unordered containers in
  the path live in `vortex_vm.cpp`/`vortex_stdlib`; rescope to the vm/stdlib/meta path.
- §3.5 (:267): repoint cite is still only `meta.cpp:1850-1851` (`learnBody`);
  `graftBody`'s repoint is `:1345-1346`.
- §3.1 (:156): `vortex_vm.cpp:745-829` still cuts `Linked` mid-body (`Link` :745-767,
  `Break` :768-782, `Linked` :783-851, `New` :853) — cite `:745-851`.
- §3.2 (:204): "the actor stops forever (`mesh_actor.cpp:549-551`)" — :549-551 is the
  catch→`ExecutionFailed` map; the stop is `actor->stopped = true` at :612-617.
- §3.2 (:205-206): auto-reply still glosses the failure arm — on failure `pumpOne`
  emits an `ActorDown` envelope (`mesh_actor.cpp:595-610`, `ActorDown` vs
  `ActorDeliverValue` at :598), not the returned string.

## Confirmed new/changed claims (no action)

- **§3.2 API surface — all names exist and are public** (`vortex_vm.hpp:174-178,:211`):
  `pushChoicePoint(cursor, altOp)` (:279-309 — requires `altOp` be a valid opcode cell,
  :285-288, so "the fail_step landing pad" = the step-failure routine's op, matching the
  sibling's :334-336), `backtrack(cursor)` (:311-334), `cut(std::size_t)` (:336-354),
  `choiceDepth()` (:356-358), `clearChoicePoints()` (:360 = `cut(0)`),
  `setCursorOpcode` (:211), `setCurrentDeweyPath` (:369-372), `setExecutionLimits`
  (`arena_manifold.hpp:471`; called `mesh_actor.cpp:188,:314`). **Mechanics verified:**
  `cut(n)` pops while `choiceStack_.size() > n`, so `cut(1)` preserves index 0 = the
  base CP pushed first — "pops every per-statement CP above the base … the base CP
  survives" is exactly right. Commit arm `cut(0) → discard(m0)`: pops all CP floors
  LIFO, `discard(m0)` zeroes `outstandingMarks_` and clears `trail_` — end state
  `choiceDepth==0 && marks==0` holds, matching the sibling's success arm verbatim.
- **`vagent-agent-language.md:369-372`** — `ChildStopping{cause:"init"}` staged to the
  parent, executor returns error, `pumpOne` sets `stopped`, no retry: verified, the
  init/fatal arm is correctly cited.
- **`vagent-agent-language.md:1479-1500`** — §9.1 "Cross-walk" opens at :1479; the
  `believes t` row ("clause walk with `#Unify`, one `#Choice` per clause") is :1500;
  `for believes`/`findall` at :1490 — correct cite.
- **`signatureOf`/arity → `$step`-via-descriptor (M2:497-498)** — `signatureOf` exists
  at `meta.cpp:1143-1165` (builds `MetaSignature` from the descriptor's
  `parameterCells` + `result`/`pure` fields via `descriptorField`, :635/:642);
  `runFragment`'s arity refusal is :710-713 (`params.size() != args.size()` →
  `ArityMismatch`). Machinery supports the ruling exactly: `$step` cannot ride the arg
  vector undeclared (exact arity), and a declared `$step` formal would force every
  entry to supply it; descriptor fields (`result`/`produced`/`arity` precedent) are
  the existing channel. Callers of `signatureOf` (:1246,:1291,:1300,:1311,:1369,:1425)
  confirm it's the live signature mechanism.
- **Belief channel — agent cell bound on `+d.vars` (implicit arg) — consistent.**
  `runFragment` binds args name-keyed on the cursor's `+d.vars` (`meta.cpp:736-756`:
  variable cell text = `labelOf(param)`, value on `+d.values`, prepended innermost like
  `#BIND`), which is where `#BIND`/`#RESOLVE` walk (:736-739). An evaluator-bound
  implicit entry survives `runFragment`'s scope restore (`priorVars` recorded at
  :724-725, restored :762-771). No conflict with the declared signature *provided* the
  implicit arg is bound directly on `+d.vars` pre-call, never in the arg vector
  (:710-713 arity is exact) — which is what the spec's "on `+d.vars`" says.
- **`#Unify`-consumable `+d.grab` term heads — verified.** A Vlog term is a cell whose
  text is the functor, first arg on `+d.grab`, rest chained `+d.step`
  (`vlog.hpp:11-13`; `Vlog::makeTerm` `vlog.cpp:33-57`; `argumentsOf` :64-78 walks
  `grab`→`step`). `#Unify` → `Vlog::unify` (`vortex_vm.cpp:1190-1204`), which derefs
  `cloneMaster`, compares `valueKind`/bits, `textOf` functors, `argumentsOf` arity +
  recursion (`vlog.cpp:98-141`). A `+d.grab`-headed term in that shape is consumable;
  `deref`/`unify`/`argumentsOf` also exist adapter-side (`vortex_stdlib.hpp:222-229`),
  matching §3.4's "reuse `VortexStdLib::unify`/`deref` adapter-side".
- **`+d.owner` clause edge is new** — nothing models clause→agent ownership today
  (`clauseHost` `meta.cpp:1167-1192`, `agentOwner` :1194-1203 are name resolutions, not
  edges). Program→program edge, no V12 issue. See Finding 2 for the name-taken caveat.
- **19 `DeclKind`s** — `ast.hpp:184-204` counts 19 (Event, Record, Predicate, Question,
  Capacity, Skill, Behavior, Agent, Goal, Plan, Method, Answer, Handler, Field, Uses,
  Install, BehaviorUse, Belief, Lifecycle). M0:481 correct.
- **`d.verb`/`d.agents`/`d.dirs`/`d.seen`/`d.evidence`** — zero occurrences in
  `apps/common/xanadu/` code; genuinely new names to file.
- **§3.8 ABI/step-2 ordering** — base CP inside `m0` matches the sibling's
  mark-first nesting (`vagent-agent-language.md:324,:334-336`); per-turn scope on
  cursor `+d.vars`/`+d.stack` matches `runFragment`'s convention and the sibling's
  :344-347 unlink rule; `resolveDimension` marks-refusal verified at `meta.cpp:820-825`
  (filed names resolve via `findDimension`/`dimensionNamed` first, :818-819).
- **§5.1 cite** — `vortex_vm.cpp:315-331` covers the unconditional pop + cursor-test
  drop (function :311-334); the defect description matches the code.

VERDICT: NOT CONVERGED
