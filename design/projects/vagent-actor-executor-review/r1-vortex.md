# R1 Review — Vortex / VM-semantics lane

**Verdict: NOT CONVERGED.** Spec `design/vagent-actor-executor.md` v0.1 reviewed against
`apps/common/xanadu/vortex/vortex_vm.{hpp,cpp}`, `zigzag/arena_manifold.{hpp,cpp}`,
`vortex/mesh_actor.{hpp,cpp}`, `vagent/meta.{hpp,cpp}`, `vagent/records.hpp`, plus the sibling specs
`vagent-agent-language.md` and `vagent-metaprogramming.md`.

Counts: **1 BLOCKING, 3 MAJOR, 5 MINOR, 1 NIT.**

______________________________________________________________________

## Findings

### 1. §3.2's failure path `release(m0)` omits the choice-point drain — the §5.1 defect self-inflicted, plus ghost-slot corruption

**Severity: BLOCKING.** Spec §3.2 (:163-166): "**Failure inside 2–6** → `release(m0)`; outbox cells
die with the mark; the intention records the named fault…"

Dispatch runs bodies "under per-statement choice points" (§3.2 step 4) — V13 pushes one CP *and one
arena mark* per statement (`vagent-agent-language.md:252-258`, CP mark taken in `pushChoicePoint` at
`vortex_vm.cpp:302` → `mark()` → `outstandingMarks_++` at `arena_manifold.cpp:1592`). A hard failure
mid-statement leaves ≥1 CP marks outstanding above `m0`. `ArenaManifold::release(m0)` restores state
correctly (trail replay + truncation, `arena_manifold.cpp:1657-1676`) but decrements
`outstandingMarks_` **by exactly one** (`arena_manifold.cpp:1695-1697`) and pops exactly one
`floors_` entry — the *last* one, i.e. a stale CP floor, not `m0`'s. Consequences of the bare
release as written:

- `outstandingMarks_` stays elevated **permanently** → `checkpointView` refuses forever
  (`mesh_actor.cpp:834-836`), `compact()` refuses forever (`arena_manifold.cpp:1703-1705`), and
  `trail_`/`floors_` never reset → unbounded growth — §4.6's memory claim dies with it.
- `choiceStack_` retains entries whose recorded `Mark` extents now exceed the truncated arena. A
  later `backtrack` releases such a stale mark: `trail_.resize(m.trailSize)` and
  `slots_.resize(m.cellCount)` **grow** the vectors with default entries
  (`arena_manifold.cpp:1661-1674`) — phantom cells, a corrupted `slots_[0]` on the next replay. This
  is worse than §5.1's leak: same trigger, corruption instead of merely refusal.
- `cursorPC_` is untrailed (`vagent-metaprogramming.md:335-340`); release leaves the PC pointing at
  the faulting op. The sibling spec's sequence includes "reset the cursor's opcode"
  (`vagent-agent-language.md:341-343`) — §3.2 omits it.

Both sibling specs already name the fix. `vagent-agent-language.md:341-343`: "On success:
`clearChoicePoints()`, then `discard(m0)`… On a hard failure: `clearChoicePoints()`, `release(m0)`,
reset the cursor's opcode." `vagent-metaprogramming.md:1510-1519` (§6.4 drain idiom):
`while (choiceDepth() > base && backtrack(cursor))` (`vortex_stdlib.cpp:3801`,`:3841`), and "a
`release(m0)` may run only when `m0` is the only outstanding mark". §3.2 matches neither:
`clearChoicePoints`/`backtrack` appear nowhere in the turn steps, on either exit.

Related gap: the language spec's turn pushes a *base* choice point whose alternative is
`std:agent`'s step-failure routine (`vagent-agent-language.md:334-336`). §3.2 neither pushes nor
cuts one, so its commit path can't say why `choiceDepth==0` holds (V13 cuts only *successful*
statements' CPs; a base CP would survive every clean turn) — the assert in step 7/§4.4 is
unmotivated as written.

**Proposed fix:** amend §3.2 to the sibling idiom, verbatim-shaped:

- step 7: `clearChoicePoints()` (or `cut(base)` if a base CP is adopted) **then** `discard(m0)`; the
  assert `choiceDepth()==0 && outstandingMarks()==0` follows the drain.
- failure bullet: `clearChoicePoints()` → `release(m0)` → `setCursorOpcode` reset → fail_step,
  citing `vagent-agent-language.md:341-343` and the §6.4 drain idiom. If the language spec's
  base-CP/fail-routine mechanism is being *replaced* by the C++ catch-and-short-turn model, say so
  explicitly and explain how fail_step is entered.

### 2. §3.8 cites the wrong marks rule — and `d.clone`'s lazy mint refuses exactly on the gated mid-turn learn paths

**Severity: MAJOR.** Spec §3.8 (:249-256): meta-verbs run between segments, "cursor quiescent —
where `mintMetaBindSet`'s no-marks rule is satisfied by construction".

`mintMetaBindSet` refuses `outstandingMarks() != 0` (`meta.cpp:65-68`) — a condition that is
**never** true mid-turn: `m0` is outstanding from step 1 to step 7. The rule the statement- boundary
verbs actually live under is `MetaRuntime::resolveDimension`'s (`meta.cpp:809-826`): *filed* names
resolve fine under marks; only the `ensureDimension` **mint** refuses ("cannot be minted under live
marks"). The mutation surface is otherwise trailed-write- safe under marks by design
(`meta.cpp:1113-1121`: "no session verb needs a marks guard of its own") — rollback on `release`
even gives the wanted transactional semantics.

The pre-filing obligation has a concrete hole: `d.clone` is deliberately **not** in the bind-set
table (`meta.cpp:34-38`) — "reify resolves it by name and mints it lazily outside marks". Reify hits
`resolveDimension(kCloneDimName)` for any RefCapture member carrying a `+d.values` binding hint
(`meta.cpp:964-972`), and any recipe `DimAnchor` naming an unfiled dim (`meta.cpp:1049-1051`).
`installable`/`learnCopies` call `reify` (`meta.cpp:1222-1228`, `:1574-1589`) — i.e.
`graft_*`/`learn_*` invoked from a handler body, the exact §9.2/§9.3 gate — can refuse mid-turn for
want of a dim nobody filed. M0 already amends `kAgentDimensionNames`; `d.clone` is not on the list.

**Proposed fix:** restate the rule correctly: statement-boundary verbs are trailed-write-safe under
live marks; the enforceable constraint is `resolveDimension`'s no-mint-under-marks, so *every dim
any reachable verb or recipe can name must be filed at genesis* — add `d.clone` to the genesis-filed
set (M0 work item) and name the invariant. Note `mintMetaBindSet` itself can never run inside a
turn; that is a genesis-only API by construction.

### 3. §2.1/§3.4's clause weight channel is misstated — clauses carry no `+d.weight` edge; weights live in agent `d.vars` records

**Severity: MAJOR.** Spec §2.1 (:79-80): "A clause cell *is* a junction: `setWeight`/`weightOf` hit
the scalar `+d.weight` edge arm — O(1) trailed edge read/write (`meta.cpp:2038-2064`)"; §3.4
(:200-201): "collect `(weight, clause)` in one walk (O(1) `+d.weight` edge reads)".

`setWeight` (`meta.cpp:2038-2064`) takes the `+d.weight` junction arm **only when the edge already
exists**; otherwise a labelled clause falls to the clause arm — a `d.vars` record on the *agent*
cell keyed by the clause's `+d.name` (`varsRecord`, `records.hpp:69-79`: an O(|d.vars|) `rankAfter`
walk with a `textOf` string materialization per record, plus a mint on first write). Nothing mints
`+d.weight` on clauses: `graftPlan`/`learnPlan` mint name/grab/spin only
(`meta.cpp:1396-1407`,`:1732-1741`). `weightOf` (`meta.cpp:602-619`, not in the cited range) has the
same two arms, `+d.weight` edge first.

Two consequences:

- If M3 selection reads `+d.weight` edges per the spec, it sees *nothing* `set_weight` ever wrote —
  §9.3's "set_weight now actually drives selection" fails silently. The channels also diverge: a
  clause that later gains a `+d.weight` edge shadows its `d.vars` record (edge arm reads first) —
  split-brain weights.
- If selection calls `weightOf` instead, each candidate costs an O(|d.vars|) string-keyed walk, not
  O(1) — the virtual weight-order's priced cost model is wrong.

**Proposed fix (owner-level decision):** either (a) mint a `+d.weight` scalar at clause install
(graft/learn/declare) so the junction arm is the live one and §3.4's edge reads are honest — then
route `set_weight`'s clause arm to the edge for one channel; or (b) spec the weight read as
`weightOf` and price O(C·|d.vars|) per selection explicitly. Also fix the `weightOf` cite to
`meta.cpp:602-619`.

### 4. The cell budget does not bound `shadow()` — an unmetered `slots_` growth channel undermines §4.6's invariant

**Severity: MAJOR.** Spec §4.6 (:301-308): "`liveCells(turn boundary) ≤ grant.maxLiveCells`", and
§4.2's turn fuel "covers *all* constituents".

`allocationLimit_` is enforced only in `mintSlot` (`arena_manifold.cpp:1172-1177`, which also
`consumeWork`s). `ArenaManifold::shadow` (`arena_manifold.cpp:1465-1504`) pushes a slot copy of a
base cell **directly** — no `consumeWork`, no limit check. Every `link`, `writeValue`,
`setValueBits`, or unlink aimed at an unshadowed base cell grows `slots_` (and `liveLinks_` etc.)
with zero cost on the work axis and no cap on the cell axis — including in adapter C++ loops
(settle-round belief writes over B clauses, `clausesOf`-driven repoints), where not even the cycle
axis bounds it. The per-turn bound exists only via `maxCycles` for in-graph writes; C++ work is
unmetered entirely. `liveCells` can therefore pass any limit without a single throw — the §4.6
invariant cannot lean on the cell budget; only the drain-mode GC remains, which the spec already
admits is a named host duty.

**Proposed fix:** either count shadow copies against the cell account (a `consumeWork` +
`allocationLimit_` check in `shadow()` — a code change to price honestly), or state in §4.2/§4.6
that the cell budget bounds mints only, shadowing is a separate unmetered channel bounded by the
base's size, and the `liveCells` invariant is enforced solely at the checkpoint boundary.
Separately: `ActorGrant` has no `maxLiveCells` field (`mesh_actor.hpp:55-65`) — name the tunable
(`maxCells` or a new M0.5 `ActorOptions` entry).

### 5. §4.2's two fuel accounts are implementable only on one axis — the spec should name it

**Severity: MINOR.** `consumeWork` (`arena_manifold.hpp:481-487`) decrements the single shared
`workRemaining_` and throws a bare `std::runtime_error` — no account identity, as the review premise
states. `setExecutionLimits` (`arena_manifold.hpp:471-477`) is an absolute re-arm, not relative,
callable mid-turn at will (nothing enforces monotonic decrease). There is **no remaining-work
getter** — `executionLimited()` (`:478-480`) only tests presence. Consequences:

- "One aggregate condition-fuel account" works on the work axis: arm once at turn start, all
  constituents share the counter; exhaustion throws; catch per condition → defer remaining (§4.3's
  deferral is self-consistent — every later condition exhausts instantly).
- "Reserved fuel" for `fail_step` works only as absolute re-arm (set `workRemaining_` to the
  reserved amount on the failure path) — implementable.
- "Per-candidate `where` fuel drawn from the aggregate" is **not** implementable on the work axis:
  the adapter cannot measure remaining work, so it cannot hand out a sub-cap and restore the
  remainder. It *is* implementable on the cycle axis: `vm_.totalCycles()` (`vortex_vm.hpp:193-195`)
  is readable, so per-`runFragment` `maxCycles` = budget minus cycles burned this turn (matches
  §4.2's "draws from the turn's remaining fuel" for `maxCycles`).
- Note `workRemaining_` persists across rollback (`arena_manifold.hpp:470`) — "failed work costs
  fuel" is the implemented semantic; worth stating, since §4.3's accounting relies on it.

**Proposed fix:** §4.2 should name the axis per budget — cycles via `maxCycles`/`totalCycles()` for
per-constituent caps, the shared `consumeWork` counter for the aggregate allocation account,
absolute re-arm for the reserve — and say explicitly that per-candidate *work* sub-caps are out of
scope absent a remaining-work accessor.

### 6. §4.4's "field writes" can consume work post-commit — tighten to scalar writes

**Severity: MINOR.** §4.4 (:289-293): "drain = C++ `rankAfter` reads, journal bookkeeping = field
writes. Assert `choiceDepth()==0 && outstandingMarks()==0`…"

The reads claim is verified: `rankAfter`/`rankTail` are lazy views over `linked`
(`cell_views.hpp:237-271`), `linked`/`dimensionsOf`/`link`/`unlink` call no `consumeWork` (only
`intern` and `mintSlot` do inside the arena — `arena_manifold.cpp:1159`,`:1172`). But a
*string-valued* field write calls `intern` → `consumeWork` → can throw post-commit → escapes to
`pumpOne`'s catch → `ExecutionFailed` → `actor->stopped` (`mesh_actor.cpp:549-551`,`:612-617`).
Scalar writes (`setValueBits`) are free. The stated invariant ("mint/intern nothing") already covers
this; the example phrase "field writes" does not make the scalar-only constraint obvious.

**Proposed fix:** say "scalar field writes only — a string write interns and consumes work; any
string bookkeeping moves inside `m0`". Optionally extend the debug assert to trip on `consumeWork`
calls post-commit (a flag, not a counter).

### 7. §4.5/§5.1 understate the `backtrack` defect's second arm

**Severity: MINOR.** §5.1's claim is verified exactly: `backtrack(cursor)` pops
`choiceStack_.back()` unconditionally at `vortex_vm.cpp:317`; the match test at `:318`
(`!cursor || cp.cursor == cursor || !cp.cursor`) guards the `release(cp.mark)` at `:319`; a foreign
(non-null, different) cursor's CP is dropped with its mark — `outstandingMarks_` elevated
permanently → `checkpointView` refuses forever. Line range is :315-333 (spec says :315-331 — close).

Two additions for the defect record:

- §4.5's "`backtrack(nullopt)` matches any CP and would pop the actor's" is accurate but understated
  — with `!cursor` it also **releases the mark** (`:319`), silently rolling the actor's arena back
  to that CP's extent with *no* cursor updated (the `if (cursor)` arm at `:323-328` is skipped): a
  silent rollback of in-turn writes, not merely a pop.
- The `!cp.cursor` arm means an *unbound* CP matches **any** cursor — a foreign-cursor `backtrack`
  doesn't leak it, it *consumes* it and jumps the querying cursor to the foreign alternative
  (`:323-328`): cross-cursor hijack, a different failure mode worth one line.

### 8. Line-cite drift and the `d.seen` writer's actual opcode surface

**Severity: MINOR.** The `Link`/`Break`/`Linked`/`New` semantics cited for the ring writer check out
(`vortex_vm.cpp:745-898`): `Link` is a single-edge set-with-eviction or a `readLink` (`:745-767`,
onto `arena.link`'s displace-both-ends write, `arena_manifold.cpp:1349-1377`); `Break` removes the
(dim,dir) edge (`:768-782`); `Linked` materializes the rank as `+d.grab`/ `+d.step` ref-boxes with
`consumeWork` per member (`:783-852`); `New` mints+links, with a 4-arg append mode that walks to the
tail and **refuses on a cycle** (`:853-898`, ":877-882").

Caveats worth one line each in §3.1:

- `link()` is an edge-set, not a rank-insert: appending at the tail is read-tail + one link;
  evicting the head is read-head + read-next + one link — the displaced evictee is fully detached
  for free (`arena_manifold.cpp:1364-1373`). `insertIntoRank` exists
  (`arena_manifold.cpp:1423-1439`) but has **no opcode** — the emitted fragment must do the two-link
  surgery itself.
- If `↻` means a literal closed ring, `New`-append cannot serve (it refuses cycles) and `Linked`
  self-limits via its seen-set; `Link` surgery works either way.
- Cite ranges: `Linked` runs to `:852` (spec's :745-829 truncates it); `New` is `:853-898`.

### 9. §5.2's allocation profile: `render()` runs per *input*, not per text operand

**Severity: NIT.** §5.2 (:328-332) says "`render()` string materialization per text operand" —
`step()` renders **every** input for snapshot-before-write (`vortex_vm.cpp:1397-1399`) and every
output for postcondition evaluation (`:1478-1479`), regardless of type. The rest of the profile
verifies: `inputsOf`/`outputsOf` vectors (`:1383`,`:1402`), two `unordered_set`s (`:1384`,`:1464`),
`masterOf` walks inside `readValue`/`writeValue` (`vortex_core.cpp:183`,`:207`).

______________________________________________________________________

## Verified-as-accurate claims (for the record)

- §5.1's defect description (finding 7) — exact.
- §4.1: `pumpOne` serves `mailbox.front()` whenever non-empty; wakes decrement only at
  `mailbox.empty()` (`mesh_actor.cpp:520-531`); executor invoked in try/catch → `ExecutionFailed`
  (`:542-551`); `!result` → `actor->stopped` forever (`:612-617`); auto-reply to every `ActorSend`
  (`:595-610`). `wake()`/`pendingWakes` (`:780-793`).
- §2.1: `checkpointView` quiescence check incl. `choiceDepth()==0 && outstandingMarks()==0`
  (`mesh_actor.cpp:833-838`); `ActorExecution.message` optional (`mesh_actor.hpp:89-97`);
  `clausesOf` tombstone-filtered walk (`meta.cpp:210-220`); `clauseHost` (`:1167-1192`); clause
  shape minted identically by `graftPlan`/`learnPlan` (`:1396-1407`,`:1732-1741`); `learnBody`
  repoints `+d.spin` at `meta.cpp:1850-1851`; `commitStateSide` returns `Clears` only above `extent`
  (`arena_manifold.hpp:278-282`).
- §4.5's avoidance rule is correct and conservative: `goalBoundedFirst`/`goalEvery` spawn second
  cursors and push CPs bound to them (`vortex_stdlib.cpp:3790-3856`); the drain idiom
  `while (choiceDepth() > base && backtrack(cursor))` at `:3801`/`:3841` is the correct unwind
  shape. Interleaved CP pushes across cursors are what §5.1 makes unsafe; non-interleaved nesting at
  nonzero actor depth is actually safe, so the spec's rule is stricter than required — harmless.
- `VortexStdLib::unify`/`deref` exist adapter-side (`vortex_stdlib.hpp:223-224`).
- Mesh/VM determinism claim §4.7: `actors_`/`deliveries_`/etc. are `std::map`
  (`mesh_actor.hpp:287-291`); `step()`'s `unordered_set`s are membership-only dedup.

VERDICT: NOT CONVERGED
