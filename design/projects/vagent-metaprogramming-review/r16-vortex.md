# Round 16 critique (vortex role): vagent-metaprogramming.md v1.15

Lane: VortexCore/VM execution semantics, store/spool positions, handle
relocation, promote, genesis pass order. Convergence check on the v1.15 deltas:
the recipe-tuple marked-int64 keyspace (R15-B), the unreachability restatement
of the agent-cell legs (R15-A), the (e) `Int64(resolved DimRef)` writeback, the
(c)-arm "a dim referent" wording, and the named `+d.ref`/`+d.refFor` pairing —
plus a fresh defect hunt over the `git diff` (25 hunks, ~200 changed lines).

## Verification of the r15 dispositions

### R15-B — the marked `+d.recipe` field cell rides `promote`'s relocation machinery end to end

The operative question was whether a `+d.recipe` field cell carrying marked
`int64` re-kinds `OpHandle` at checkpoint and rehydrates correctly. Walked
against the code, it does — mechanically, in one keyspace, with one exception
(finding 1):

- **Emission** (:1144-1155, :2041-2057, :1055-1071): the field cell is an
  emitter-minted `makeScalarCell(int64(srcPos))` plus its own
  `d.mesh.relocation→"cell"` marker edge; the `+d.recipe` field record is the
  inventory entry "beside `+d.refs`" (:2052-2055, :1078-1081). `+d.values` on
  the record is decorative, never load-bearing (:1151-1155) — consistent with
  :1176-1178 (external referents take no edge) and :2243-2244 (`+d.values` only
  ever annotates an internal endpoint).
- **Re-kind** (end of the flagged turn): the ceremony sets `OpHandle` naming
  `cellAt(int64)` — the endpoint. `isCellRelocationHandle`
  (`foreign_op_handle.cpp:109-117`, constants `:24-25` of the header) is then
  true: `OpHandle` kind + `d.mesh.relocation`→`"cell"` edge.
- **Promote BFS**: program root → head `+d.recipe` rank → record → field
  edges → field cell. `:1759-1764` — `isCellRelocationHandle` → `handleTarget`
  = endpoint → `seen`/`frontier` enqueue, so the endpoint lands in the durable
  subgraph even if nothing else reaches it. `d.dims` is the only skipped dim
  (:1766-1768); the marker edge and the record→field edge fold through the
  `real`-translated `setLink` pass (:2020-2053 — `edge.dim` and `edge.pos`
  both translated through `real`, :2030-2042).
- **Mint order**: field cell is `OpHandle`-kind ephemeral with a possibly
  ephemeral target — deferred until the target is `ready`, cycling to the
  birth-then-`setValue` path if need be (:1797-1828, :1984-2003). Non-ephemeral
  targets pass the `getCompactOp` existence check (:1809).
- **Fold**: `Int64` folds verbatim (`:1934-1936` — the R15-B failure arm, now
  unreachable for marked fields); `OpHandle` folds
  `makeOpHandle(version, isEphemeral(target) ? real.at(target) : target,
  textOf)` (:1947-1951) — the field cell lands in the image as
  `OpHandle`→`P_durable(endpoint)`, still `isCellRelocationHandle` since the
  marker edge folds with it.
- **(d)**: the donor's marked cells are still `OpHandle`-kind, (e) rehydrating
  last (:2276-2278) — so the recipe reader reads `*handleTarget`, the
  endpoint's promoted position (:1148-1149, :2271-2272 cites
  `arena_manifold.cpp:1947-1951` correctly). `handleTarget` reads the kind
  through `slot()` (:1091-1098).
- **(e)**: `writeValue(field, Int64(*handleTarget))` writes the promoted
  position back (:2271-2272); kind-guarded, so replay-minted copies'
  `Int64` fields no-op (:2285-2288).

The keyspace claim checks out per phase: in-session the field reads the live
session position; post-checkpoint both the tuple field and the ref-capture
slot read the *durable* position (`*handleTarget` at (d), rehydrated `Int64`
at (e)); the old→new map translates source→copy in whichever keyspace the
incarnation is in. Shared endpoints re-join on the same key (:1150-1151).
The in-session re-kind/restore window is the same one `+d.refs` slots already
live inside — no new exposure (`wingOf`'s `asInt64`-fail→`masterOf` decode,
`vortex_core.cpp:358-365`, never sees an `OpHandle` slot: the window is
between turns and (e) precedes (f)).

### R15-A — legs classify by unreachability (:2157-2171): verified, with one wording over-reach

- `d.clone` is absent from the walk's `+d` vocabulary (:1524-1526 lists
  `d.stdlib`, `d.vars`, `d.clause`, `d.extends`, `+d.values`, `+d.entry`,
  `+d.name`, `+d.refs`, `+d.recipe`, `+d.grab`, `+d.spin`, `+d.step`,
  `d.contract`, `d.source`, plus agent-side ranks — no `d.clone`). ✓
- The capture edge's direction is exact: `Clone` walks
  `linked(tail, d.clone, POS)` to the rank's posward tail and appends
  `tail -(+d.clone)→ cloneCell` (`vortex_vm.cpp:816-825`;
  `fromNegward(false)` = `POS`, `dim_vector.hpp:85-87`). First capture:
  `master +d.clone→ capSlot` — "the binding cell's own `+d.clone →`" —
  running state→program. `cloneMaster` = `rankEnd(NEG)`
  (`arena_manifold.cpp:1008-1012`), `masterOf` the `.value_or(cell)`
  wrapper (`vortex_core.cpp:344-346`) — the operand decode §5.3 states
  (`wingOf`'s lambda, `:358-365`), never a traversal.
- "Image-sealed for a declared fragment": §5.3 mints the capture slot at
  emission ("the emission wraps the binding cell in a capture-slot cell on
  the operand's wing", :824-826), so the edge is in the sealed image, not
  runtime-grown. For a session/reify capture the edge is runtime-grown —
  landing `>E_ops` anyway.
- "No agent-rooted second walk is owed — its only consumer is `closed`'s
  frontier enumeration, which does read the `cap$N` slot links and decode
  them through `masterOf`, not this test" — matches :1870 and :1898-1899
  exactly; `masterOf` resolves the binding through any clone chain
  (`rankEnd`, not one hop), so chained clones decode correctly.
- Both changelog echoes (:3643-3645, :3660-3665) carry the same corrected
  mechanism.

### Adopted minors — verified

- **(e) `Int64(resolved DimRef)`** (:2256-2264): `Linked` binds a `DimRef`
  (`const DimRef dim = toInt(inputs[1])`, `vortex_vm.cpp:673`);
  `ensureDimension`'s name arm resolves base/`arenaDims_` before minting
  (`arena_manifold.cpp:78-105`); a session-minted dim's `d.dims` rank edge
  is skipped at promote (`:1766-1768`), so `*handleTarget` names the orphan
  twin while `ensureDimension` re-mints the canonical — exactly the split
  the new arm states. For image dims the two arms agree (the dim's durable
  position *is* what `dimensionNamed` returns), so keying on the record's
  `+d.name` rather than sideness is correct and lossless.
- **Arm (c) "a dim referent"** (:1876-1879): position qualifier dropped;
  "the endpoint re-resolving to the named dim rather than carried as a cell
  position" reads correctly. Mirrored in the glossary row (:137) and the
  §6.1 dim-referent arm (:1179-1185 — "image-folded or session-minted alike,
  the record's `+d.name` rather than the sideness test selecting the arm").
- **Pairing named** (:1907-1908, :2274-2276): "a `+d.refs` name-record (slot
  paired by `+d.ref`) or the `+d.refFor`-paired `+d.recipe` ref-capture
  record's `+d.name`" and "the paired records' `+d.ref`/`+d.refFor`" — both
  pairings now named at both sites.
- **`set_weight` row + §8.1** (:1125, :2508-2512): the `+d.name`-less refusal
  is stated and the mint-on-miss glossed; consistent between row and section.
- **`wingOf` cite** `:366-369`→`:373` (:1163): `inputsOf` = `wingOf(POS)` at
  `vortex_core.cpp:372-374`; the `linked(opcode, dims_.grab, side)` entry is
  `:366`. Correct as adopted.
- **`(d)` "never the endpoint value" scoped** (:2245-2247): the
  `+d.name`-bearing dim record exception is now explicit and correct — the
  name *is* the endpoint source there.

## New findings (v1.15 deltas only)

### 1. MAJOR — the tuple `dim` endpoint is lumped under the position keyspace, but a session-minted dim lands the orphan twin the sibling arm just refused

**Location:** §6.1, :1145-1149; §6.6, :2051-2053; Appendix 1.15 entry,
:3707-3709.

**Problem.** The uniform-keyspace restatement makes *every* source-naming
recipe field a marked `int64` ref cell — the changelog names "tuple
`from`/`dim`/`to` endpoints" (:3708). For `from`/`to` the arm is right:
graph cells are either internal (map-translated) or external (verbatim
durable position, correct). But the `dim` endpoint is a *dimension*, and
this same revision just established that a dim endpoint must rebind by
name: the `dimConstant`-slot arm writes `Int64` of the `DimRef`
`ensureDimension` re-resolves — "`Linked` binds a `DimRef`, never a
promoted position, and `*handleTarget` names the dim-twin a session-minted
dim leaves orphaned off the folded `d.dims` rank … dead weight beside the
re-minted canonical cell" (:2259-2263). That rationale applies verbatim to
the tuple `dim` field: for a session-minted dim endpoint (`ensureDimension`
mints ephemeral, `arena_manifold.cpp:87-103`; `findOrMintDimension` the
same, `vortex_core.cpp:72-98`), the marked field folds
`OpHandle`→ the promoted orphan twin, `(e)` writes `Int64(twin)`, and at
replay the endpoint map-misses and binds *verbatim* — the dead twin —
while the copy's own `dimConstant` slots (rebuilt through the ref-capture
arm) carry the re-minted canonical. The copy then holds edges on the twin
and op operands on the canonical: `Linked`/`cells` reads through
`ensureDimension(name)` miss the copy's own edges on session dims. Base
dims are unaffected (a non-ephemeral endpoint self-maps at :1867-1869, so
position and name resolution agree) — the hole is exactly the class the
(e) fix was adopted for, one field over, and "one position keyspace …
translate either way" (:1149-1151) states the uniformity *affirmatively*,
so an implementer following the letter builds the twin-bind.

**Evidence:** twin formation — `d.dims` skip at `arena_manifold.cpp:1766-1768`,
`discover(edge.dim, false)` at `:1781`, `real.at(target)` fold at `:1949-1950`;
canonical resolution — `dimensionNamed`/`arenaDims_` at `:54-105`;
map-miss→verbatim is the external arm (:2281-2284).

**Fix.** Give the tuple `dim` field the dim-referent arm's channel: a
`+d.name` on the field record (or a field-level name cell) carrying the dim
name, resolved `Int64(ensureDimension(name))` at replay — the machinery and
the one-record-one-edge headroom already exist (a tuple record's `+d.name`
dim is free). Minimal wording at :1145-1151: "every field naming a source
cell … — the `dim` endpoint carried by name like the dim referent below:
position only ever names base dims canonically, a session-minted dim folds
off `d.dims` (§6.6 (e))" — and scope :2051-2053's "tuple endpoints"
accordingly.

### 2. MINOR — genesis (e)'s enumeration never names the `+d.recipe` marked-field arm it now owes

**Location:** :2254-2264; P16 row tail, :2907 ("genesis rehydrates in the
post-`d.patch`-replay `+d.refs` sweep").

**Problem.** The pass is titled "**(e) `+d.refs` rehydration**" and its
enumeration runs "per ref-record `Int64(*handleTarget)` … per name-record
the role-split rebind" — both `+d.refs`-side arms. The `+d.recipe`
source-naming field cells are marked identically and need the same
`Int64` writeback, but no sentence in (e) or the P16 row enumerates them;
coverage exists only implicitly via "each marked cell" (:2271-2272) and
the §6.6 inventory (:2051-2055, :1078-1081). The functional consequence of
a `+d.refs`-only sweep is small — an unrehydrated field stays `OpHandle`,
still `*handleTarget`-readable — but it leaves the field kind-divergent
from the `Int64` semantics :1148-1149 asserts ("the field reads the live
position in-session"), and an implementer building the sweep from §6.5's
pass list alone can read "every learn-registered head's records" as the
`+d.refs` records the bullet is titled after.

**Fix.** "(e) `+d.refs`/`+d.recipe` rehydration, last genesis pass — …:
per ref-record `Int64(*handleTarget)`, per `+d.recipe` source-naming field
cell the same writeback off its own record inventory; …".

### 3. MINOR — "the binding cell's own `+d.clone →`" is exact only for a first capture; a chained capture's edge runs clone→clone

**Location:** :2162-2164 (and the changelog echo :3643-3644).

**Problem.** `Clone` appends at the master's `d.clone` rank *posward tail*
(`vortex_vm.cpp:816-825`): for a second capture of the same binding the
new edge runs `prevClone -(+d.clone)→ newSlot` — and where `prevClone` is
another declared fragment's cap slot, the created edge is
program→program, not "the binding cell's own … running state→program".
Every operative conclusion survives (the edge is on `d.clone`, outside the
walk vocabulary; the binding is still never admitted; `masterOf` still
decodes through the chain via `rankEnd`), but the sentence asserts the
edge's *source* as a fact that is only true for the first capture —
sitting in the paragraph whose whole job is precision about what the edge
is and is not.

**Fix.** "the only edge a §5.3 capture creates is the `d.clone` rank's
posward-tail append `-(+d.clone)→` the fragment's `cap$N` slot — the
binding cell's own on a first capture, the prior clone's on a chained one
(`Clone` appends …)". The tail cell is at worst a sibling cap slot, so
nothing in the argument changes.

### 4. MINOR — the 1.11/1.12 changelog entries keep carrier claims the v1.15 retro-edit touched but did not correct

**Location:** :3539-3541 (1.11 entry), :3578 (1.12 entry).

**Problem.** :3540 still reads "`+d.values → the referent endpoint` for
internal/state-side referents" — under v1.15 a state-side referent takes
no edge at all (:1176-1178) and `+d.values` annotates an internal endpoint
conditionally (:2243-2244); and :3578 still reads "`+d.name` keeps the
program-side arm" — the same qualifier the 1.15 entry confesses dropping
"for the same reason" in the 1.11 entry (:3733). The 1.11 line was itself
retro-edited this round ("program-side referents"→"dim referents" on the
adjacent clause), so its surviving `+d.values` carrier claim now sits one
edited-word away from the convention the doc applies elsewhere: a touched
line gets corrected, an untouched line is history. Per the confession
convention these should either be corrected in place and confessed, or
bracketed "(superseded by 1.13's content-carried endpoint / 1.15's
decorative `+d.values`)".

**Fix.** At :3540 append "(the `+d.values` carrier superseded — content-
carried, 1.13; decorative-only, 1.15)"; at :3578 same qualifier, or drop
"program-side"→"dim" and confess in the 1.15 entry where the sibling fix
is already narrated.

## Nits (non-blocking)

- :2051-2055 — "the `+d.recipe` field record standing as their inventory
  beside `+d.refs`" could be read as the field cells *additionally* taking
  `+d.ref` records on `+d.refs`. "In place of a `+d.refs` record" would
  pin the inventory split.
- :1151-1153 and :1173-1175 — "the record's `+d.values` *may* name the
  endpoint / its own endpoint cell": for a two-endpoint link tuple the
  definite article is ambiguous (one `+d.values` edge, two endpoints);
  harmless since the edge is decorative, but "an endpoint" or scoping the
  decorative edge to single-referent records would close it.
- :1192 "its `cap$N` slots re-clone the same definition-site cells" —
  worth pinning that re-clone keeps `Clone`-append/rank-tail semantics:
  if a reader treats the binding→slot `d.clone` edge as a replayable link
  tuple, `Link(binding, d.clone, POS, slot)` evicts the master's existing
  `+d.clone` neighbour on a second capture (M14) and orphans the earlier
  clone's `masterOf` chain. The capture-record path implies the right
  thing; the tuple schema never says the clone edge is *not* a tuple.
- :2164 — "`Clone` appends it at the master's posward tail" attributes the
  sealed declared-fragment edge to the runtime op; for a declared fragment
  the compiler emits the same edge shape at emission. Cosmetic.

## Summary for the orchestrator

R15-B's core mechanism verifies against the code: a marked `+d.recipe`
field cell is discovered, deferred, folded `OpHandle`→promoted target, and
read/written back through the same `*handleTarget`/`Int64` channels the
`+d.refs` slots use — one position keyspace per incarnation phase, and
`+d.values` genuinely demoted (cites: `arena_manifold.cpp:1759-1764`,
`:1781`, `:1797-1828`, `:1867-1869`, `:1934-1951`, `:2024-2042`;
`foreign_op_handle.cpp:98-117`). R15-A's unreachability restatement is
mechanically exact — `d.clone` absent from the walk vocabulary
(:1524-1526), the append direction confirmed (`vortex_vm.cpp:816-825`,
`dim_vector.hpp:85-87`), `masterOf` the NEG-ward decode
(`arena_manifold.cpp:1009-1012`, `vortex_core.cpp:344-346`, `:358-365`).
The adopted minors all land correctly. One new MAJOR: the uniform
keyspace over-reaches by one field — the tuple `dim` endpoint inherits
the orphan-twin binding the sibling `dimConstant` arm was just fixed to
refuse. Three MINORs (the (e) sweep's unnamed recipe arm, the
first-capture-only "binding cell's own" edge source, two stale changelog
carrier claims) and four nits.

VERDICT: NOT CONVERGED
