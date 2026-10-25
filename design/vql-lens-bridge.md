# VQL Lens Bridge: Lenses as Step Functions in a Declarative Query Language (v2.9)

A design specification for how a **VQL program** — not the `vquery` REPL — opens, traverses, mounts,
writes back, and releases projection lenses. Design deliverable only: the signatures are proposals;
the engine's generator-anchor (`lens` open) and step-function (`path/fn(...)` mutate/read) machinery
is the seam, not a new layer. Rounds 1–2 critique folded in.

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
| `StoreGeneratorCall` | `anchor.generator` in `resolveAnchor` (`vql_engine.cpp:938-957`, inside `resolveAnchor` at `:846`) — the open seam |
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

Every gesture on a *cell* is a **step function** — `path/lensmount()`, `$cell/lensnext()`, `path/lensedit("text")` — mapping over the relation: `$cells/lensmount()` mounts under each cell (N mounts, per-cell yield; a per-cell refusal drops that cell, not the whole relation — `Err`-per-cell → empty for that cell). **Session-scoped ops dedupe**: `lenscommit`/`lensbrowse`/`lensunmount` resolve each input cell to its (session[, mount]) and run once per distinct target — arg-taking variants (`$cells/lensbrowse($c)`) take the arg from the first cell of each distinct target group (per-cell arg variance is silently first-wins — noted) — `$cells/lenscommit()` over N cells of one session commits once, not N times (yielding `[n]` not `[n,0,0…]`) — dispatched in `evaluateStep`'s `FunctionInvocation` arm (`vql_engine.cpp:1390+`), which already receives `currentCells` and already mutates (`link(...)` at `:1466-1540`); cell-typed *args* resolve via `cellsOfValueExpr` (`:362-381`), not `evaluateValueExpr` (which renders cells to `CellValue`, destroying the identity `celleq` tests). The arg is the relation it filters — the "pipe" semantics without a pipe token (`|` is union-type-only, `parser.cpp:1603`). A step function needs no keyword — it's an `Identifier + '('` step (`parser.cpp:912-915`), so `lensmount`/`lensnext`/etc. stay *unreserved* names, not `Kw*` tokens.

### D2 — Sessions live on the coordinator, not the per-query engine

`vquery` builds a fresh `VQLEngine` per query (`main.cpp:522`) — an engine-owned `sessions_` dies
between `lens("…")` (query N) and `lensmount($cell)` (query N+1). The session registry belongs on
the **coordinator's core** — the same place `LensRepl`'s session lives (`main.cpp:424`), so
REPL-verb sessions and code-opened sessions coexist. The registry is `shared_ptr`-backed —
`unique_ptr` can't hold the non-owning `LensRepl` session Q4 injects (vlace uses `shared_ptr`,
`compiler.hpp:225`). The engine gets a `sessions()` accessor over
the coordinator's registry (or a `vector<shared_ptr<ProjectionSession>>` the frontend
owns and injects (the `LensRepl` session joins as a `shared_ptr` — `unique_ptr` anywhere in the
type loses the non-owning join)); `sessionFor(cell)` scans it calling `ownsCell` (the P3 pattern).

`$lens` auto-binds on `lens()` open — and re-injects per query from the **coordinator registry's
most recent open** (open order — a `:lens` REPL open and a `lens()` code open share recency,
`main.cpp:525-526` currently injects only `lensRepl.root()`); the vector `setVariable` overload
(`vql_engine.hpp:61`) binds all roots. A bare integer anchor (`AnchorKind::LiteralCellId`, `parser.cpp:641-658`) does **not** reach
projected cells — arena cells carry `ephemeralBit` (`arena_manifold.hpp:24-29`) and `denseOf`
rejects masked/dense ids (`arena_manifold.cpp:257-270`, `resolveAnchor` `:905-910`); dense ids are
mint-order-dependent anyway. The stable cross-line handle is the **ExternalKey** `cellkey`
exposes (`fs:52:794498`); a `cellfor(key)`-style resolution is deferred (U-V3).

### D3 — Step-function mutations are query-time side effects, sequenced by the relation

`$cell/lensmount` yields the delegate root cells AND mounts; `$cell/lenscommit` yields the applied
count as a scalar cell AND commits. The side effect runs when `evaluateStep` reaches that step —
a mutating step inside a `where`/join/predicate could evaluate multiple times or not at all. Rule:
**at most one session-mutating step per top-level statement `PathExpression`; read steps may
follow it** (`path/lensmount()/d.contains` is allowed; `path/lensmount()/d.contains/lenscommit()`
and `…/lensedit("99")/lenscommit()` are refused — commit is a separate statement; `where
lensmount($c)`, mid-join, or any nested evaluation is a `TypeContractError`). The single rule: **a mutating construct may appear only where it evaluates exactly once at
statement level** — the statement's sole top-level path anchor (`lens("u")`), a `let`/flat
`for`-`in` **target path that binds cells at bind time** (`let $x := lens("u")` — `:=` — allowed;
`let $f :=` binds a *functor* evaluated per application via `functorOf`/`functors_`, `:495-505` —
banned), or **one step in a top-level path containing at most one mutating step**
(`path/lensmount()/d.contains` allowed; `…/lenscommit()` after any earlier mutation refused).
`lens("u")/lensmount()` = mutating anchor + mutating step = two mutations → refused. Nested `for`
in-paths, `return`, `def`, `watch`, `until`, predicates, clone/shape/morphism/create/destructure
targets — all refused (the depth flag catches them as nested). The **indirection channel** is
covered too: the static check scans `def` bodies and functor-bound `let` targets for mutating
constructs (a `def` re-enters `executeScriptBody`/`evaluatePath`, `:617-642`; `applyFunctor` calls
`evaluateStep` directly, `:435-441`; an untyped param accepts a functor arg — `f(/lensmount())`
smuggles a step), and the runtime flag is a *depth counter* incremented across `callUserDef`/
`applyFunctor`/`executeScriptBody` re-entry, never reset per statement — a mutating step reached
through a def or functor is refused. `watch lens("u")` is banned too (`targetPath` evaluates once,
`:685`, but the body re-fires per target cell — and `executeWatch`'s `dDelta` write now refuses
under D7 anyway). Enforcement is two-layered: **(i)** a statement-level static rule in the parser/type-contract
pass (a mutating step/generator legal only as a top-level statement path's last transform —
nested inside `for`/`let`/`def`/`watch`/predicates it's a `TypeContractError`); **(ii)** a runtime
backstop in `evaluateStep`/`resolveAnchor` — a mutating dispatch refuses when the evaluation is
nested, via an evaluation-depth/`topLevelStatement` flag threaded through `evaluatePath`/
`evaluateStep` (`contextCells`-non-empty does **not** work: the nested evals to refuse — `for`/`return`/
effect/`let`/`link`/`count` — call `evaluatePath` with `{}` context, `:2277`/`:2310`/`:2346`/`:2357`/
`:465`/`:1509`, indistinguishable from a top-level path). Covers predicate/lambda/
macro-dim/`PerspectiveStep`/`PerspectiveBlock`/`PathLambda` multiplicities (`:1373-1388`/
`:1756-1761`/`:1917-1950`/`:2213-2219`/`:2278-2365`). `lens()` itself mints a session — it's **session-mutating**, legal only as the anchor of a
top-level statement `PathExpression` that evaluates exactly once (not inside `return`, `for`,
`let`, `def`, `watch`, `until`, predicates, clone/shape/morphism/create/destructure targets —
`return lens("x")` over N rows would leak N sessions into append-only `sessions_`, and `lensclose`
is deferred U-V2). The mutating set is `{lens, lensmount, lensunmount, lensnext, lenscommit,
lensedit, lensrevert, lensbrowse, lensclose}`; the exempt const probes are `{lensstub, lensdirty}`.

`cellkey`/`celleq`/`cellscalar` are pure **value expressions** — `where cellkey($c) = "fs:52:794498"` filters; `celleq($a,$b)` identity-compares (the real motivation: `=` compares renders, not identity). Note `?fn(...)` produces `PredicateTest::ValueExpr` (`parser.cpp:1404-1407`) and routes into
`evaluateValueExpr`'s function table (`:2057-2116`) — an unknown fn returns `""` (falsy, silent
filter, `:2142`). The pure probes register in **both** dispatch surfaces — `evaluateStep`'s `FunctionInvocation`
arm (`:1390+`) for `cellsOfValueExpr`-routed args (comparison operands in a predicate go through
`cellsOfValueExpr` → `evaluateStep`, `:397-417`, *not* `evaluateValueExpr`) **and**
`evaluateValueExpr`'s fn table (`:2059+`) for `?expr`/value positions (precedent: `contains`/`count`/
`delta` are registered in both, `:1437-1465`/`:2059-2078`) — **and the mutating names refuse in
both**
(`?lensedit("x")` is a `TypeContractError`, not a silent filter — the last-step rule has no
predicate exception).

### D4 — Reads observe the arena at query time; projected cells are mutable

`$lens/d.contains` sees the arena as of query time — gestures `refresh()` into the same arena. VQL
has no thunk caching; reads are always current at evaluation. `cellScalar` returns `Int`/`Float`/
`Bool`/`NULL` — `Int64`/`Double`/`Bool`/`None+spans` are the lens-minted kinds (`mintNodeCell`,
`projection_session.cpp:95-116`); `Timestamp` and the handle kinds (`OpHandle`/`ExternRef`/
`ForeignOpHandle` — `valueBits` is a `CellRef`, `arena_manifold.cpp:1086-1094`) yield empty by
design (consistent with the §4 model — `cellScalar` sees only the lens-minted kinds).

### D5 — The `session_ops` layer carries the gesture protocol

`sessionMountAt`/`sessionUnmountAt`/`sessionNextPage`/`sessionCommitAll`/`sessionEdit`/
`sessionRevert`/`sessionDirty` (P7) are the implementation — the step functions call them,
inheriting the dirty-gate (`lensEdit`/`lensCommit`/`lensRevert` exempt), the per-session
resolution, `canApply` inside `editContent`, and `commit()`→`expected<int>` (P5). `lensbrowse`
calls `SqlLens::browse(tableName)` (`sql_lens.cpp:442`) through a new
`session_ops::sessionBrowseAt(session, key, table)` helper — the key does the mount-scoped routing
(`mountBoundary` → `windowedFor` → delegate, vlace's `mountedLens` resolver at `compiler.cpp:
1441-1470`, inlined today) → `dynamic_cast<SqlLens*>` → `isBrowsed` → `browse` → `refresh()`;
the yield needs the new query's root cell — `browse` returns `expected<void>` (`sql_lens.cpp:442`)
and `queries_` keys are private, so the helper needs a browse-returns-key or a `SqlLens` root-key
accessor (registered in Q6). `session_ops` gains a `.cpp` for this (a `SqlLens` complete type
would pull `sql_lens.hpp`/`sql_backend.hpp` into the generic header otherwise).
`lensdirty` calls `session.dirtyCount()` directly (the `sessionDirty` probe is a gate, not a
counter).

`lensnext`/`lensstub` scope: cell → `keyFor` → `mountBoundary` → mount key → `windowedFor`/
`nextStubAt` (mount-scoped). `mountBoundary` → `nullopt` on a RouterLens cell means *outside all
mounts* → **`Err`** (session-level `RouterLens::nextStub()` is first-wins across mounts — the
hazard `session_ops.hpp:100-104` documents; vlace refuses the same way); a plain `WindowedLens`
session (`sql:`/`log:`/`journal:`/`git:` opened directly) → the session-level
`nextStub`/`materializePage`; exhaustion → empty relation. `session_ops::sessionMountScopeAt
(session, key)` holds the cell→keyFor→router→mountBoundary preamble once (triplicated today
across vlace's `IO.LensNext`/`mountedLens` and VQL's next/stub/browse/unmount).

### D6 — `meta:` refused; `union:` and `sql:` boundaries documented

`openLensUri` (P6) refuses `meta:` — the engine *could* build the `MetaSource` fold
(`coordinator_.stores()`/`primaryStore`/`rebuildManifold`, `main.cpp:408-420`), but a borrowed
fold must outlive the session and the engine's `core_` has no base manifold; `meta:` is refused
by design, not by capability gap — noted, not a permanent exclusion. A direct `sql:` open is schema-only
until `lensbrowse`; a `RouterLens` inside a `union:` child is unreachable for `lensmount`.
`--engine vortex` maps `StoreGenerator` anchors to `home()` (`vql/compiler.cpp:687-691`) — a `lens()`
query under the compiler silently yields home; either scope to the direct engine or add a compiler
row (Q5).

### D7 — VQL's own arena writes refuse session-owned cells too

vlace's D6 guards `setLink`; VQL's write surface is *larger* and equally unguarded: `performCreates`
(`/ +d.contains("x")` splices an authored cell into a managed rank — `vql_engine.cpp:1250-1261`),
`PathMorphism` `->`/`=>` (`link(c,dim,dir,sprout)` + reciprocal — `:1883-1897` — a write on a
projected cell overwrites its posward link *and* the displaced child's negward edge), `cloneTail`
and perspective clones (`d.clone` rank — **not lens-managed**, survives every refresh → a permanent
untracked edge, `:1024-1038`,`:1873-1880`), `shapeProjection` (`:1046-1089`), and `executeWatch`
(`dDelta` edges on projected cells, `:719-725`) — **including the `link(...)` step builtin itself**
(`core_->link(c, dim, dir, cloneCell)` over `currentCells`, `:1519-1527` — the flagship mutating
step; its `cloneGenerator(target)` also appends `d.clone` on a session-owned *target*,
`vortex_core.cpp:317-325`) and `performCreates`' `attachPoint`/`rankTail` end (`:1233-1235`, not
just `in`). Rule — same as vlace D6: **every VQL link-writing
site checks `sessionFor(cell)` on the cells it links and refuses when a write would land on a
session-owned cell**; the correct write is `lensedit` (content) — projected structure is the
session's to maintain (Q7B).

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
| `lensbrowse(table)` | `String` | the new query's root cell | `sessionBrowseAt` — sql-lens-only; `browse` returns `expected<void>` so the root comes from a `SqlLens` accessor or a browse-returns-key change (Q6) |
| `lensdirty` | — | `dirtyCount` (scalar cell) | `session.dirtyCount()` |
| `lensclose` | — | empty | releases the session — deferred (`U-V2`) |

### Value expressions (`fn(args)` inside a where/predicate/return)

| fn | args | returns | notes |
|---|---|---|---|
| `cellkey(cell)` | `Cell` | `String`/empty | `ExternalKey` if a projected node cell; empty yield for attr/authored (VQL has no `NULL` — `CellValue` is `{string,double,int64_t,bool}`) |
| `celleq(a, b)` | `Cell, Cell` | `Bool` | identity on `CellRef` — `=` compares rendered text; each arg must resolve to exactly one cell (`cellsOfValueExpr` N≠1 → `TypeContractError`) |
| `cellscalar(cell)` | `Cell` | `Int`/`Float`/`Bool`/empty | scalar `valueBits`; empty yield on span-cells and non-lens kinds (`Timestamp`/handle kinds — scope: lens-minted kinds only) |

## 5. Worked example

```vql
// open a dir lens; $lens binds the root
lens("/tmp/lensj")

// walk the listing -- /d.seq::rank re-seeks the head, reaching ALL
// children (d.contains alone yields only the firstborn link's target)
$lens/d.contains/d.seq::rank

// mount git under the repo cell (find it first)
$lens/d.contains/d.seq::rank[cellkey(.) = "fs:52:794498"]/lensmount()

// query commits under the delegate root
$lens/d.contains/d.seq::rank[cellkey(.) = "fs:52:794498"]/d.contains/d.seq::rank

// back out: unmount under the repo cell
$lens/d.contains/d.seq::rank[cellkey(.) = "fs:52:794498"]/lensunmount()

// a sql: lens: schema-only until browsed
lens("sql:/tmp/lensj/data.db")
$lens/d.contains/lensbrowse("t")
$lens/d.contains/d.seq::rank/d.contains/d.seq::rank

// edit + commit a cell
$lens/d.contains/d.seq::rank/d.contains/d.seq::rank[cellscalar(.) = 1]/lensedit("99")
$lens/d.contains/lenscommit()
```

**The `d.contains`/`d.seq` asymmetry**: `d.contains` is the single firstborn link; siblings chain
on `d.seq`, and `traverseDimension` starts *after* the start cell — so `/d.contains/d.seq` yields
children 2..N only. `/d.contains/d.seq::rank` (a `Placement::Rank` suffix, `parser.cpp:963`,
`traverseDimension` re-seeks the head, `vql_engine.cpp:1142-1153`) yields the whole run. Every
"all children" example needs the `::rank` form.

A step function takes no arg when the relation *is* the arg — `path/lensmount()` mounts every cell in
the relation. **The parens are mandatory**: a bare `path/lensmount` parses as a `SignedDimensionStep`
(`parser.cpp:928-935`) — a traversal on a minted `lensmount` dim yielding empty, silently; only
`Identifier + ('` reaches the `FunctionInvocation` dispatch (`:912-915`). `lensedit("99")` takes the
new text; `lensbrowse("t")` the table name. An empty relation maps to a no-op — `[]/lenscommit()`
yields empty without running the gesture (`:1294-1298` guards only `value`/`link`; the lens dispatch
must check `currentCells.empty()` first).

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
| Q1 | `vql_engine.{hpp,cpp}` | `StoreGenerator` `lens` dispatch → `openLensUri` (intercepted **before** the silent named-store→home fallback at `:944-954`) + `List` roots + `$lens` auto-bind; step-function dispatch for `lensmount`/`lensunmount`/`lensnext`/`lensstub`/`lenscommit`/`lensedit`/`lensrevert`/`lensbrowse`/`lensdirty`/`lensclose` in `evaluateStep`'s `FunctionInvocation` arm; mutating-step last-position check (parser/type-contract) | The generator + step seams live only here. |
| Q2 | `vql_engine.{hpp,cpp}` | `cellkey`/`celleq`/`cellscalar` as `FunctionInvocation` value-expr builtins + `?fn()`/bare-predicate function resolution through `evaluateValueExpr`'s table for all names | Same engine, different surface. |
| Q3 | `vql/{lexer,parser,token}.{hpp,cpp}` | `lens` joins `parseAnchorNode`'s generator keywords (`:523-528`); the step names stay unreserved `Identifier`-steps | `KwLens` is lexed but unreachable today. |
| Q4 | `vql_engine.{hpp,cpp}` + coordinator | `sessions_`/`sessionFor` on the coordinator's session registry (per-query engine sees it); `$lens` vector-bind; `LensRepl` exposes `session()` so the REPL's session joins the registry | The session-outlives-engine requirement + shared `:lens`/`lens()` recency. |
| Q5 | `vql/compiler.cpp` OR doc-scope | `StoreGenerator` `lens` under `--engine vortex` maps to `home()` — either compile it or scope the design to the direct engine | The compiler path is a separate surface. |
| Q6 | `session_ops.{hpp,cpp}` + `sql_lens.{hpp,cpp}` | `sessionBrowseAt(session, key, table)` — mount-scoped resolver + `browse` + `refresh()`; a `SqlLens` accessor (or browse-returns-key) to yield the new query's root; `sessionUnmountAt` pre-resolves `mountBoundary`→`cellFor`; new `session_ops.cpp` keeps `SqlLens` out of the header | The browse resolver shared between vlace and VQL. |
| Q7B | `vql_engine.cpp` + `vquery/main.cpp` | `sessionFor` guard at every link-writing site: `link(...)` step + `cloneGenerator`, `performCreates` (both `in` and `attachPoint`), `PathMorphism`, `cloneTail`/perspective clones, `shapeProjection`, `executeWatch`; **and** on the `promote`/`--in-place`/`writeResultSlice`/`:save` result-persistence path | D7 — the provenance guard no other row owns. |
| Q7 | `tests/` (new) | VQL lens journey — `lens()` → traverse → `lensmount` → back → `lenscommit`/`lensrevert`/`lensnext`/`lensstub`/`lensbrowse`; sole-anchor refusal (mutating step inside a `where`) | A test sits with its code — the acceptance artifact. |

The session machinery (`session_ops`, `ownsCell`, `canApply`, `revertKey`, `commit()`→`expected<int>`,
`windowedFor`/`nextStubAt`, `openLensUri` guard) is the P5–P8 layer — VQL's steps call it.

## 8. What this is not

- Not the `:lens` REPL verbs — those stay.
- Not a pipe operator (`|` is union-type-only) — the step-function surface is the existing
  "relation as arg" model.
- Not a session stack — "back" is a bound variable.
- Not yet xuzz's picker surface — different frontend's doc.
