# R4 convergence check — all seven lanes on `design/vagent-actor-executor.md` **v0.4**

Scope per the round-4 brief and `r3-disposition.md`: verify the r3-B1 fix mechanically (base CP
pushed **before** `m0`), verify the M1 cite corrections and the M2 `+d.holder` rename, and hunt only
in v0.4 text. Dispositioned items are not re-opened. Verified against
`apps/common/xanadu/vortex/vortex_vm.{hpp,cpp}`,
`apps/common/xanadu/zigzag/arena_manifold.{hpp,cpp}`,
`apps/common/xanadu/vortex/{mesh_actor.cpp,vortex_stdlib.cpp}`,
`apps/common/xanadu/vagent/{meta.cpp,lineage.hpp}`, `apps/common/xanadu/zigzag/manifold.{hpp,cpp}`,
siblings `vagent-agent-language.md` / `vagent-metaprogramming.md`, and the v0.3→v0.4 git diff
(`82c1e45c..96c6dfc6`) inspected in full.

**Panel result: all seven lanes CONVERGED.** The r3-B1 reorder is mechanically sound end-to-end —
the failure path now needs no invariant of any kind. Findings: 0 BLOCKING, 0 MAJOR, 4 MINOR, 4 NIT,
all in v0.4 text.

______________________________________________________________________

## Shared priority verification — the r3-B1 exit protocol, walked end-to-end

### Ground truths (re-verified against code this round)

- `pushChoicePoint(cursor, altOp)` (`vortex_vm.cpp:279-309`): validates args **first** (`noCell`
  cursor :281-284; `altOp` must satisfy `getOpcodeKind` :285-288 — throws *before* any mark), reads
  `linked(*cursor, d.stack)` for `stackFrame` (:289-292), Dewey bookkeeping only when `oraclePath_`
  nonempty or `cutCallback_` installed (:293-298), then `choiceStack_.push_back` with
  `.mark = arena().mark()` at :302. **Nothing in it assumes an outstanding mark** — `mark()`
  (:1572-1594) has no preconditions, is `noexcept`, and the only arena mutation is the mark itself.
- `mark()` records extents (cells/links/content/scratch/trail/live counters/shadow/ proxy/quote
  counts) and pushes `Floors{cells,links,content}`; `outstandingMarks_++`.
- `discard(m)` (:1596-1610): `std::ignore = m` — **never reads the argument**; decrements,
  `floors_.pop_back()` unconditionally, `trail_.clear()` at zero.
- `release(m)` (:1612-1699): replays `trail_` back to `m.trailSize`, then
  `slots_.resize(m.cellCount)` / `links_` / `content_` / `scratch_` — `resize` **grows with default
  entries when `m`'s extents exceed current state** (the phantom-regrow wedge), restores live
  counters, repairs store-refs tails, then
  `if (outstandingMarks_ > 0) { --outstandingMarks_; floors_.pop_back(); }`. **Floor identity is
  never compared** — well-nesting is positional (`arena_manifold.hpp:843` "assumed well nested:
  release() and discard() pop").
- `trail()` (:1506-1527): write-guard consults `floors_.back()` only — a write to a cell with
  `dense < floors_.back().cells` records a `TrailEntry`; cells minted under the innermost mark are
  undone by truncation, not tra­il entries.
- `backtrack(cursor)` (:311-334): pops unconditionally; on match (`cp.cursor == cursor` or
  `!cp.cursor`) → `release(cp.mark)` (:319), Dewey restore (:321),
  `setCursorOpcode(*cursor, cp.altOp)` (:324), stack-frame relink (:325-328).
- `cut(n)` (:336-354): pops while `size() > n`, calling `discard(cp.mark)` per CP — **discard, not
  release**; `clearChoicePoints()` is literally `cut(0)` (:360).
- `pumpOne` opens no mark before the executor callback (:545-551); `checkpointView`'s quiescence
  predicate is `choiceDepth() != 0 || outstandingMarks() != 0` (:833-836) — the predicate the §3.2
  assert mirrors.

### The v0.4 ordering, floor-by-floor

Step 1 pushes the base CP: `floors_ = [F_mB]`, `choiceStack_ = [baseCP]`, extents `E_mB`. Step 2
takes `m0 = mark()`: `floors_ = [F_mB, F_m0]` — **F_mB sits below F_m0**, `E_mB ≤ E_m0`
componentwise (the arena only grows under marks; between the two calls only the Dewey reset runs,
pure VM state — `E_mB == E_m0` in practice). Scope edges follow under `floors_.back() = F_m0` →
trailed under m0. Steps 3–7 push per-statement CPs above `F_m0`.

- **Commit (step 8)** `cut(0) → discard(m0)`: `cut` pops stmt CPs LIFO — each `discard` pops its own
  floor (matched); pops the base CP last — `discard(mB)` pops `floors_.back()`, which is `F_m0`
  (**crossed, cosmetic**: `discard` ignores its argument and floor identity is never read; nothing
  writes between the two discards so `trail()`'s guard is never consulted on the wrong floor). Then
  `discard(m0)`: marks → 0, `trail_.clear()`. End state:
  `choiceDepth()==0 && outstandingMarks()==0`, floors empty, **writes kept**. The assert holds
  exactly.
- **Fail inside 3–7** `cut(1) → release(m0) → backtrack(cursor)`:
  - `cut(1)` keeps index 0 = the base CP, discards every CP above it. After:
    `floors_ = [F_mB, F_m0]`, `choiceStack_ = [baseCP]`.
  - `release(m0)`: `floors_.back()` is `F_m0` — **a matched pop, unconditionally**. m0 innermost is
    guaranteed structurally: anything pushed during steps 3–7 sits *above* `F_m0` (a mark cannot
    land between `F_mB` and `F_m0` — the floors stack is append-only) and is a CP mark removed by
    `cut(1)`. Rolls back every write since `m0` — scope edges, staged outbox, event cell, `d.seen`
    append — all trailed or truncation-undone. `floors_ = [F_mB]`, marks = 1.
  - `backtrack(cursor)`: pops the base CP, cursor match → `release(mB)`: `floors_.back()` is `F_mB`
    — **matched pop**; resizes to `E_mB ≤` current — a **pure shrink or no-op, never a grow** (it
    even undoes anything in the mB→m0 gap for free). Lands on `cp.altOp` (`fail_step`), restores
    Dewey + stackFrame — all at `outstandingMarks()==0` → untrailed, durable. End state 0/0. **The
    v0.3 wedge is gone, unconditionally — no `E_B == E_m0` invariant, no zero-write-gap
    requirement.**
- **Fail inside step 2**: floors `= [F_mB, F_m0]`, m0 innermost → bare `release(m0)` is a matched
  pop. **Safe as stated** — see Finding 1 for the missing continuation.
- **Fail inside step 1**: `pushChoicePoint`'s throws precede its `mark()` (:285-302) and
  `setCurrentDeweyPath` cannot throw → at most `mB` stands; there is no `m0` to release. Unaddressed
  in text — both calls are non-throwing in practice and a throw here is an invalid-pad adapter bug;
  NIT-grade window (noted under ERGO).

### Remaining mark-lifecycle paths — exhaustiveness check

- **Non-CP marks inside m0**: `meta.cpp` opens none (only two `outstandingMarks()` *reads* — the
  mint refusals :65, :820). `vortex_stdlib.cpp` has ~25 raw `mark()`/`release()` pairs, all inside
  the Vlog solver helpers §3.4 puts off-limits, all scoped; a `consumeWork` throw escaping one
  mid-scope is the adapter-bug class the zero-residual assert catches loudly. So "the popped marks
  sat above it" is exhaustive **under the contract** — a stray non-CP mark reintroduces the crossed
  pairing but is already a contract violation the assert trips on.
- **Stolen CPs**: `stealNearRootChoicePoint` (:374-387) removes `choiceStack_.front()` — index 0
  *is* the pad — plus `evictOldestMark()` pops `floors_.front()` (positionally consistent,
  `arena_manifold.cpp:2188-2194`). Its only caller is `OrParallelCoordinator::exportSparkFromVictim`
  (`or_parallel.cpp:192`) — no or-parallel machinery is named on actor VMs; §4.5's discipline
  excludes it. Boundary case, not a defect.
- **Leaked marks/CPs from a prior turn**: would put the pad above index 0 — the previous turn's
  zero-residual assert fires first → `ExecutionFailed` → `actor->stopped` (:612-617) → the next turn
  never runs. The invariant is enforced one turn late by the doc's own assert chain.
- **Conclusion**: under the spec's own contracts there is **no remaining path** where a CP mark
  outlives its floor or a floor outlives a CP mark.

______________________________________________________________________

## VORTEX — VM/arena semantics

The corrected words land:

- "base CP pushed BEFORE m0 so its mark mB sits below m0's floor" — **accurate** (F_mB below F_m0 on
  the floors stack; `mark()` at :302 vs :1572-1594).
- "release(m0) is then unconditionally well-nested" — **verified** (matched pop of `floors_.back()`
  in every arm under contract; see shared verification).
- "cut(1) discards every per-statement CP above the base … the base CP survives" — **exact**
  (`:336-347`; keeps index 0).
- "backtrack … consuming the base CP (popping its mark)" — **accurate**: `backtrack` pops the entry
  and `release(cp.mark)` rolls back to `E_mB` — here it *is* a release (rollback), and "popping" the
  floor matches `:1695-1698`.
- Drain-idiom provenance now correct: `vagent-metaprogramming.md` §6.4 (:1486-1519 — the drain rule
  itself :1510-1519) and the live precedent `vortex_stdlib.cpp:3789- 3801` (`goalProduce`:
  `base = choiceDepth()` → `pushChoicePoint(cursor, goalSentinel_)` →
  `while (choiceDepth() > base && backtrack(cursor))` — the same sentinel-CP-then-drain shape the
  spec adopts).

### Findings

1. **MINOR — "the §5.1 wedge" still misnames the hazard** (:204-205). A bare `release(m0)` over live
   inner marks does not produce §5.1's defect (a foreign CP popped *without* release → permanently
   elevated `outstandingMarks_`, `vortex_vm.cpp:315-318`). It produces the **regrow wedge** the
   sibling names: `release` resizes arena vectors *upward* with default entries when the passed
   mark's extents exceed the truncated arena (`arena_manifold.cpp:1657-1674`;
   `vagent-metaprogramming.md:1510-1515` describes exactly this). The sentence survives as half-true
   only because marks also elevate. Fix: "…would self-inflict the crossed-floor regrow §6.4 of the
   metaprogramming spec describes — `cut(1)` removes them first" (or name both defects).
1. **MINOR — the step-2 carve-out is an incomplete arm and mislabels itself** (:202-203). "A failure
   inside step 2 itself is pre-CP work" — step 2 is *post*-base-CP under the new order; it is
   pre-*inner*-mark. Worse, the bullet gives `bare release(m0)` as the whole response: read
   literally, the base CP and `mB` remain outstanding (`choiceDepth()==1`, `outstandingMarks()==1`)
   and the turn ends in residual marks → the zero-residual assert trips → `ExecutionFailed`. The fix
   is one clause: the 3–7 sequence already subsumes step 2 (`cut(1)` degenerates to a no-op), or
   "…then `backtrack` lands on the pad as usual." Also worth noting `fail_step` then still runs —
   the landing is not optional.
1. **NIT — "releasing their marks" / "releasing its mark"** (:186-187, :197-198).
   `cut`/`clearChoicePoints` call `discard(cp.mark)` — the savepoint is dropped and writes are kept;
   `release` is the rollback arm. The r3 wording fix changed "pops" to "discards" for the CP entries
   but left the marks described as "released" in both parentheticals — in a document where the
   discard/release distinction was itself the blocking substance, the verbs should read "discarding
   their marks" (harmless here because `release(m0)` follows, but precision is the point).
1. **NIT — §1 "the turn is one mark (`m0`)" and the §3.2 title "one mark, one decision"** (:50,
   :167): two marks are now outstanding for the whole turn (`mB` + `m0`). "One decision" is exactly
   right; "one mark" is now loose — "one decision mark plus the `fail_step` landing pad" if the
   framing is kept.

VERDICT (VORTEX): CONVERGED

______________________________________________________________________

## ARCHITECT — ordering, dispatch, deliverability

- **`+d.holder` resolution is unambiguous mechanically**: one edge per symbol → one `linked()` read
  (`arena_manifold.cpp` `linked`); emission once at M0 (:486-487) precedes its consumer at M1
  ("dispatch keys on it", :352) — **no milestone-order break**. The dim joins the filed list at M0
  before any mid-turn use, consistent with the file-before-mint rule.
- **Learned clauses inherit**: `learnPlan`/`graftClause`/`learnHandler` all attach the clause to
  `rankTail(*host, d.clause)` where `host` is the goal/event **symbol** (`meta.cpp:1739-1741`,
  `:1403-1405`; host resolution `clauseHost` :1167-1192). The holder edge on the symbol covers every
  clause on its rank — M2's "mid-turn `learn_*` mints ownerless" is genuinely solved: no stamping,
  no ownerless mints.
- v0.4's other edits touch no ordering claims: step renumber consistent with M1's "steps 1–10" and
  both arm shorthands.

### Findings

1. **MINOR — `+d.holder` arity and the absent-holder case are unstated** (:348-353, :486-487). "Each
   goal/event symbol carries `+d.holder → <agent>`" presumes one symbol per agent decl: if two agent
   decls in one module declare a same-named event (`agent W { on Attack … }` /
   `agent S { on Attack … }`), M0 either lowers to one shared symbol — which cannot name both owners
   in one edge — or must emit per-agent symbols (the uniqueness precondition is unstated).
   Symmetrically, a `learn_plan` targeting a symbol with no holder (a non-agent host — `clauseHost`
   resolves any module `d.vars` symbol) yields a clause whose filter semantics (match-all vs
   match-none) are undefined. One clause fixes both: "each symbol belongs to exactly one agent decl
   (M0 emits per-decl symbols); a host without `+d.holder` is refused by the learn verbs / matches
   all agents [pick one]".
1. **NIT — `<agent>` should name its target cell kind**: for the claimed "program→program edge" the
   target must be the **agent decl symbol** on module `d.vars` (the instance cell would be the
   state-side anchor). The V12 claim forces the program-side reading anyway, but one word
   ("`<agent decl symbol>`") removes the ambiguity.
1. **Carried (not re-opened)**: the actor-side comparison target — where the agent cell records
   "this actor is a `W`" — remains unstated (r3-architect finding 6 residual, now reads "+d.holder →
   its decl symbol" under the rename); §3.2 step 5's dispatch text (:181-182) still doesn't mention
   the filter.

VERDICT (ARCHITECT): CONVERGED

______________________________________________________________________

## CODEBASE — citation truth

The corrected cites verify:

- **`vagent-metaprogramming.md` §6.4** — section opens at :1486 ("Mutation under execution and under
  marks"); the drain rule is :1510-1519 ("the drain idiom, never a bare `release(m0)` under live
  marks … `while (choiceDepth() > base && backtrack(cursor))` … a `release(m0)` may run only when
  `m0` is the only outstanding mark"). Correct document, correct section.
- **`vortex_stdlib.cpp:3789-3801`** — `goalProduce`: `base = choiceDepth()` (:3789) → `spawnCursor`
  → `pushChoicePoint(cursor, goalSentinel_)` (:3794) → failure drain
  `while (choiceDepth() > base && backtrack(cursor))` (:3801). The sentinel is a minted
  `#GOAL_SENTINEL` Halt (:3072) — a literal landing-pad CP consumed by the final backtrack. The
  precedent is on point for both mechanism and shape.
- **`+d.holder` is free**: zero occurrences of `d.holder`/`"holder"` in `apps/common/xanadu/` and
  `include/` (only unrelated English "holder" comments — `provenance.hpp:77`, `buffer_pool.hpp`).
- **"(d.clone and the recipe dims are already filed — r3)"** — **TRUE**: `d.clone` ∈
  `kGenesisDimensionNames` (`lineage.hpp:48`); `d.recipe`, `d.refs`, `d.ref`, `d.entry` ∈
  `kMetaDimensionNames` (:58-62); `d.values`, `d.name` ∈ genesis (:49-50) — every `compileFragment`
  emission dim is filed. The prior mis-bucketing (listing them "in `kAgentDimensionNames`") is gone;
  the remaining list (:487) is six genuinely new names.
- **`d.owner` collision claim — TRUE**: the link-package layer decodes `owner` properties on
  `d.owner` (`manifold.cpp:1640` `dimensionNamed("d.owner")` + :1682 `link.owner`; documented
  `manifold.hpp:568`). The rename rationale is accurate, and `d.owner` survives in the spec only as
  that rationale (:349).
- **§5.1's defect description** still matches `vortex_vm.cpp:311-334` (the cite `:315-331` covers
  the pop/drop body — function bounds drifted slightly; carried).
- **`resolveDimension` marks refusal** — `meta.cpp:820-824` (cited :818-825) holds; `d.clone` minted
  lazily outside marks via `resolveDimension` — :34-38 + :965-968.

### Findings

1. **MINOR — "the §5.1 wedge" names the wrong defect** (:204-205) — shared with the vortex lane's
   Finding 1: the inner-mark `release` hazard is the crossed-floor regrow
   (`arena_manifold.cpp:1657-1674`, `vagent-metaprogramming.md:1510-1515`), not the foreign-CP mark
   leak §5.1 documents.
1. **NIT** — the r3-carried cite batch persists verbatim (`vortex_vm.cpp:745-829` still cuts
   `Linked` at :851 — :156; `mesh_actor.cpp:549-551` is the catch-map, the stop is :612-617 — :209;
   `meta.cpp:1850-1851` learnBody-only for the repoint claim — :272; `vortex_vm.cpp:315-331` vs
   :311-334 — :432). All dispositioned/carried; listed only for the record.

VERDICT (CODEBASE): CONVERGED

______________________________________________________________________

## ERGO — implementer completeness

**The exit protocol is implementable exactly.** Every named call is public and behaves as described
(`pushChoicePoint`/`cut`/`backtrack`/`clearChoicePoints`/
`release`/`discard`/`setCurrentDeweyPath`/`setExecutionLimits` — the last is `noexcept` counter
writes, `arena_manifold.hpp:471-477`). The reorder removes the only conditional step: nothing must
be true between `mark()` and the CP push anymore. An implementer reading the step list literally now
produces correct code under every arm — the failure bullet even survives the natural mis-fusion
(folding `$turn`/decode earlier) that broke v0.3, because both releases are pure shrinks under this
ordering.

**Remaining gaps — all MINOR-or-below:**

- **MINOR (shared, VORTEX F2)** — the step-2 failure arm lacks its continuation: after
  `bare release(m0)` the pad and `mB` are still outstanding; whether `backtrack`/`fail_step` follows
  is unsaid. One clause closes it.
- **MINOR** — no arm covers the step-1 window (CP push → `mark()`): both calls are effectively
  non-throwing (`pushChoicePoint` validates before it marks), so the exposure is `bad_alloc`-only —
  but the bullet's "inside 3–7" + "inside step 2" enumeration now leaves a formal gap where v0.3's
  "inside 2–7" at least covered it in name. One word ("failures in steps 1–2 before `m0` exists are
  fatal — nothing to roll back but `mB`") closes it.
- **Carried, per the r3 disposition (not re-opened)**: post-landing flow (whether steps 9–10 run
  after `fail_step`; whether `fail_step` stages into a destroyed outbox), the pad's `altOp` cell +
  cursor-binding convention (M3 — rides the §3.8 scope protocol + M1's assert), the sibling's
  drop-and-count / `remote_fault` arms, `$turn`/delivery-queue shape, `msgStreak` discipline.

VERDICT (ERGO): CONVERGED

______________________________________________________________________

## META — staging semantics

- **Symbol-side `+d.holder` satisfies the ownership requirement.** The three clause-minters all
  attach to the host symbol's `d.clause` rank (`learnPlan` :1739-1741, `graftClause` :1403-1405 —
  shared by `graftPlan`/ `graftHandler`; `learnHandler` same shape), and `clauseHost` resolves the
  name to that symbol (:1167-1192). Ownership now lives one hop up on the rank owner — mid-turn
  `learn_*` mints need no edge and cannot be ownerless. The M2 disposition's claim is verified in
  the attach path itself.
- **`priorAttachName` channel consistency — unchanged and consistent** (:1618-1644): walks
  `agentCell +d.patch` newest-first, matches patch `"target"` to the clause's `+d.name`
  (`patch:<n>`), returns head-names[0/1] per attachDim. Nothing in the holder change touches the
  patch-record channel; the §3.5 recorded-name comparand still rides registered-head names.
  Consistent.
- The emitted-once program→program edge promotes with the image fold and needs no replay handling —
  genesis (d) re-mints clauses onto already-holding symbols.

VERDICT (META): CONVERGED

______________________________________________________________________

## WRITER — document coherence

- **Step list ↔ milestones ↔ failure bullet: consistent.** Renumbered 1–10 (:169-195) matches M1's
  "steps 1–10" (:498); "Failure inside 3–7" + the step-2 carve-out covers the work span (steps 8–10
  are post-decision under §4.4); M1's `commit(cut(0)→discard) / fail(cut(1)→release→backtrack)`
  shorthand (:500-501) still reads exactly; "the step-2 scope edges" (:199) correctly attributes
  scope to step 2 under the new order. Header claim "the exit protocol rests on no unstated
  invariant" is **true**.
- `d.owner` appears only as the rename rationale (:349); all mechanism mentions say `d.holder`
  (:349, :486-487); the M0 filed list matches §3.8's.
- **mdformat `--check --wrap 100`: clean. `mdl -i`: clean** (0.7.22 / 0.18.1).

### Findings

1. **MINOR — "releasing" marks that are `discard`ed** (:186-187, :197-198) — shared with VORTEX F3:
   the CP entries are correctly "discarded"/"popped", but their marks are *discarded* (writes kept),
   and this document's whole r2/r3 substance was that discard ≠ release. Both parentheticals should
   say "discarding their marks".
1. **NIT — "one mark, one decision"** (:50, :167): two marks stand all turn now (shared with VORTEX
   F4).
1. **Carried (not re-opened)**: §9 still has no pending-confirmation block for the three §3.8
   rulings and the header's "(§3.8, §9)" pointer half-dangles (W2 — the rename is an r3 fix, not a
   ruling, so it correctly does not appear); the "reserved fuel (§4.3)" cite at :201 still points at
   the settle section (the reserve lives in §4.2's third bullet) — the rewrite carried the miscite
   forward verbatim.

VERDICT (WRITER): CONVERGED

______________________________________________________________________

## ZIGZAG — topology & promotion

- **`+d.holder` promote story: clean.** Symbol → agent-decl edge is intra-image; sealed and promoted
  verbatim with the program fold — and, unlike the r3 per-clause `d.owner`, the edge is emitted
  *once* at M0 on a program cell, so **no mid-turn mint ever creates a state→program edge**: learned
  clauses inherit ownership through their host rank with zero new edges. The V12 claim
  ("program→program edge, no V12 letter") is accurate.
- **Base-CP-before-m0 vs `spawnCursor` order: no interaction.** `spawnCursor` (:233-247) runs at
  `marks==0` — mesh-side before the genesis hook, or post-commit for staged spawn effects — so its
  `+d.cursors` appends are always durable or absent, never inside `mB`/`m0`. Mid-turn nested
  activations (`goalProduce` spawns its own cursor + sentinel CP inside `m0`) leave the `+d.cursors`
  link on pre-turn `home` trailed → rolled back by `release(m0)`, cursor cell truncated — consistent
  under both orderings. The §5.2 accretion defect is unchanged.
- **Recorded `stackFrame` is cleaner under the new order**: `pushChoicePoint` captures
  `linked(*cursor, d.stack)` at step-1 time — before *any* turn work, including the scope pushes —
  so the pad restores the true pre-turn frame (:290-292, :325-328). Equivalent before, strictly
  better as a baseline now.
- **Mark accounting**: see the shared verification — crossed-but-harmless only on the commit arm's
  `discard`s (identity never read); matched LIFO pops everywhere on the failure arm; both ends at
  0/0.
- **Boundary observation** (not a finding): `stealNearRootChoicePoint` steals `choiceStack_.front()`
  = the pad (`or_parallel.cpp:192` is the only caller) — excluded by the absence of or-parallel
  machinery on actor VMs; noted so the index-0 assumption is on record.

### Findings

1. **NIT — "releasing their marks"/"releasing its mark"** (:186-187, :197-198): shared wording
   residual (VORTEX F3) — `cut` discards CP marks.

VERDICT (ZIGZAG): CONVERGED

______________________________________________________________________

## Cross-lane summary

The r3-B1 reorder lands exactly as dispositioned and is **stronger than claimed**: both releases on
the failure arm are pure shrinks to matched floors, so the protocol no longer depends on *any*
extent equality — the "no zero-write-gap invariant required" parenthetical is verified, not
aspirational. The corrected provenance (§6.4 + `goalProduce`) lands, `+d.holder` solves the
ownership problem in the attach path itself, and the filed-dims claim is code-true.

What remains: two wording-level debts in the new failure bullet (the `discard`/"release" verb blur
and the `§5.1`-wedge misnomer), one incomplete arm (step-2 `bare release(m0)` with no stated
continuation onto the pad), and the `+d.holder` arity/absent-case preconditions — every one
MINOR-or-below and none load-bearing. This closes the round: **the document converges**.
