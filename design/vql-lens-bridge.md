# VQL Lens Bridge: Lenses as Step Functions in a Declarative Query Language (v2.2)

A design specification for how a **VQL program** — not the `vquery` REPL — opens, traverses, mounts,
writes back, and releases projection lenses. Design deliverable only: the signatures are proposals;
the engine's generator-anchor (`lens` open) and step-function (`path/fn(...)` mutate/read) machinery
is the seam, not a new layer. Round 1 critique folded in.

VQL is **declarative**: a query produces a relation (a set of cells). Lens ops split two ways: the
**open** is a generator anchor (produces cells from nothing); every **gesture on a cell** is a
**step function** (`path/lensmount`, `$cell/lensnext`) — the existing `FunctionInvocation` step
dispatch, the same surface the mutating `link(...)` step already uses. No new grammar.

______________________________________________________________________

## 1. Intent

A VQL query should be able to write, in its own terms:

- **open a source** as projected root cells — `dir:`/bare path, `git:`, `sql:`, `cfg:`, `log:`,
  `journal:`, `union:` — the `openLensUri` scheme table (`lens_uri.cpp:23-124`);
- **traverse** with the cell-path syntax it already uses — `$root/d.contains/d.seq/d.role`;
- **mount a delegate** under a projected cell and come **back** — "back" is a bound variable;
- **edit, commit, and revert** projected cells on write-capable lenses (`sql:`/`cfg:` today);
- **materialize** a deferred page, and let sessions go out of scope.

Acceptance journey: `open dir → query listing → mount git under the repo cell → query commits →
release mount → keep querying the dir → edit+commit on a sql: lens → revert a refused edit →
materialize a page → query ends`.

## 2. Current surface (already in the build)

| surface | what it does |
|---|---|
| `:lens <uri>` / `:lens mount|unmount|next|commit` | REPL verbs — `LensRepl` on the coordinator's core |
| `--lens <uri>` | programmatic raster — `runLensRaster` |
| `$lens` | `setVariable("lens", *lensRepl.root())` — the open lens's root bound in `env_` (`vquery/main.cpp:525-526`) — binds `roots().front()` only (`lens_repl.hpp:82-86`) |
| `path/fn(...)` | step-function dispatch — `evaluateStep` applies builtins to `currentCells` (`vql_engine.cpp:1466-1540`); the mutating `link(...)` is the precedent |
| `StoreGeneratorCall` | `anchor.generator` in `resolveAnchor` (`vql_engine.cpp:938-957`) — the open seam |
| `KwLens` | reserved token (`token.hpp:106`, `lexer.cpp:779-780`) — lexed but parser-unreachable (`parseAnchorNode` :523-528 has no `lens` keyword entry) |

VQL has **no cell-identity predicate** (`cellEq` — `=` on cells stringifies operands and compares
*rendered text*, `vql_engine.cpp:2145-2199` — a wrong-answer comparison, not a type error), **no
scalar read** (`cellScalar`), and **no session resolution** — the engine holds no `sessions_` list;
the REPL's `LensRepl` owns the one session `$lens` binds.

## 3. Design decisions

### D1 — `lens("uri")` is the sole-anchor generator; gestures are step functions

`lens("uri")` is a `StoreGeneratorCall` anchor — `resolveAnchor` calls `openLensUri` (all schemes;
P6's guard refuses `meta:`/unrecognized `scheme:`), projects into the engine's `core_`, yields the
session's projected roots, and binds `$lens` (the `setVariable` vector overload for `union:` forests).
`lens()` is itself session-mutating (it opens a session) but as an anchor it's the query's source —
`lens("u")/d.contains` is the natural open+traverse.

Every gesture on a *cell* is a **step function** — `path/lensmount`, `$cell/lensnext`, `path/lensedit("text")` — mapping over the relation: `$cells/lensmount` mounts under each cell (N mounts, per-cell yield; a per-cell refusal drops that cell, not the whole relation — `Err`-per-cell → empty for that cell) — dispatched in `evaluateStep`'s `FunctionInvocation` arm (`vql_engine.cpp:1466+`), which already receives `currentCells` and already mutates (`link(...)` at `:1466-1540`); cell-typed *args* resolve via `cellsOfValueExpr` (`:362-381`), not `evaluateValueExpr` (which renders cells to `CellValue`, destroying the identity `celleq` tests). The arg is the relation it filters — the "pipe" semantics without a pipe token (`|` is union-type-only, `parser.cpp:1603`). A step function needs no keyword — it's an `Identifier + '('` step (`parser.cpp:912-915`), so `lensmount`/`lensnext`/etc. stay *unreserved* names, not `Kw*` tokens.

### D2 — Sessions live on the coordinator, not the per-query engine

`vquery` builds a fresh `VQLEngine` per query (`main.cpp:522`) — an engine-owned `sessions_` dies
between `lens("…")` (query N) and `lensmount($cell)` (query N+1). The session registry belongs on
the **coordinator's core** — the same place `LensRepl`'s session lives (`main.cpp:424`), so
REPL-verb sessions and code-opened sessions coexist. The engine gets a `sessions()` accessor over
the coordinator's registry (or a `shared_ptr<vector<unique_ptr<ProjectionSession>>>` the frontend
owns and injects); `sessionFor(cell)` scans it calling `ownsCell` (the P3 pattern).

`$lens` auto-binds on `lens()` open (REPL parity — `main.cpp:525-526` already injects `lensRepl.root()` per query; the vector overload binds all roots).

### D3 — Step-function mutations are query-time side effects, sequenced by the relation

`$cell/lensmount` yields the delegate root cells AND mounts; `$cell/lenscommit` yields the applied
count as a scalar cell AND commits. The side effect runs when `evaluateStep` reaches that step —
a mutating step inside a `where`/join/predicate could evaluate multiple times or not at all. Rule:
**session-mutating step functions may appear only as a query's last step** (the statement's final
transform — `path/lensmount` or `path/lensmount/d.contains` is allowed; `where lensmount($c)` or a
`lensmount` mid-join is a `TypeContractError`). Enforcement is two-layered: **(i)** a statement-level static rule in the parser/type-contract
pass (a mutating step/generator legal only as a top-level statement path's last transform —
nested inside `for`/`let`/`def`/`watch`/predicates it's a `TypeContractError`); **(ii)** a runtime
backstop in `evaluateStep`/`resolveAnchor` — a mutating dispatch refuses when the evaluation is
nested (a `contextCells`-non-empty flag or a `mutating`-depth counter), covering the per-candidate
predicate/lambda/macro-dim multiplicities (`vql_engine.cpp:1373-1388`/`1917-1950`/`2213-2219`/
`2278-2365`). `lens()` itself mints a session — it's **session-mutating**, legal only as the anchor of a
top-level statement `PathExpression` that evaluates exactly once (not inside `return`, `for`,
`let`, `def`, `watch`, `until`, predicates, clone/shape/morphism/create/destructure targets —
`return lens("x")` over N rows would leak N sessions into append-only `sessions_`, and `lensclose`
is deferred U-V2). The mutating set is `{lens, lensmount, lensunmount, lensnext, lenscommit,
lensedit, lensrevert, lensbrowse, lensclose}`; the exempt const probes are `{lensstub, lensdirty}`.

`cellkey`/`celleq`/`cellscalar` are pure **value expressions** — `where cellkey($c) = "fs:52:794498"` filters; `celleq($a,$b)` identity-compares (the real motivation: `=` compares renders, not identity). Note `?cellkey($c)` (the `PredicateTest::FunctionInvocation` form, `ast.hpp:177`) silently evaluates false today (`vql_engine.cpp:2212-2230` never handles it) — the `?fn()` arm needs a dispatch too.

### D4 — Reads observe the arena at query time; projected cells are mutable

`$lens/d.contains` sees the arena as of query time — gestures `refresh()` into the same arena. VQL
has no thunk caching; reads are always current at evaluation. `cellScalar` returns `Int`/`Float`/
`Bool`/`NULL` — `Int64`/`Double`/`Bool`/`None+spans` are the lens-minted kinds (`mintNodeCell`,
`projection_session.cpp:95-116`); `Timestamp` and the handle kinds (`OpHandle`/`ExternRef`/
`ForeignOpHandle` — `valueBits` is a `CellRef`, `arena_manifold.cpp:1086-1094`) return `NULL` by
design (scope the function or document the disposition).

### D5 — The `session_ops` layer carries the gesture protocol

`sessionMountAt`/`sessionUnmountAt`/`sessionNextPage`/`sessionCommitAll`/`sessionEdit`/
`sessionRevert`/`sessionDirty` (P7) are the implementation — the step functions call them,
inheriting the dirty-gate (`lensEdit`/`lensCommit`/`lensRevert` exempt), the per-session
resolution, `canApply` inside `editContent`, and `commit()`→`expected<int>` (P5). `lensbrowse`
calls `SqlLens::browse(tableName)` (`sql_lens.cpp:442`) through a new
`session_ops::sessionBrowseAt` helper (the `mountedLens` → `dynamic_cast<SqlLens*>` → `browse` →
`refresh()` sequence — vlace's `runIO` inlines it today, `compiler.cpp:1633-1650`; moving it into
`session_ops` shares the resolver with `lensstub`);
`lensdirty` calls `session.dirtyCount()` directly (the `sessionDirty` probe is a gate, not a
counter).

`lensnext`/`lensstub` scope: cell → `keyFor` → `mountBoundary` → mount key → `windowedFor`/
`nextStubAt` (mount-scoped); `mountBoundary` → `nullopt` (a RouterLens cell outside all mounts,
or a plain `WindowedLens` session — `sql:`/`log:`/`journal:`/`git:` opened directly) → the
session-level `WindowedLens::nextStub`/`materializePage`; exhaustion → empty relation.

### D6 — `meta:` refused; `union:` and `sql:` boundaries documented

`openLensUri` (P6) refuses `meta:` — the engine *could* build the `MetaSource` fold
(`coordinator_.stores()`/`primaryStore`/`rebuildManifold`, `main.cpp:408-420`), but a borrowed
fold must outlive the session and the engine's `core_` has no base manifold; `meta:` is refused
by design, not by capability gap — noted, not a permanent exclusion. A direct `sql:` open is schema-only
until `lensbrowse`; a `RouterLens` inside a `union:` child is unreachable for `lensmount`.
`--engine vortex` maps `StoreGenerator` anchors to `home()` (`compiler.cpp:687-691`) — a `lens()`
query under the compiler silently yields home; either scope to the direct engine or add a compiler
row (Q5).

### D7 — VQL's own arena writes refuse session-owned cells too

vlace's D6 guards `setLink`; VQL's write surface is *larger* and equally unguarded: `performCreates`
(`/ +d.contains("x")` splices an authored cell into a managed rank — `vql_engine.cpp:1250-1261`),
`PathMorphism` `->`/`=>` (`link(c,dim,dir,sprout)` + reciprocal — `:1883-1897` — a write on a
projected cell overwrites its posward link *and* the displaced child's negward edge), `cloneTail`
and perspective clones (`d.clone` rank — **not lens-managed**, survives every refresh → a permanent
untracked edge, `:1024-1038`,`:1873-1880`), `shapeProjection` (`:1053-1084`), and `executeWatch`
(`dDelta` edges on projected cells, `:719-725`). Rule — same as vlace D6: **every VQL link-writing
site checks `sessionFor(cell)` on the cells it links and refuses when a write would land on a
session-owned cell**; the correct write is `lensedit` (content) — projected structure is the
session's to maintain (Q8).

## 4. Proposed surface

### Anchor (sole position — produces cells from nothing)

| generator | args | yields | notes |
|---|---|---|---|
| `lens(uri)` | `String` | session's root cells | `openLensUri`; binds `$lens`; refusal → empty relation |

### Step functions (`path/fn(...)`, mutating = last-step only)

| step | args | yields | notes |
|---|---|---|---|
| `lensmount` | — | delegate root cells | `sessionMountAt`; `Err`→empty |
| `lensunmount` | — | the mount-point cells | `mountBoundary`→`cellFor` resolved *before* `sessionUnmountAt` (which returns `expected<void>`); `Err`→empty |
| `lensnext` | — | materialized stub cells | `sessionNextPage` — mount-scoped via `windowedFor` on a Router session, session-level on a plain `WindowedLens` |
| `lensstub` | — | pending stub cells | `nextStubAt` (const — not dirty-gated) |
| `lenscommit` | — | applied count (scalar cell) | `sessionCommitAll` → `commit()` applied count |
| `lensedit(text)` | `String` | the cell | `sessionEdit` — `Err` for ref-bound/unkeyed/non-writable |
| `lensrevert` | — | the cell | `sessionRevert` — `Err` on source-vanished |
| `lensbrowse(table)` | `String` | the query page root | `SqlLens::browse(tableName)` — sql-lens-only |
| `lensdirty` | — | `dirtyCount` (scalar cell) | `session.dirtyCount()` |
| `lensclose` | — | empty | releases the session — deferred (`U-V2`) |

### Value expressions (`fn(args)` inside a where/predicate/return)

| fn | args | returns | notes |
|---|---|---|---|
| `cellkey(cell)` | `Cell` | `String`/empty | `ExternalKey` if a projected node cell; empty yield for attr/authored (VQL has no `NULL` — `CellValue` is `{string,double,int64_t,bool}`) |
| `celleq(a, b)` | `Cell, Cell` | `Bool` | identity on `CellRef` — `=` compares rendered text |
| `cellscalar(cell)` | `Cell` | `Int`/`Float`/`Bool`/empty | scalar `valueBits`; empty yield on span-cells and non-lens kinds (`Timestamp`/handle kinds — scope: lens-minted kinds only) |

## 5. Worked example

```vql
// open a dir lens; $lens binds the root
lens("/tmp/lensj")

// walk the listing
$lens/d.contains/d.seq

// mount git under the repo cell (find it first)
$lens/d.contains[cellkey(.) = "fs:52:794498"]/lensmount

// query commits under the delegate root
$lens/d.contains[cellkey(.) = "fs:52:794498"]/d.contains/d.contains

// back out: unmount under the repo cell
$lens/d.contains[cellkey(.) = "fs:52:794498"]/lensunmount

// a sql: lens: schema-only until browsed
lens("sql:/tmp/lensj/data.db")
$lens/d.contains/lensbrowse("t")
$lens/d.contains/d.contains/d.contains

// edit + commit a cell
$lens/d.contains/d.contains/d.contains[cellscalar(.) = 1]/lensedit("99")
$lens/d.contains/lenscommit
```

A step function takes no arg when the relation *is* the arg — `path/lensmount` mounts every cell in
the relation. `lensedit("99")` takes the new text. `lensbrowse("t")` takes the table name.

## 6. Open questions

1. **`lenserror`** — refusal = empty relation; a generator yielding the last error's reason as a
   cell is the honest read, but its state must outlive the per-query engine (coordinator-owned;
   `U-V1`). Alternatively throw — `vquery` reports `Error:` (`main.cpp:528-533`).
2. **`lensclose`** — releases a session (`U-V2`).
3. **`meta:`** — the engine has no `MetaSource` fold; refused (P6).
4. **`?cellkey($c)`** — the `PredicateTest::FunctionInvocation` arm (`ast.hpp:177`) silently
   evaluates false; needs a dispatch (Q2).
5. **`mount` vs `lensmount`** — VQL's `mount … as +d.dim` statement (`parser.cpp:300-323`) is a
   different thing; the step-function names stay unreserved (`Identifier`-steps, not keywords).
6. **`executeScriptBody`** runs all statements in one engine, returning only the last non-empty
   result (`vql_engine.cpp:164-175`) — a `lens()` + `lensmount` script sees both; separate REPL
   lines need D2's coordinator-side sessions.

## 7. Proposal Register

| Q | files | change | why not caller/vql |
|---|---|---|---|
| Q1 | `vql_engine.{hpp,cpp}` | `StoreGenerator` `lens` dispatch → `openLensUri` + `List` roots + `$lens` auto-bind; step-function dispatch for `lensmount`/`lensunmount`/`lensnext`/`lensstub`/`lenscommit`/`lensedit`/`lensrevert`/`lensbrowse`/`lensdirty`/`lensclose` in `evaluateStep`'s `FunctionInvocation` arm; mutating-step last-position check (parser/type-contract) | The generator + step seams live only here. |
| Q2 | `vql_engine.{hpp,cpp}` | `cellkey`/`celleq`/`cellscalar` as `FunctionInvocation` value-expr builtins + `PredicateTest::FunctionInvocation` dispatch for `?fn()` | Same engine, different surface. |
| Q3 | `vql/{lexer,parser,token}.{hpp,cpp}` | `lens` joins `parseAnchorNode`'s generator keywords (`:523-528`); the step names stay unreserved `Identifier`-steps | `KwLens` is lexed but unreachable today. |
| Q4 | `vql_engine.{hpp,cpp}` + coordinator | `sessions_`/`sessionFor` on the coordinator's session registry (per-query engine sees it); `$lens` vector-bind; `LensRepl` exposes `session()` so the REPL's session joins the registry | The session-outlives-engine requirement + shared `:lens`/`lens()` recency. |
| Q5 | `vql/compiler.cpp` OR doc-scope | `StoreGenerator` `lens` under `--engine vortex` maps to `home()` — either compile it or scope the design to the direct engine | The compiler path is a separate surface. |
| Q6 | `session_ops.hpp` | `sessionBrowseAt(session, table)` — the `mountedLens`/`SqlLens` resolver + `browse` + `refresh()`; `sessionUnmountAt` pre-resolves `mountBoundary`→`cellFor` to yield the mount-point cell | The browse resolver shared between vlace and VQL. |
| Q7 | `tests/` (new) | VQL lens journey — `lens()` → traverse → `lensmount` → back → `lenscommit`/`lensrevert`/`lensnext`/`lensstub`/`lensbrowse`; sole-anchor refusal (mutating step inside a `where`) | A test sits with its code — the acceptance artifact. |

The session machinery (`session_ops`, `ownsCell`, `canApply`, `revertKey`, `commit()`→`expected<int>`,
`windowedFor`/`nextStubAt`, `openLensUri` guard) is the P5–P8 layer — VQL's steps call it.

## 8. What this is not

- Not the `:lens` REPL verbs — those stay.
- Not a pipe operator (`|` is union-type-only) — the step-function surface is the existing
  "relation as arg" model.
- Not a session stack — "back" is a bound variable.
- Not yet xuzz's picker surface — different frontend's doc.
