# R2 Review — Vortex / VM-semantics lane (freeze pass)

**Verdict: NOT CONVERGED.** Spec `design/vagent-actor-executor.md` **v0.2** re-reviewed against
`apps/common/xanadu/vortex/vortex_vm.{hpp,cpp}`, `zigzag/arena_manifold.{hpp,cpp}`,
`vortex/vortex_core.cpp`, `vortex/mesh_actor.cpp`, `vagent/{meta,lineage,genesis}.{hpp,cpp}`,
`vagent/p16.cpp`, plus sibling spec `vagent-agent-language.md` §4.2.

Counts: **1 BLOCKING, 2 MAJOR, 4 MINOR** in v0.2 text (+4 residual r1 minors noted, not re-opened).

______________________________________________________________________

## Disposition check — did my r1 findings land?

| r1 # | disposition | landed? |
| ---- | ----------- | ------- |
| 1 (BLOCKING, release-under-marks) | B2: drain then release on every exit | **Half.** Failure bullet gained `clearChoicePoints()` — but step 7's commit path did not (finding 1), and the drain named destroys the fail_step pad it claims to preserve (finding 2). |
| 2 (§3.8 marks rule + d.clone) | M-§3.8: corrected basis + ABI | **Landed** (§3.8's "trailed, not mark-quiescent" is correct; `resolveDimension` cite `meta.cpp:818-825` verifies at :820-824; `ensureDimension` no-check verifies `arena_manifold.cpp:78-105`). The `d.clone` rationale is now a fresh wrong code claim (finding 4). |
| 3 (weight channel) | B1 | **Landed, verified.** §2.1 "Weight storage" and §3.4's O(V) snapshot + O(C) pairing match `meta.cpp:602-619,:2038-2064`, `records.hpp:69-79`. |
| 4 (cell budget / grant field) | B8 | **Landed.** §4.6 states both halves as owed (`maxLiveCells` grant field, `liveCells()` counter — M0.5). |
| 5 (§4.2 fuel axes) | fold into v0.2 | **Landed.** "Fuel honesty" paragraph is exactly the r1 recommendation — virtual sub-accounts on the step axis, `consumeWork` left whole. Implementable (verified below). |
| 6 (§4.4 scalar writes) | minors folded | **Not landed** — §4.4 still says "journal bookkeeping = field writes" with no scalar qualifier. Residual MINOR. |
| 7 (§5.1 second arm) | minors folded | **Partially.** §4.5 says `backtrack(nullopt)` "would pop the actor's" — omits that `!cursor` also **releases** the mark (`vortex_vm.cpp:319`) — silent rollback, not just a pop; the `!cp.cursor` cross-cursor-hijack arm (:318) still unremarked. Residual MINOR. |
| 8 (cite drift :745-829) | minors folded | **Not landed** — §3.1 still cites `:745-829`; `Linked` runs to :852, `New` is :853-898. Residual MINOR. |
| 9 (§5.3 render per input) | minors folded | **Not landed** — §5.3 still says "per text operand"; `step()` renders **every** input (`vortex_vm.cpp:1397-1399`) and every output (:1478-1479). Residual NIT. |

______________________________________________________________________

## New findings (v0.2 text only)

### 1. §3.2 step 7's commit path never drains the choice stack — `discard(m0)` alone leaves the step-0 base CP + its mark forever (r1-B1's mechanism, now on the hot path)

**Severity: BLOCKING.** Spec §3.2 step 7 (:182): "commit: `discard(m0)` → writes keep, marks release;
assert `choiceDepth==0 && marks==0`".

Step 0 pushes the turn's base choice point (`:170`), and `pushChoicePoint` takes an arena `mark()`
inside the CP (`vortex_vm.cpp:299-308` → `outstandingMarks_++` at `arena_manifold.cpp:1592`).
Nothing else consumes the base CP on the success path — V13 cuts only *successful statements'* CPs;
the base CP is not a statement. At step 7, `choiceDepth ≥ 1` and `outstandingMarks ≥ 2` (base CP mark
\+ `m0`). `discard(m0)` ignores its argument and pops exactly one floor
(`arena_manifold.cpp:1596-1610` — `std::ignore = m`, `outstandingMarks_--`, `floors_.pop_back()`),
leaving the base CP's mark outstanding and `choiceStack_` non-empty. The same-line assert can
therefore **never pass**; in a non-debug build the mark and CP accumulate +1 per successful turn —
`checkpointView` refuses forever (`mesh_actor.cpp:833-838`), `compact()` refuses
(`arena_manifold.cpp:1703-1705`), and a later foreign-cursor `backtrack` would release the stale CP
mark and *grow* `slots_`/`trail_` with default entries (`:1661-1674`) — precisely the §5.1 wedge the
turn design exists to avoid, now triggered by every good turn.

The sibling prescription is explicit and the spec itself applies it one paragraph later:
`vagent-agent-language.md:341` — "On success: `clearChoicePoints()`, then `discard(m0)`". The
disposition (B2) said "on every exit"; v0.2 applied it only to the failure exit.

**Fix:** step 7 → "`clearChoicePoints()` **then** `discard(m0)`; assert …". (Under v0.2's step-0
ordering the base CP's discard pops `m0`'s floor rather than its own — see finding 3 — but `discard`
ignores the mark entirely, so the count still drains correctly and `trail_.clear()` fires at
`outstandingMarks_==0`, `:1606-1608`.)

### 2. §3.2's failure path names `clearChoicePoints()` — which is `cut(0)` and destroys the base CP the bullet says survives

**Severity: MAJOR.** Spec §3.2 (:191-196): "`clearChoicePoints()` (**backtrack-drain to the step-0
base CP**, which releases every per-statement mark), **then** `release(m0)` … `fail_step` runs
against the **surviving base CP** with reserved fuel".

`clearChoicePoints()` is literally `cut(0)` (`vortex_vm.cpp:360`), and `cut` pops the *entire*
`choiceStack_`, discarding every CP mark including the base CP's (:336-347). After it, no base CP
survives — the parenthetical misdescribes the named function (it does not drain *to* the base CP; it
drains *through* it), and "the surviving base CP" is a phantom. The sequence as written is
self-contradictory: the teardown half works (all marks released, `release(m0)` rolls back to `m0`),
but the landing pad the spec built in step 0 ("the fail_step landing pad", :170) is gone, and the
bullet offers no other entry into `fail_step`.

The mechanics for the evident intent all exist and compose cleanly under **v0.2's own ordering**
(base CP pushed *before* `m0`, floors `[F_mB, F_m0, …]`):

```
cut(1);                      // discard stmt CPs, keep the base CP  (vortex_vm.cpp:336-347)
release(m0);                 // rollback to m0; floors_.back() is now F_m0 — a true LIFO pop
backtrack(cursor);           // pops base CP → release(mB) + setCursorOpcode(cursor, altOp)
                             // + restores the recorded stackFrame (vortex_vm.cpp:315-331)
```

`backtrack` into the base CP does three needed things at once: releases `mB` (whose extents predate
`m0`, so this second release shrinks the arena further to the true pre-turn state — **rolling back
the step-0 `+d.vars`/`+d.stack` scope edges for free**), jumps the cursor to the `fail_step`
alternative (subsuming the sibling spec's separate "reset the cursor's opcode" step,
`vagent-agent-language.md:342`, which v0.2's bullet omits), and restores the pre-turn stack frame
(`:323-328`). Order matters: `release(m0)` must precede the `backtrack`, because `release` replays by
the *argument's* recorded extents and `mB`'s are below `m0`'s — reversing them would release `mB`
first then attempt `release(m0)` against a `trail_` already resized below `m0.trailSize`, growing
vectors with default entries (the §5.1 defect, self-inflicted).

**Fix:** replace `clearChoicePoints()` in the failure bullet with `cut(1)` (or
`while (choiceDepth() > 1)`) and add the terminal `backtrack(cursor)` after `release(m0)` — or, if
the sibling's recovery-turn model is intended instead, drop "surviving base CP"/"landing pad" and say
`fail_step` runs as a short turn, as `vagent-agent-language.md:363-366` describes. Also state
explicitly whether the base-CP alternative *is* `std:agent`'s `fail_step` routine (the sibling's
mechanism, `:335-336`) — v0.2 never says what the alt op is.

### 3. §3.2 step 0 places the base CP and per-turn scope *before* `m0` — the scope edges escape `release(m0)` and the failure arm never unlinks them

**Severity: MAJOR.** Spec §3.2 (:170-172): "0. base: push the turn's base choice point …; establish
per-turn scope (cursor `+d.vars`/`+d.stack`)  1. `m0 = mark()`…".

As sequenced, the scope edges are written **below `m0`**: `release(m0)` on the failure path does not
undo them. Step 9's unlink (:185-186 — "otherwise they leak onto the cursor rank every turn") sits in
the post-commit flow; the failure bullet jumps straight to `fail_step`/`$stats` and never says it
runs. Literal result: each failed turn leaves one dead `+d.vars`/`+d.stack` scope layer on a durable
cursor — the same accretion class §5.2 just catalogued. (The sibling spec avoids this by ordering:
`m0` at step 1, `$turn`/scope writes and the base CP *after* —
`vagent-agent-language.md:324-336` — so failure-path `release(m0)` reclaims all of it and the
scope-unlink step is a success-path no-op on the failure arm.)

Note the ordering interacts with finding 2: under v0.2's order, the corrected landing-pad sequence
(`cut(1); release(m0); backtrack`) makes `release(mB)` roll the scope edges back **for free** —
arguably why the author inverted the sibling order — but that only holds if the pad is actually
backtracked into, which `clearChoicePoints()` prevents. Two clean resolutions, pick one:

- **Keep v0.2's order** (justified only by the landing pad): failure arm = `cut(1) → release(m0) →
  backtrack(cursor)` per finding 2 — scope, base mark, and cursor PC all resolved in one motion.
- **Revert to the sibling order** (`m0` → scope/`$turn` → base CP): failure arm = sibling-verbatim
  `clearChoicePoints() → release(m0)` + recovery short turn; all CP floors sit above `m0`'s and every
  pop matches LIFO (v0.2's inversion makes `cut(0)` pop `m0`'s floor via the base CP's `discard` —
  harmless to the count since `discard` ignores its argument, but it is one more unmotivated
  divergence from the prescribed idiom).

Either way, the success path also needs finding 1's drain.

### 4. §3.1's `d.clone` rationale is a wrong code claim — `d.clone` is filed at `VortexCore` construction; the genuinely unfiled new dim is `d.verb`

**Severity: MINOR.** Spec §3.1 (:116-117): "`d.clone` and the recipe dims must be filed at genesis
too, since `d.clone` is minted lazily 'outside marks' today (`meta.cpp:34-38,:967`) and a mid-turn
`learn_*`→`reify` would refuse."

`d.clone` is minted onto `home`'s `d.dims` rank by `VortexCore::initGenesis` itself
(`vortex_core.cpp:57` via `mintNamedDimension`, `:18-29`), so `resolveDimension("d.clone")` returns
on the *filed* arm (`meta.cpp:818`, `findDimension` walks that rank, `vortex_core.cpp:89-95`) and
never reaches the mint arm a mark would refuse. The `meta.cpp:34-38` comment describes the lazy
*fallback* for arenas lacking the core genesis — dead code here. The *rule* (file every dim at
genesis) stays right; the motivating example is false, and §8-M0's filing list already carries the
redundant `d.clone`.

The actually-unfiled new dim is **`d.verb`**: §3.8's ABI has emitted segments carry `+d.verb →
<verb-name>`, yet `d.verb` appears in no filed set (`kGenesisDimensionNames`
`lineage.hpp:47-51`, `kMetaDimensionNames` `:58-62`, `kAgentDimensionNames` `:67-71`) and M0's
enumerated filing item (:454-455) omits it. Emission-time minting is mark-free so this bites only if
a verb edge is written mid-turn — still, §3.1's own gate says every semantic role is filed at
genesis: add `d.verb` to M0's list and fix the `d.clone` example to a name that is genuinely lazy.

### 5. §5.2's mechanism is confirmed but its "always in the fold" phrasing misdescribes the emission path

**Severity: MINOR.** §5.2 (:408-411): "`home` is a base cell always in the promote fold, so its
shadow's whole diff commits each cycle under Additive".

Independently verified: `spawnCursor` appends onto `home`'s `+d.cursors` rank
(`vortex_vm.cpp:242-245`) before the genesis hook on both spawn and resume
(`mesh_actor.cpp:195`/`:324`); `link()` shadows **both** ends (`arena_manifold.cpp:1338-1342`);
bidirectional frontier expansion verifies at `:1794-1796`; `commitStateSide(extent)` gives `Additive`
to refs ≤ extent (`arena_manifold.hpp:278-282`) — so `home` can never emit a clear. The defect is
real.

The imprecision: `home` sits inside the image band (persistent ref ≤ `imageExtent` — genesis
apparatus precedes the program band), so `regenesisCycle`'s `extraRoots` filter
(`!isEphemeral && ref > imageExtent`, `lineage.cpp:190-195`) **excludes** `home` — it is not "always
in the fold". The durable append actually rides two paths: (a) once any twin exists, the rank *tail*
is a post-image persistent — shadowed by each spawn's append, it *does* enter `extraRoots`, expands,
discovers the fresh cursor, and commits the edge (`:2054-2103`); (b) `home`'s own head-edge commits
only when `home` is fold-*reached* — e.g. under the new `d.dirs` design the first checkpoint walks
`agent →(neg d.agents)→ dirCell →(neg d.dirs)→ home` while `dirCell` is still ephemeral and
expandable. Worth one clarifying sentence so M0.5's fix targets the real emission path (a
Suppress-policy `+d.cursors` rank still works; genesis-unlink of dead twins still works).

### 6. Cross-reference drift in the new §3.2 text; M1 shorthand reads backwards

**Severity: MINOR.**

- Step 5 (:179): "one aggregate condition-fuel account (§4.3)" — the account is §4.2's first bullet;
  §4.3 is the deferral/`condition`-fault rule.
- Step 2 (:174): "coalesced, §4.1" — the coalesced-drain wake semantics are §3.3's ratified block;
  §4.1 is arbitration counters.
- M1 (:466-467): "commit/**clearChoicePoints→release**" — shorthand inverts the success path
  (release is the failure arm; success is `clearChoicePoints → discard`). Given findings 1–2, this
  milestone should spell the pair correctly: `clearChoicePoints → discard` on success,
  `cut(1) → release → backtrack` (or the chosen recovery) on failure.

______________________________________________________________________

## Verified-as-accurate in v0.2's new text (for the record)

- **`cut`/`discard`/`release` mechanics:** `cut` discards CP marks top-down (`vortex_vm.cpp:336-347`);
  `discard` = keep-writes/release-marks, ignores its argument (`arena_manifold.cpp:1596-1610`);
  `release` replays trail to the *argument's* extents and pops one floor (`:1612-1700`);
  `floors_.back()` also feeds `trail()`'s per-innermost-mark trailed-write test (`:1510-1523`) — which
  is why mismatched floor pops are only safe when no writes intervene.
- **Step 0/9 scope mechanics exist:** `runFragment` captures `priorStack`/`priorVars`, pushes the
  `#Call` frame on `+d.stack`, binds args name-keyed on `+d.vars`, and restores both heads post-run
  (`meta.cpp:720-783`); `#BIND`/`#RESOLVE` write/read the same rank (`vortex_vm.cpp:1037-1087`).
  Post-commit `unlink` is mint-free and `consumeWork`-free — step 9's unlink is implementable as
  written.
- **§3.8 ABI is consistent:** `+d.vars` arg/result marshaling mirrors `runFragment`'s established
  convention (`meta.cpp:736-756`); a `+d.verb`-named edge on an emitted segment descriptor is new but
  pure data — read-side under marks is fine; no conflict with how cursors bind inputs.
- **§4.2 sub-accounts implementable:** `totalCycles()` is a readable monotonic counter
  (`vortex_vm.hpp:193-195`, `:241`) — "steps-since-`m0`" is a snapshot-diff, no new counter required;
  per-constituent caps also ride `run(cursor, maxCycles)` (`:205`) / `runFragment`'s `maxCycles`.
- **§5.2 substance confirmed** (finding 5): spawn-before-genesis ordering, dual-end shadowing, the
  durable-tail accretion channel, and the Additive-never-Clears policy on `home` all verify.
- **§8 milestones** consistent with the §3.5/§3.8 mechanics: `+d.spin` repoint cite
  (`meta.cpp:1850-1851`) and registered-name recording match the code; M2's segmentation-first
  ordering is coherent with the two-position lowering.

VERDICT: NOT CONVERGED
