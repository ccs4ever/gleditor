# r7-codebase: Round-7 codebase verification of `design/vagent-metaprogramming.md` v1.6

**Orchestrator summary (≤200 words):** All five of my r6 findings are correctly resolved, and the
R6-A/R6-B majors land mechanically sound. The image-first genesis ordering is real: a persistent
`CellRef` *is* the cell's MakeCell op index (`manifold.hpp:295` "`== this CellRef`"), so replaying
the program spool first preserves ids and lands program cells `0..E` below the recorded extent —
the discriminator holds by construction. The writeback split is right: `Call` executes
`toInt(inputs[0])` (`vortex_vm.cpp:854-856`, entry-valued), while `run` derefs `+d.entry`/`+d.values`
(§6.2, head-valued); `exportSymbol` binds `sym +d.values → entryOp` verbatim
(`vortex_core.cpp:119-127`). ~30 file:line cites in the v1.6 deltas re-verified — every one checks
out. One new wording-level error: "`home` is arena-minted and ephemeral" (:1344) — under a
checkpointed base `initGenesis` adopts `*base->home()` (`vortex_core.cpp:30-36`), a *persistent*
pre-extent ref (first MakeCell, `manifold.cpp:450-451`), which is exactly the resume incarnation
§6.6 governs. The exclusion conclusion survives via the stated unreachability arm, but the claimed
mechanism is wrong. Counts: **0 BLOCKING, 0 MAJOR, 1 MINOR, 1 NIT**.
**VERDICT: CONVERGED in substance** — one cosmetic mechanism fix remaining.

## Verdicts on r6 findings (codebase-1..5 + disposition items)

### Finding 1 (`d.stack` off-cursor / `home` misattribution) — RESOLVED

v1.6 :1342-1344 now reads "`d.stack` hangs off *cursor* cells (`vortex_vm.cpp:206`, `:865-887`),
never `home` — the cursors sit on `home`'s `+d.cursors` rank (`vortex_vm.cpp:159-160`)". Verified:
every `d.stack` edge is on `*cursor`/`frame` — `linked(*cursor, dims().stack)` :206 and :241,
Call's push `link(*cursor, …, frame)` + `link(frame, …, *oldStack)` :865-870, Return's pop
`linked(*frame, …)`/`link(*cursor, …)` :881-889 — and `spawnCursor` appends the cursor onto
`rankTail(home, d.cursors)` at :157-160 (literal comment "Onto the tail of home's +d.cursors
rank"). Attribution now correct. (The same sentence adds a new, separately wrong claim — see new
finding 1.)

### Finding 2 (`ensureDimension` "re-mints" claim) — RESOLVED

v1.6 :1310-1311: "genesis's `ensureDimension` is find-or-mint — `dimensionNamed` resolves base
dims and the `arenaDims_` cache before any mint, `arena_manifold.cpp:78-105`, so a promoted dim
cell re-binds rather than re-mints"; restated at :1587-1588 "(dims resolve from base/cache; only
absent names mint, `arena_manifold.cpp:78-105`)". Verified exactly: `ensureDimension` is :78-105;
`dimensionNamed` (:54-76) checks `base_->dimensionNamed` first (:58-69) then `arenaDims_` (:71-74);
the mint runs only at :87-89 after both miss. The genesis step (c) wording is accurate.

### Finding 3 (`d.ref`/`d.mesh.relocation` slash-join) — RESOLVED

v1.6 :1525-1526: "The mark is `+d.ref` on the record plus `d.mesh.relocation` on the cell — not
`+d.values`, because `+d.values` has occupancy semantics". The suggested fix applied verbatim.
`d.mesh.relocation`/`"cell"` strings confirmed at `foreign_op_handle.hpp:24-25`.

### Finding 4 (`writeValue` cite range) — RESOLVED

The P16 row (:2106) now cites `vortex_core.cpp:196-229` — the whole function (signature :196-198,
kind arms :203-227, `return cell`/`}` :228-229). `writeValue` writes only Double/Int64/Bool/None
bits — no `OpHandle` arm — so "no opcode can write `OpHandle` kind" is accurate.

### Finding 5 (dead `code_named`/`reflect`-born → `d.name` arm) — RESOLVED

v1.6 :1447-1450: "a `code_named`-resolved operand → its `d.name` label — while a `reflect`-born
operand is a session-side wrapper with no durable referent, so an opaque or `reflect`-born `run`
target in a learnable fragment is a closedness refusal at emission". The misnamed arm is dropped;
wrappers now hit the refusal, matching the memo-record shape (label lives on the memo record,
never the wrapper head).

### Disposition nit bundle — all applied and verified

- `manifold.hpp:154-158` now cites `ephemeralBit` (:154) *and* `isEphemeral` (:156-158) — the
  reviser's note is correct on this checkout (:1326).
- "left dead" → "left untouched" at :2239-2240 ✓ (the :821 "binding left dead" is a different,
  correct use).
- Per-record memo label freshness: :287-289 + :1371-1372, with the `makeCell(name)` precedent at
  `vortex_stdlib.cpp:4051` — verified (`CellRef nameCell = core_.arena().makeCell(name)` inside
  the per-var helper).
- "Identifying label" + label-less bare-op attach cells never memo-hitting: :290-293 ✓.
- Glossary `closed` gains the run-target label arm + two-arm refusal: :137 ✓.
- `patch:` removed from the live rebind shapes: :1486-1487 and the P16 row — the only remaining
  `patch:`/`reflect`-born occurrences are the historical v1.3/v1.5 changelog entries (:2399,
  :2461-2463), which correctly record what those revisions did.
- `vortex_vm.cpp:854-893` for "`run`'s frame writes" (:1089): Call push :854-876 + Return pop
  :878-893 — exact.

## R6-A mechanism verification — image-first genesis is sound

- `birthOp == CellRef` is literal: `manifold.hpp:295` "`std::uint32_t birthOp{0}; ///< 4: the
  MakeCell index; == this CellRef`". A persistent ref *is* a spool position, so "replay the same
  spool → same ids" (:1576-1579) and "the image's ops are the spool's low prefix" are
  mechanically real — the extent `0..E` boundary holds by construction, not by mint discipline.
- Step (b)'s `getOpcodeKind` cite `vortex_vm.cpp:102-114` verified (map-first then `asInt64`,
  bounded by `OpcodeKind::Linked`).
- Step (e)'s resume-vs-import dichotomy verified again: `resume` calls `receiveValue` then builds
  `WorkerContext(actor->checkpoint->manifold.get())` (`mesh_actor.cpp:260-271`, function opens
  :182) — no `importValue`; `importValue` demotes unmarked `OpHandle`→`None` and re-kinds marked
  cells via `local.at(*target)` (`mesh_value.cpp:485-501`); promote writes
  `isEphemeral(target) ? real.at(target) : target` (`arena_manifold.cpp:1947-1951`).
- Root exemption stated correctly: :1335-1336 "the walk prunes at the program-side boundary —
  its own root exempt: a program-side `agent_cell()` cannot prune the durable set" — matches the
  disposition ruling.

## R6-B mechanism verification — the writeback split is correct

- `Call` operand is entry-valued: `CellRef targetOp = toInt(inputs[0])` then
  `setCursorOpcode(*cursor, targetOp)` (`vortex_vm.cpp:854-856`,873). Writing `int64(entryOp)`
  reproduces the emission binding exactly.
- `run`-target slot is head-valued: §6.2 :1084-1087 — "the emitter writes `int64(head)` into it,
  and `run` derefs the head's `+d.entry` *and* `+d.values`" — consistent with the new split; an
  `int64(entryOp)` write there would indeed name an op cell where a head is dereffed.
- `exportSymbol` verified: `link(symCell, dims_.values, Posward, entryOp)` +
  `link(rankTail(moduleCell, d.vars), d.vars, Posward, symCell)` (`vortex_core.cpp:119-127`) —
  "sym `+d.values → entryOp` directly" exact; the "op else follow `+d.entry` once" discriminator
  is the honest generalization over head-valued `+d.values` targets.
- Rank-membership cite `vortex_core.cpp:113-126` (:1306) verified: both `getOrCreateModule`'s
  `link(rankTail(home, d.stdlib), …)` (:113-115) and `exportSymbol`'s `link(rankTail(moduleCell,
  d.vars), …)` (:124-126) are the `rankTail +d → member` convention on bare `d.` rank dims.
- `manifold.hpp:154-158`, `arena_manifold.hpp:468-470` (`refOf` = `ephemeralBit|dense`),
  `arena_manifold.cpp:1489-1490` (shadow birthOp = base ref), `:1759-1764` (handle→seen/frontier),
  `:1766` (`d.dims` skip), `:1797-1809` (deferred/`getCompactOp` gate), `:1352-1373` (one-`-d`
  eviction), `mesh_evaluator.cpp:735` (per-ref `makeCell("cell")` marker), `mesh_actor.cpp:462`
  (empty-mailbox skip), `:473-477` (`executor_` invoke), `:501` (stop-on-failure), `:659-662`
  (`pending`=`mailbox.size()`), `:672` (mailbox guard), `:668-694` + `mesh_actor.hpp:102-105`
  (`checkpointView` `const&`), `mesh_value.hpp:69-74` (`publishValue` `const&`), `worker_context.hpp:63`
  (`arena` public), `compiler.cpp:769-772` (`dimConstant` → `int64(findOrMintDimension)`),
  `compiler.cpp:853-855` (`localCells_`), `compiler.cpp:475-483` (compile-time anchor check),
  `vortex_stdlib.cpp:4051`, `cell_views.hpp:258` (`rankTail`), `arena_manifold.cpp:1423-1439`
  (`insertIntoRank`) — all exact. `vagent-agent-language.md:1670` is the P4 `wake(pid)` row;
  `ActorExecution::message` is still `ActorMessage`, not `std::optional` (`mesh_actor.hpp:78-84`)
  — "approved but unimplemented" accurate. The `'#' Ident` claim (:1643-1644) verifies against the
  lexer: `identifierPart` admits only `[a-zA-Z0-9_]` (`lexer.cpp:14-24`) — a user `#label` cannot
  spell `:` or `"`.

## New findings (v1.6 deltas only)

### Finding 1 — MINOR

- **Location:** :1343-1345 (inside the corrected `d.stack` sentence).
- **Problem:** "and `home` is arena-minted and ephemeral, not a program-side anchor". Under a
  checkpointed base `home` is neither: `VortexCore::initGenesis` sets `home_ = *baseHome` whenever
  `arena_.base()->home()` has a value (`vortex_core.cpp:30-36`), and `resume` constructs
  `WorkerContext(actor->checkpoint->manifold.get())` (`mesh_actor.cpp:270-271`), whose manifold's
  `home_` is the store's *first MakeCell* (`manifold.cpp:450-451`) — a persistent ref in the
  lowest positions, i.e., pre-extent. The ephemeral claim holds only for a base-less first
  genesis; §6.6's entire subject is the checkpointed-base incarnation, where it is false. The
  paragraph's conclusion — frames stay out of the durable set — survives on the *other* stated
  arm ("`home` carries no link the agent cell can reach"), since a persistent-but-unreachable or
  program-side home still excludes the frames behind it; but "not a program-side anchor" is now
  asserted rather than earned: under resume, home is a persistent pre-extent cell, and whether it
  classifies program-side turns on `program()`-reachability the passage does not discuss (if the
  adapter anchors the agent under `home`'s ranks and `program()` cannot reach `home` outbound, the
  either-direction state-side walk would see straight through it to `+d.cursors`/`+d.stack` — the
  unreachability arm is what actually carries the exclusion).
- **Concrete fix:** drop the ref-class claim and lean on the operative arm: "`home` is the
  session's cursor anchor — arena-minted on first genesis, the checkpoint's own persistent `home`
  after `resume` — and the frames stay out of the durable set because no state-side walk from the
  agent cell reaches it (if `home` *is* program-side, the same frames are excluded by the prune,
  so the conclusion holds either way)".

### Finding 2 — NIT

- **Location:** :2106 (P16 row, "Why it cannot live" column).
- **Problem:** "`promote` copies Int64 content verbatim and translates only links and `OpHandle`
  values (`arena_manifold.cpp:1934-1954`)". `ForeignOpHandle` values are also translated —
  `foreignOpTarget(from, arena)` → `store.makeForeignOpHandle(out.version, *target)` at
  `:1952-1954`, inside the cited range. The claim's point (Int64 refs verbatim) is right; the
  "only" list is one kind short.
- **Concrete fix:** "translates links and `OpHandle`/`ForeignOpHandle` values" or "translates
  only links and handle-kind values".

### Observation (not a finding)

The v1.5 changelog at :2461-2463 still describes `patch:` in the rebind shapes and the
`code_named`/`reflect`-born → `d.name` arm. That is a historical revision record — the v1.6 entry
directly below (:2488-2491) records the correction — so no change is required; flagging only so a
`patch:` grep isn't mistaken for live spec.

VERDICT: CONVERGED in substance — **0 blocking, 0 major, 1 minor, 1 nit**. All r6 findings and the
disposition's nit bundle are correctly resolved; the image-first ordering and the entry-vs-head
writeback split are the first fully mechanically-backed versions of both mechanisms. The single
minor is a wrong ref-class claim about `home` that the paragraph's other arm already covers.
