# R3 Review — Vortex / VM-semantics lane

**Verdict: NOT CONVERGED.** Spec `design/vagent-actor-executor.md` **v0.3** reviewed against
`apps/common/xanadu/vortex/vortex_vm.{hpp,cpp}`, `zigzag/arena_manifold.{hpp,cpp}`,
`vortex/mesh_actor.cpp`, `vagent/meta.cpp`, plus sibling specs `vagent-agent-language.md` §4.2/§9.1
and `vagent-metaprogramming.md` M6.

Counts: **1 MAJOR, 4 MINOR, 1 NIT** on v0.3 text (+4 carried residuals noted, not re-opened). The
rewritten §3.2 exit protocol is *mechanically correct as literally sequenced* — commit verified
clean, failure verified correct **only under an unstated invariant** the doc's own rationale
contradicts.

______________________________________________________________________

## The critical verification — §3.2 steps 1–10 + failure bullet

### State model

`pushChoicePoint` takes an arena `mark()` inside the CP (`vortex_vm.cpp:299-308`, `.mark` at :302)
→ each CP costs one `outstandingMarks_` and one `floors_` entry (`arena_manifold.cpp:1572-1594`).
`discard` ignores its argument (`std::ignore = m`, :1597), decrements the count, pops
`floors_.back()`, clears `trail_` at zero (:1596-1610). `release` replays `trail_` and truncates
`slots_`/`links_`/`content_`/`scratch_` **to the *argument's* recorded extents** (:1657-1674), then
decrements and pops `floors_.back()` unconditionally (:1695-1697) — floor *identity* is never
compared; floors are consulted only by `trail()`'s write-guard via `floors_.back()` (:1506-1527).

Under v0.3's ordering (step 1 `m0 = mark()`, step 2 base CP): `floors_ = [F_m0, F_mB]`,
`outstandingMarks_ = 2`, `mB`'s extents ≥ `m0`'s componentwise (the mark is taken later; arena
vectors only grow under marks).

### Commit (step 8): `clearChoicePoints()` → `discard(m0)` — VERIFIED CORRECT

`clearChoicePoints()` is literally `cut(0)` (:360); `cut` pops every `choiceStack_` entry top-down
and `discard`s each CP's mark (:336-347). Under m0-first ordering the pops are perfectly LIFO:
`discard(mB)` pops `F_mB`, straggler statement CPs pop their own floors. Then `discard(m0)` pops
`F_m0`, marks → 0, `trail_.clear()` (:1606-1608). Post-state: `choiceDepth()==0 &&
outstandingMarks()==0` — the assert matches `checkpointView`'s real quiescence predicate
(`mesh_actor.cpp:834-836`). **The assert can and does pass; writes stand.** The r2 BLOCKING is
resolved.

### Failure (`cut(1)` → `release(m0)` → `backtrack(cursor)`) — correct *only* under an unstated invariant

- **`cut(1)`** — `while (choiceStack_.size() > 1)` pops top CPs + discards their marks (:336-340):
  keeps exactly index 0 = the base CP; stmt-CP floor pops are LIFO-matched. Spec: "pops every
  per-statement CP above the base … the base CP survives" — **VERIFIED EXACTLY**.
- **`release(m0)`** — replays `trail_` to `m0.trailSize`, undoing all trailed turn writes. The
  step-2 scope edges are trailed: `link` on the pre-turn cursor records a `TrailEntry` because
  `cursor.dense < floors_.back().cells` (:1510-1527), and post-m0 mints (staged outbox, scope
  cells, event cell) are deleted by `slots_.resize(m.cellCount)` (:1671). Spec: "rolls back all
  writes incl. the step-2 scope edges and the staged outbox" — **VERIFIED**. It does **not** touch
  `choiceStack_` — `release` is an `ArenaManifold` method; the CP entry is a plain
  `std::vector<ChoicePoint>` VM member (`vortex_vm.hpp:237`) — so the base CP *entry* survives as
  the bullet requires. **VERIFIED.** But it also pops `floors_.back()` unconditionally
  (:1695-1697), which under this ordering is **`F_mB`, the base CP's floor slot** — m0's extents
  are rolled back while mB's mark-stack slot is the one consumed.
- **`backtrack(cursor)`** — pops the base CP (:315-317); on a cursor match (`cp.cursor == cursor`
  or `!cp.cursor`, :318) calls `release(mB)` (:319), restores `currentDeweyPath_ = cp.deweyPath`
  (:321), `setCursorOpcode(cursor, cp.altOp)` (:324 — the fail_step landing), and relinks the
  recorded `stackFrame` (:325-328) at marks==0 → untrailed, durable — correct.

  `release(mB)` is the crux: after `release(m0)` the arena sits **exactly** at m0's extents; mB's
  recorded extents satisfy `E_B ≥ E_m0` componentwise. So `release(mB)` resizes each vector
  **from E_m0 toward E_B**:
  - `E_B == E_m0` (zero arena writes/mints between `mark()` and `pushChoicePoint`): all resizes
    no-op; pops `F_m0`; marks → 0. **Correct — lands on `fail_step`.**
  - `E_B > E_m0` in any component: `slots_.resize(mB.cellCount)` / `trail_.resize(mB.trailSize)` /
    `links_`/`content_`/`scratch_` **grow with default entries** (:1657-1674) — phantom cells that
    the *next* `mark()` records into the durable baseline, phantom trail entries that persist
    until the next discard-to-zero. **The §5.1 wedge, self-inflicted on every failed turn** — and
    the step-8 assert cannot catch it (it checks counts, which still reach 0/0).

  Under the spec's literal step order the gap is zero — `setExecutionLimits` writes only counters
  (`arena_manifold.hpp:471-477`), the Dewey reset is VM state (`:369-372`), scope edges come after
  the CP push — so `E_B == E_m0` and the sequence works. But only as an unstated, unasserted
  invariant (finding 1).

### "Its entry lives inside m0's span so a failed turn releases it too" — the rationale is wrong

Three different objects are conflated:

- the **`choiceStack_` entry** — VM state; `release(m0)` does not and must not release it (it is
  what `backtrack` lands on);
- the CP's **`floors_` slot** — consumed by `release(m0)`'s unconditional `pop_back()`, though the
  rollback applied was m0's;
- the CP's **`mark` extents** — released (as a no-op) by `backtrack`'s `release(mB)`.

The sentence is also internally inconsistent with the same bullet's "the base CP **survives**" /
"lands on the surviving base CP's failure alternative" — the entry cannot both be released by the
failed turn and survive to be landed on. What is true: the *mark accounting* drains across the two
calls because mB's floor slot is consumed by `release(m0)` and m0's by `release(mB)` — a crossed
but count-equal pairing that only works because `E_B == E_m0` makes the second release a no-op.

______________________________________________________________________

## Findings

### 1. §3.2 failure path rests on an unstated `E_base == E_m0` invariant; the stated mechanism is wrong, and the cited sibling's step order violates it

**Severity: MAJOR.** Spec §3.2 (:170-173, :194-201).

As traced above, `cut(1) → release(m0) → backtrack(cursor)` under the m0-first ordering is correct
iff no arena write or mint occurs between `mark()` and `pushChoicePoint`. The spec never states
this; the parenthetical justifying the ordering ("its entry lives inside m0's span so a failed
turn releases it too") misdescribes what happens; and the spec the bullet cites as its idiom
places a write **in that gap** — `vagent-agent-language.md:332-336` writes `$turn` (a mint for the
event cell) at step 3, *before* the base CP push at step 4. An implementer who folds decode/turn-cell
bookkeeping ahead of the CP — the natural cross-reading — gets `E_B > E_m0` and phantom-slot
corruption on every failed turn, silently, with the debug assert green (counts reach 0/0; only
extents grow).

The strictly better ordering already exists and was this doc's own r2 order: **push the base CP
first, then `m0 = mark()`**. The identical failure sequence then needs no invariant at all —
`release(m0)` pops the top floor `F_m0` (matched) and shrinks to `E_m0 ≥ E_B`; `release(mB)` then
shrinks to `E_B ≤ current` — always a shrink, never a grow, and it even undoes anything in the
mB→m0 gap for free. The only cost is commit-side `discard`s popping floor slots out of identity
order — but `discard` ignores its argument entirely (`std::ignore = m`, :1597) and floor identity
is never read, so the mismatch is cosmetic. v0.3 inverted the order to make the *commit* path
cosmetically LIFO and thereby made the *failure* path conditional on an equality it doesn't state.

**Fix (either):**
- (a) Preferred: restore `pushChoicePoint` before `m0 = mark()` (sibling order :324-336 with the
  scope edges still *inside* m0 — CP → m0 → Dewey reset → scope → work). The
  `cut(1) → release(m0) → backtrack` arm stands verbatim, unconditionally safe; add one clause
  noting commit's discards pop floors non-identically, which is harmless because `discard` never
  reads them.
- (b) Keep v0.3's order and state the invariant explicitly: "the base CP's mark must record
  extents identical to m0's — no mint or trailed write may intervene between `mark()` and
  `pushChoicePoint` (decode, `$turn`, and scope edges all come *after* the CP); a violation makes
  `backtrack`'s `release(mB)` regrow `slots_`/`trail_` with default entries — the §5.1 wedge
  self-inflicted, invisible to the counts assert." And correct the parenthetical: the *entry*
  survives `release(m0)` because `choiceStack_` is VM state; the mark's floor slot is consumed by
  `release(m0)` while its extents are no-op-released by `backtrack`.

### 2. The landing pad's two `pushChoicePoint` arguments are still unnamed — and a wrong guess fails or leaks

**Severity: MINOR.** Spec §3.2 (:171, :196).

`pushChoicePoint(cursor, altOp)` requires `altOp` to satisfy `getOpcodeKind` and throws otherwise
(`vortex_vm.cpp:279-288`) — the pad must be a real op cell (the entry op of `std:agent`'s
fail_step fragment, or a minted `#Call` that frames into it — the sibling names the *routine* at
`:334-336`, not which cell). The spec still says only "the fail_step landing pad" and assigns its
minting to no milestone (M1 lists "base CP … included" without the alt-op item; the `std:agent`
prelude ships in M0).

Cursor binding is likewise unspecified — and both choices have consequences under §5.1's known
defect: bound to the actor cursor, a stray foreign-cursor `backtrack` pops-and-drops the pad
without releasing mB (the leak); bound to `nullopt`, a foreign backtrack *consumes* it via the
`!cp.cursor` arm and jumps the foreign cursor to the actor's failure alternative — cross-cursor
hijack (:318, :323-328). A *mismatched* non-null binding is worse: `backtrack(cursor)` pops the
pad, skips the release, returns false — no landing *and* an elevated mark. The right answer
(actor-cursor binding) is derivable but should be stated in one clause.

### 3. "This is `vagent-agent-language.md:341`'s drain idiom" cites the wrong paragraph — :341 is the sibling's *different* mechanism

**Severity: MINOR.** Spec §3.2 (:198).

`:341-343` prescribes `clearChoicePoints()` (cut(0), killing the base CP) + `release(m0)` + *manual*
opcode reset + a recovery **short turn** (`:342`, `:363-366`) — not `cut(1)` + `backtrack` into a
preserved CP. The actual sibling precedent for the new mechanism is `:355-356`'s
contract-violation arm: "the adapter calls `VortexVM::backtrack(cursor)`, which lands in the
statement's failure path" — same jump-to-alt mechanics — plus `:334-336` for the base CP's
alternative. The text already concedes the divergence ("`clearChoicePoints()` is *never* used on
the failure path") so the cite should move to `:355-356`/`:334-336` — and worth one clause saying
v0.3's inline landing subsumes the sibling's separate "reset the cursor's opcode" and recovery
turn (it does: `setCursorOpcode` + `stackFrame` restore happen inside `backtrack`).

### 4. Step 2's micro-order puts the CP push before the Dewey reset — inverts the sibling, broken if path tracking is ever enabled

**Severity: MINOR.** Spec §3.2 (:172).

`pushChoicePoint` appends `0` to `currentDeweyPath_` and records the parent path + `1` whenever
`oraclePath_` is nonempty *or* a `cutCallback_` is installed (`vortex_vm.cpp:293-297`);
`setCurrentDeweyPath` then assigns over it (:369-372). The sibling's order is reset-then-push
(`vagent-agent-language.md:334-336`); v0.3 lists push-then-reset. Inert today ("vagent never sets
an oracle path", `:377`) — but a future cut-callback consumer would get a clobbered base path and
a `backtrack` restore inconsistent with the reset. One-word fix: list the Dewey reset first.

### 5. §3.8's fragment-purity ruling is right but justified by the weaker reason — purity is *forced* by the seam

**Severity: MINOR.** Spec §3.8 (:335-339).

The M6-narrowing claim verifies: `vagent-metaprogramming.md:271-278` explicitly allows `adopt`,
`emit`, `believe`, `install`, `graft`, `run` inside `code"…"` ("stay impure"). But "required,
because a `where` fragment must be safely re-runnable under rollback" understates it — rollback
undoes trailed impure writes just fine. The binding constraints are (a) the doc's own seam two
bullets up: **no host call can leave the fragment boundary at all** — host steps exist only at
statement boundaries where the executor interposes; inside a fragment graph there is no
interposition point until `Return`, so an impure verb has *no lowering* under this ABI; and (b)
observable accumulation: a `graft` inside a `where` would re-fire at every selection, committed or
not. The fix is one sentence reframing purity as forced, not preferred — it also hardens the
ruling against a future "impure verbs in fragments" proposal.

______________________________________________________________________

## Spot-checks (as tasked)

### §3.8 three rulings — no VM-semantics errors

- **ANF hoist (:329-334):** temps bound name-keyed on `+d.vars` are exactly `runFragment`'s arg
  channel (`meta.cpp:736-756`) and `#RESOLVE`'s read surface (`vortex_vm.cpp:1064-1078`); a
  hoisted verb's refusal hits the statement-level CP the :1485 lowering already emits
  (`#ChoiceDepth`/`#Choice`/`#Cut d`). Uniform boundary discipline is coherent. Only nit: the
  illustrative temp `?b` uses logic-variable spelling (`?` is term-internal per `:1473`); a real
  hoist temp is a gensym — NIT.
- **Fragment purity:** covered as finding 5 — ruling correct, justification underweighted.
- **Belief channel (:340-345):** implicit `agent` binding on `+d.vars` matches `runFragment`'s
  convention; the clause walk is plain arena navigation; `#Unify` exists (`vortex_vm.hpp:80`,
  impl `:1190`) and the reference's `believes t` lowering is "clause walk with `#Unify`, one
  `#Choice` per clause" (`vagent-agent-language.md:1490,:1500` — the `:1479-1500` cite lands).
  `+d.owner` edge = pure data. Term-cell encoding deferral to M2 consistent with §8.

### M2 `$step` / Return-tail contract (:496-499) — VERIFIED

- "a descriptor param fails `signatureOf`/arity checks" — `runFragment` refuses
  `params.size() != args.size()` (`ArityMismatch`, `meta.cpp:710-713`) and cross-checks the
  descriptor's `arity` record against the param chain (:703-709); `signatureOf` canonical-vs-
  candidate compares guard replace arms (:1291-1311, :1425-1426). A `$step` *formal* would break
  every resume call's arity and every signature compare — keeping it out of the signature is the
  correct consequence.
- "per-segment Return-tail" — `Return` pops the `+d.stack` frame and jumps the cursor to the
  frame's recorded successor (`vortex_vm.cpp:1019-1035`); `runFragment` mints a `Halt` successor
  per run (`meta.cpp:726-734`); M6 prescribes exactly this tail ("Fragment tails are `Return`,
  not `Halt`", `vagent-metaprogramming.md:268-270`). Segment → Return → landing → host step →
  next frame is consistent with the established call/spill convention.
- Phrasing nit: "`$step` reads through the segment descriptor" conflates cells — the persisted
  `step` value lives on the *intention record*; the *descriptor* carries emission data (segment
  heads / step→entry mapping). The substance (no formal param) is right; name which cell carries
  what.

### M1 milestone shorthand (:492) — now correct

"commit(**cut(0)→discard**) / fail(**cut(1)→release→backtrack**)" — matches the verified
mechanics; r2's backwards shorthand is fixed.

______________________________________________________________________

## Carried residuals (flagged r1/r2, still present — noted, not re-opened)

- §4.4 (:393): "journal bookkeeping = field writes" — string field writes `intern` → `consumeWork`
  → can throw post-commit; scalar-only qualifier still missing.
- §3.1 (:155): cite `vortex_vm.cpp:745-829` still truncates `Linked` (runs to :852) and omits
  `New` (:853-898).
- §4.5 (:398-400): `backtrack(nullopt)` also *releases* the matched CP's mark (:319) — silent
  rollback, not just a pop; still understated.
- §5.3 (:444): "`render()` … per text operand" — `step()` renders every input (`:1397-1399`) and
  output (`:1478-1499`); also §5.1's `backtrack` cite `:315-331` drifted to :311-334.

______________________________________________________________________

## Verified-as-accurate in v0.3's new text (for the record)

- API names all real and semantics as described: `mark`/`pushChoicePoint`/`cut`/`clearChoicePoints`/
  `backtrack`/`release`/`discard`/`choiceDepth`/`outstandingMarks`/`setCursorOpcode` — no misnaming.
- Commit path drains to `choiceDepth==0 && outstandingMarks()==0` — assert implementable and
  matching `checkpointView` (`mesh_actor.cpp:834-836`).
- `cut(1)` preserves exactly the bottom CP while releasing all marks above it.
- `release(m0)` rolls back trailed writes incl. scope edges and staged outbox; leaves `choiceStack_`
  untouched.
- `backtrack(cursor)` lands on the base CP: `release(cp.mark)` + `setCursorOpcode(altOp)` +
  `stackFrame` restore + Dewey restore, all at :315-331.
- `consumeWork` throws `std::runtime_error` (`arena_manifold.hpp:481-487`); `pumpOne` catch →
  `ExecutionFailed` (`mesh_actor.cpp:545-551`) — the §3.2 "nothing propagates" warning is accurate.
- "a bare `release(m0)` under live CP marks would self-inflict the §5.1 wedge" — correct
  mechanism.
- M6 narrowing, ANF marshaling, `+d.vars` implicit-arg convention, `signatureOf`/arity basis,
  Return-tail frame contract — all verified above.

______________________________________________________________________

VERDICT: NOT CONVERGED
