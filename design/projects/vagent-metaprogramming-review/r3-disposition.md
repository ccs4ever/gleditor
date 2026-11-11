# R3 disposition — design/vagent-metaprogramming.md v1.2

All seven r3 critics returned NOT CONVERGED: architect (1B/5M/6m), ergo (0B/3M/9m), meta
(0B/3M/~13m), codebase (1B/1M/8m), zigzag (2B/5M/9m), vortex (0B/2M/7m), writer (0B/2M/12m).
v1.2 discharged virtually every r2 finding in substance — the residual work is concentrated in
five shared mechanisms where the *fixes themselves* were under-specified or, in two cases, wrong.
Six lanes independently converged on the same four issues: the `d.validate` seen-set is degenerate
in **both** directions, name-bound calls have no executable form, `learn` registers program-side
originals, and `+d.refs` name-records cannot reach the slot they govern.

**Orchestrator admission.** Two r3 BLOCKINGs trace to r2-disposition rulings I wrote: the seen-set
reversal ("N backlinks on the marker for free" is mechanically false — `link` evicts both ends,
`arena_manifold.cpp:1352-1373`) and the "one shared `cell` marker" prescription (a shared marker
self-evicts; the precedent mints per-reference, `mesh_evaluator.cpp:735`). Both were verified wrong
against the tree this round. The correct mechanisms are ruled below.

## Rulings

### R3-A. `d.validate` seen-set — per-visited tag cells (architect-1, codebase-1, zigzag-1; writer-N1-confirmed mechanism family)

Neither arrow direction can work: one cell = one `+d` + one `-d` neighbour per dim, and `link`
clears the displaced end on both sides. Any scheme with a *shared* endpoint holds ≤1 mark.
**Rule: `visited +d.validate → tagᵢ`, where `tagᵢ` is a freshly minted cell per mark carrying the
pass serial as content.** Membership: `t = linked(v, d.validate, POS); t && textOf(*t) == serial` —
O(1) mark, O(1) test; each tag has exactly one `-d` neighbour (nothing evicts); a stale edge points
at an old-serial tag and fails equality; re-marking evicts the old tag harmlessly. Cost: one mint
per visited cell — priced in §7.2. Add `d.entry` to the traversal-dim enumeration (codebase-6) and
keep the trailed-marks clause (vortex-15).

### R3-B. Name-bound calls — int64 operand + name-record exemption + genesis rewrite (vortex-2, zigzag-2, architect-3, meta-7, ergo-F2, writer-R3-1)

Ruling **model (b)**: the Call operand is a normal emitter-minted int64 ref cell naming the program
op — no new execution machinery — but it is carried as a **name-record**, and the uniform marking
rule gets the carve-out the critics demanded:

1. **Unified `+d.refs` schema:** every record carries `+d.ref → the slot cell genesis must rewrite`.
   Discriminator: `+d.name` present ⇒ name-record; absent ⇒ ref-record.
2. **Pre-pass skip:** a record with `+d.name` never gets the `d.mesh.relocation` marker and is never
   OpHandle re-kinded (OpHandle-ing a program-side target makes `promote` fail — the target isn't
   `seen`, `arena_manifold.cpp:1797-1809`).
3. **Genesis rewrite chain, stated fully:** `symbol_named(qname) → the symbol's `+d.values` →
   head → `+d.entry` → entry op → `writeValue(slot, int64(entryOp))`. Unresolvable name →
   fault/drop, matching the dead-patch rule.
4. **Divergence clause:** session calls keep the binding written at emission; genesis rebinds — a
   learned fragment follows redefinitions *after* resume. Stated once.
5. The `closed` re-verification's "no int-encoded program references" becomes: no program refs
   *without* name-records. §9.3's `learn_plan` is this shape — state it works by design.

### R3-C. `learn` registers a fresh reify, never the original (vortex-1, zigzag-4)

`code"…"`-born heads are program-side artifact cells (they must be link-reachable in the artifact
to be sealed): registering one puts a program cell on `agent +d.code` — the invariant breach on the
*primary* path. **Rule: the registered head is always a fresh, uninstalled `reify`** — `learn_*`
produces two copies (installed + registered) for program-side args; a session-side arg still gets a
fresh registered copy for uniformity. Also:

- **Name the sideness test** the closed re-verification and `learn` use: a cell is program-side iff
  reachable from `program()` over the program's named-dim vocabulary (`d.stdlib`/`d.vars`/`d.clause`/
  `+d.grab`/`+d.spin`/`+d.step`) — bounded by `maxCycles`. State where fragment heads hang in the
  artifact (which is what makes them program-side).
- **`learn_*` of an installed head:** refuses like `graft_*` — the caller `reify`s first.
- **Re-registration:** `insertIntoRank` is idempotent (`arena_manifold.hpp:624-628`) — second
  `learn` of a registered head reuses it; the duplicate-label refusal applies only to *distinct*
  heads sharing a label.
- **Patch records name the registered head's `+d.name` label** — never the installed copy's
  `reify:` label (zigzag-3, ergo-F4, writer-R3-5): "each record `+d.values`-points at the registered
  head's name — the original replay re-resolves". `unlearn` re-point installs a `reify` of the named
  prior head (writer-R3-5); a `learn_handler` patch also records `priority` (zigzag-12).

### R3-D. P16 mutable-arena seam — the executor callback (architect-2)

`checkpointView`/`publishValue` are `const&`; the only mutable window into a `WorkerContext` is the
`Executor` callback during `pumpOne` (`mesh_actor.cpp:472-480`). **Rule: the adapter-as-executor
performs the ceremony inside its own callback, flag-driven:** the adapter sets a request before the
checkpoint-terminating `pumpOne`; the executor runs the re-kind at that turn's end (marks already
zero); `publishValue` runs between turns on the `const&`; the executor runs the restore at the next
turn's head. The session never *executes* `OpHandle` kinds — cells rest OpHandle only between
turns; reword the claim accordingly. Hygiene, all stated: every pass skips cells already in the
target kind (kind-check before `asInt64`/`handleTarget`); the restore runs on every exit path
(success, refusal, throw); the base-target arm (`arena_manifold.cpp:1809` `store.getCompactOp`) is
acknowledged with a required round-trip test (learn→checkpoint→resume→learn→checkpoint). The
`withContext`-hook alternative is recorded in §11.

### R3-E. Marker cardinality — per-ref-cell (codebase-2, architect-5)

"The marker cell is freshly minted per marked ref cell — sharing is impossible: `link` gives each
cell one `-d` neighbour and a second ref's edge would evict the first (the precedent mints
per-reference, `mesh_evaluator.cpp:735`)". Cost: +1 cell per ref cell, counted in §13.

### R3-F. `closed`/`state-side` fully defined (architect-4, meta-3/8, vortex-1, writer-R3-10)

- "State-side" = endpoint reachable through links AND not program-side (R3-C's `program()` walk).
  Captured `let` locals and parameters are **non-state-side** → a fragment capturing one is
  un-`learn`able — and §5.3's "stable" class shrinks to fields/module vars; frame-bound bindings
  (params + locals) read the binding's last-written value under the §5.8 ABI (ergo-F1 — locals move
  into the ABI-debt case; the scalar-kernel `localCells_` shape is noted as interim).
- A capture whose declared type is `Cell`/`Code` — or whose cell's content is a ref-box int naming
  a program-side cell — is **non-closed** (meta-3): refuse at `learn`, or require `d.name`/
  `code_named` resolution (state the durable-channel alternative).
- Boundary clause: the re-verification covers the enumerable frontier over named dims; an outside
  edge on a non-op-graph dim is a stated §11-door limitation (writer-R3-10).
- The `run`-inside-fragment durability clause: a `run` target in a learnable fragment must be
  name-resolved (`code_named`) — a captured `Code` value is a program ref under this vocabulary
  (meta-15).

### R3-G. Wrappers carry `+d.entry` (meta-2, vortex-6, zigzag-7)

The ref-box-Int entry contradicted `run`/attach/`validate` for zero benefit — wrappers are
session-side and unpromotable by construction. **Rule: wrapper heads carry `+d.entry → the wrapped
graph's root op` like every head; the clause-side memo key stays the attach cell.** Memoization:
`clause +d.code →` a `+d.step` chain of memos keyed by recorded attach (or state the single-slot
policy — reviser picks, but name the dim). For head-attached clauses the wrapper keys by the head.

### R3-H. Signature check includes parameter names (ergo-F3)

"Signature match = ordered `(name, type)` pairs + arity + result + purity (+ effect-ness for
`graft_method`)" — §6.3 and §7.2. Binding is name-keyed (`#Bind`/`#Resolve`, spec §5.8);
`signaturesEquivalent`'s type-only comparison is insufficient.

### R3-I. Template-`def` free names — stamps honored (meta-1)

The checker resolves stamp-carrying `Identifier`s to their resolve-pass bindings rather than
re-walking env — one resolution truth; `cap$N` env records remain only for invented names. State
that `cloneDecl` preserves stamps, and that M13's trigger is any `splice` of a `Code` param
anywhere in the `def`'s expansion (contracts and nested `code"…"` bodies included, meta-12).

### R3-J. Minor bundle — all accepted

Genesis ordering inserts **program-artifact bind** (programRef→root, V10 mechanism) before (c)
(vortex-3, zigzag-11). Release guard: "`outstandingMarks() == 1` under cp-only marking" (vortex-4).
`["result"]` row: name the second dim; recipe tuples as per-field records (codebase-3/4).
`value_of` row in the table (pure, `any`); `unlearn(clause_or_symbol: Cell)`, matching = patch
target's resolved name, empty-`+d.grab` window = test-less clause semantics stated (writer-R3-8,
zigzag-12/13); `run(Code)`→`unknown`; `Code` arity "0 or 1, wildcard when 0" (ergo-F8).
`run`/`value_of`/`splice`/`static` name-reservation extended to `let`/`var`/param dead-name
bindings (ergo-F10, architect-12). `splice` operand-peeling sentence (ergo-F10). `?`-export
rephrased; `occurrence` formals (ergo-F11). `run(c,a,b)` variadic §11 row (ergo-F12).
`plan_named` three-part + `:`-label parse rule + `reify` serial from a durable per-agent counter
(ergo-F7, writer-R3-11, meta-14, codebase-8). Shared weight scalars → each junction owns a distinct
scalar (zigzag-15); clause weights → agent-`d.vars` records keyed by clause qualified name —
never on the clause (zigzag-6). §8.5 pseudocode order + §8.3 retitle (writer-R3-12, ergo-F9d).
`prune` wording (writer-R3-13, zigzag-9). Post-resume `opcodeMap_` re-registration clause
(vortex-9). Call-frame carve-out naming (codebase-7). Splice-only fragments "emit no
head/descriptor" (meta-10). `static` in parameterized fragments judged per context (meta-11).
`+d.values` redundancy on ref-records resolved (zigzag-10 — `+d.ref` canonical). §13 phrasing +
`run` list-literal mint priced (architect-9/10). `d.code` invariant wording "program-skeleton cells
outside the fragment's own graph" (zigzag-14). `static`-inside-`code` allow-list note. Resume
imprecision: import leaves OpHandle-kind with remapped targets; genesis converts to Int64
(meta-16). Citation residuals: codebase-9's six + writer-R3-9's three. Examples: `import std:meta`
in every std:meta-using snippet, declare or elide-note all goals/events/verbs/predicates, §5.5
`Float`→`Int` arg fix (writer-R3-2, ergo-F9 — this is the last MAJOR).

## Register updates for v1.3

- **P15** — unchanged from v1.2 + post-resume `opcodeMap_` re-registration clause.
- **P16** — add: the seam is the adapter's `Executor` callback (flag-driven end-of-turn re-kind,
  head-of-turn restore); per-ref-cell markers; `+d.name` discriminator; the round-trip test.
- No new P-numbers: the name-record schema is descriptor structure, not a core change.

## Convergence assessment

r1: ~6B/30M/46m → r2: ~6B/30M/46m fixed, ~3B/15M/40m new → r3: 3B/18M/55m. The r3 BLOCKINGs are
mechanism repairs inside fixed architecture — no lane proposes re-architecting. Expected r4:
verdict possible once R3-A..R3-I land.

VERDICT: NOT CONVERGED — v1.3 required.
