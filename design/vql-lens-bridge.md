# VQL Lens Bridge: Lenses as Step Functions in a Declarative Query Language (v4.2)

A design specification for how a **VQL program** — not the `vquery` REPL — opens, traverses, mounts,
writes back, and releases projection lenses. Design deliverable only: the signatures are proposals;
the engine's generator-anchor (`lens` open) and step-function (`path/fn(...)` mutate/read) machinery
is the seam, not a new layer. Rounds 1–2 critique folded in.

VQL is **declarative**: a query produces a relation (a set of cells). Lens ops split two ways: the
**open** is a generator anchor (produces cells from nothing); every **gesture on a cell** is a
**step function** (`path/lensMount`, `$cell/lensNext`) — the existing `FunctionInvocation` step
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

### D1 — a `*Lens` generator is the sole-anchor generator; gestures are step functions

a `*Lens` generator is a `StoreGeneratorCall` anchor — `resolveAnchor` calls `openLensUri` (all schemes;
P6's guard refuses `meta:`/unrecognized `scheme:`), projects into the engine's `core_`, yields the
session's projected roots, and binds `$lens` (the `setVariable` vector overload for `union:` forests).
a `*Lens` generator is itself session-mutating (it opens a session) but as an anchor it's the query's source —
`fsLens("/x")/d.contains` is the natural open+traverse.

Every gesture on a *cell* is a **step function** — `path/lensMount()`, `$cell/lensNext()`, `path/lensEdit("text")` — mapping over the relation: `$cells/lensMount()` mounts under each cell (N mounts, per-cell yield; a per-cell refusal drops that cell, not the whole relation — `Err`-per-cell → empty for that cell). **Session-scoped ops dedupe**: `lensCommit`/`lensBrowse`/`lensUnmount` resolve each input cell to its (session[, mount]) and run once per distinct target (`lensNext`/`lensStub` are deliberately **not** deduped — N cells in one mount materialize N pages (`lensStub` is const — N probes of the same stub, dedupe at the relation level)) — arg-taking variants (`$cells/lensBrowse($c)`) take the arg from the first cell of each distinct target group (per-cell arg variance is silently first-wins — noted) — `$cells/lensCommit()` over N cells of one session commits once, not N times (yielding `[n]` not `[n,0,0…]`) — dispatched in `evaluateStep`'s `FunctionInvocation` arm (`vql_engine.cpp:1390+`), which already receives `currentCells` and already mutates (`link(...)` at `:1466-1540`); cell-typed *args* resolve via `cellsOfValueExpr` (`:362-381`), not `evaluateValueExpr` (which renders cells to `CellValue`, destroying the identity `cellEq` tests). The arg is the relation it filters — the "pipe" semantics without a pipe token (`|` is union-type-only, `parser.cpp:1603`). A step function needs no keyword — it's an `Identifier + '('` step (`parser.cpp:912-915`), so `lensMount`/`lensNext`/etc. stay *unreserved* names, not `Kw*` tokens.

### D2 — Sessions live on the coordinator, not the per-query engine

`vquery` builds a fresh `VQLEngine` per query (`main.cpp:522`) — an engine-owned `sessions_` dies
between `fsLens("…")` (query N) and `lensMount($cell)` (query N+1). The session registry belongs on
the **coordinator's core** — the same place `LensRepl`'s session lives (`main.cpp:424`), so
REPL-verb sessions and code-opened sessions coexist. The registry is `shared_ptr`-backed —
`unique_ptr` can't hold the non-owning `LensRepl` session Q4 injects (vlace uses `shared_ptr`,
`compiler.hpp:225`). The engine gets a `sessions()` accessor over
the coordinator's registry (or a `vector<shared_ptr<ProjectionSession>>` the frontend
owns and injects (the `LensRepl` session joins as a `shared_ptr` — `unique_ptr` anywhere in the
type loses the non-owning join)); `sessionFor(cell)` scans it calling `ownsCell` (the P3 pattern).

`$lens` auto-binds on a `*Lens` open — and re-injects per query from the **coordinator registry's
most recent *live* open** (expired `weak_ptr` entries are skipped — a `:lens close` leaves no stale
`$lens`; today `lensRepl.root()` → `nullopt` after close unbinds correctly, `main.cpp:525-526`); the
vector `setVariable` overload (`vql_engine.hpp:61`) binds all roots. A bare integer anchor (`AnchorKind::LiteralCellId`, `parser.cpp:641-658`) reaches a projected cell only
by typing the full ephemeral value (`denseOf` *accepts* ephemeral refs — `arena_manifold.cpp:
267-269` unmasks the bit — `--format cells` prints them, `main.cpp:558`); a bare dense number
resolves in the base/overlay namespace, and an ephemeral id is meaningless and unstable across
runs. Write-guarded by D7 regardless. The stable cross-line handle is the **ExternalKey** `cellKey`
exposes (`fs:52:794498`); a `cellFor(key)`-style resolution is deferred (U-V3).

### D3 — Step-function mutations are query-time side effects, sequenced by the relation

`$cell/lensMount` yields the delegate root cells AND mounts; `$cell/lensCommit` yields the applied
count as a scalar cell AND commits. The side effect runs when `evaluateStep` reaches that step —
a mutating step inside a `where`/join/predicate could evaluate multiple times or not at all. Rule:
**at most one session-mutating step per top-level statement `PathExpression`; read steps may
follow it** (`path/lensMount()/d.contains` is allowed; `path/lensMount()/d.contains/lensCommit()`
and `…/lensEdit("99")/lensCommit()` are refused — commit is a separate statement; `where
lensMount($c)`, mid-join, or any nested evaluation is a `TypeContractError`). The single rule: **a mutating construct may appear only where it evaluates exactly once at
statement level** — the statement's sole top-level path anchor (`fsLens("/x")`), a flat `for`-`in`
path (`inPath` evaluates once, `:2277`) or a `let` target **in the pre-`for` binding prefix**
(`let $x := fsLens("/x")` — `:=` — allowed; a `let` *inside* a `for` recursion re-evaluates per
binding, `evaluateBindings` `:2294` — same leak as `return fsLens("/x")`; and `let $f :=` binds a
*functor* evaluated per application via `functorOf`/`functors_`, `:392-432`/`:453-462` — both banned), or **one step in a top-level path containing at most one mutating step**
(`path/lensMount()/d.contains` allowed; `…/lensCommit()` after any earlier mutation refused).
`fsLens("/x")/lensMount()` = mutating anchor + mutating step = two mutations → refused. Nested `for`
in-paths, `return`, `def`, `watch`, `until`, predicates, clone/shape/morphism/create/destructure
targets — all refused (the depth flag catches them as nested). The **indirection channel** is
covered too: the static check scans `def` bodies and functor-bound `let` targets for mutating
constructs (a `def` re-enters `executeScriptBody`/`evaluatePath`, `:617-642`; `applyFunctor` calls
`evaluateStep` directly, `:435-441`; an untyped param accepts a functor arg — `f(/lensMount())`
smuggles a step), and the runtime flag is a *depth counter* incremented across `callUserDef`/
`applyFunctor`/`executeScriptBody` re-entry, never reset per statement — a mutating step reached
through a def or functor is refused. `watch fsLens("/x")` is banned too (`targetPath` evaluates once,
`:685`, but the body re-fires per target cell — and `executeWatch`'s `dDelta` write now refuses
under D7 anyway). Enforcement is two-layered: **(i)** a statement-level static rule in the parser/type-contract
pass (a mutating construct legal only where it evaluates exactly once — top-level path, for-in, outermost-let, sole anchor —
nested inside `for`/`let`/`def`/`watch`/predicates it's a `TypeContractError`); **(ii)** a runtime
backstop in `evaluateStep`/`resolveAnchor` — a mutating dispatch refuses when the evaluation is
nested, via an evaluation-depth/`topLevelStatement` flag threaded through `evaluatePath`/
`evaluateStep` (`contextCells`-non-empty does **not** work: the nested evals to refuse — `for`/`return`/
effect/`let`/`link`/`count` — call `evaluatePath` with `{}` context, `:2277`/`:2310`/`:2346`/`:2357`/
`:465`/`:1509`, indistinguishable from a top-level path). Covers predicate/lambda/
macro-dim/`PerspectiveStep`/`PerspectiveBlock`/`PathLambda` multiplicities (`:1373-1388`/
`:1756-1761`/`:1910-1921`/`:2213-2219`/`:2278-2365`). a `*Lens` generator itself mints a session — it's **session-mutating**, legal only as the anchor of a
top-level statement `PathExpression` that evaluates exactly once (not inside `return`, `for`,
`let`, `def`, `watch`, `until`, predicates, clone/shape/morphism/create/destructure targets —
`return fsLens("/x")` over N rows would leak N sessions into append-only `sessions_`, and `lensClose`
is deferred U-V2). The mutating set is `{fsLens, gitLens, sqlLens, logLens, journalLens, cfgLens, unionLens, lensMount,
lensUnmount, lensNext, lensCommit, lensEdit, lensRevert, lensBrowse, lensClose}`; the exempt const probes are `{lensStub, lensDirty}`.

`cellKey`/`cellEq`/`cellScalar` are pure **value expressions** — `where cellKey($c) = "fs:52:794498"` filters; `cellEq($a,$b)` identity-compares (the real motivation: `=` compares renders, not identity). Note `?fn(...)` produces `PredicateTest::ValueExpr` (`parser.cpp:1404-1407`) and routes into
`evaluateValueExpr`'s function table (`:2057-2116`) — an unknown fn returns `""` (falsy, silent
filter, `:2142`). The pure probes register in **both** dispatch surfaces — `evaluateStep`'s `FunctionInvocation`
arm (`:1390+`) for `cellsOfValueExpr`-routed args (comparison operands in a predicate go through
`cellsOfValueExpr` → `evaluateStep`, `:397-417`, *not* `evaluateValueExpr`) **and**
`evaluateValueExpr`'s fn table (`:2059+`) for `?expr`/value positions (precedent: `contains`/`count`/
`delta` are registered in both, `:1437-1552`/`:2059-2108`) — **and the mutating names refuse in
both**
(`?lensEdit("x")` is a `TypeContractError`, not a silent filter — the last-step rule has no
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
resolution, `canApply` inside `editContent`, and `commit()`→`expected<int>` (P5). `lensBrowse`
calls `SqlLens::browse(tableName)` (`sql_lens.cpp:442`) through a new
`session_ops::sessionBrowseAt(session, key, table)` helper — the key does the mount-scoped routing
(`mountBoundary` → `windowedFor` → delegate, vlace's `mountedLens` resolver at `compiler.cpp:
1441-1470`, inlined today) → `dynamic_cast<SqlLens*>` → `isBrowsed` → `browse` → `refresh()`;
`sessionBrowseAt` is dirty-gated like the other mutating gestures (vlace's `lensBrowse` has no
gate today — VQL's goes through `session_ops`, so it inherits one; consistent). The yield needs
the new query's root cell — `browse` returns `expected<void>` (`sql_lens.cpp:442`)
and `queries_` keys are private, so the helper needs a browse-returns-key or a `SqlLens` root-key
accessor (registered in Q6). `session_ops` gains a `.cpp` for this (a `SqlLens` complete type
would pull `sql_lens.hpp`/`sql_backend.hpp` into the generic header otherwise).
`lensDirty` calls `session.dirtyCount()` directly (the `sessionDirty` probe is a gate, not a
counter).

`lensNext`/`lensStub` scope: cell → `keyFor` → `mountBoundary` → mount key → `windowedFor`/
`nextStubAt` (mount-scoped). `mountBoundary` → `nullopt` on a RouterLens cell means *outside all
mounts* → **`Err`** (session-level `WindowedLens::nextStub()` (inherited by RouterLens) is first-wins across mounts — the
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
until `lensBrowse`; a `RouterLens` inside a `union:` child is unreachable for `lensMount`.
`--engine vortex` maps `StoreGenerator` anchors to `home()` (`vql/compiler.cpp:687-691`) — a `*Lens`
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
site checks `sessionFor(cell)` on the cells it links — including the *displaced* slots
(`link()` clears the reciprocal edge on `displacedUs`/`displacedIt`, `arena_manifold.cpp:1356-1374`;
`linkedMinting` can write through `proxyFor`, `:919-950`; `resolveDimension` through
`ensureDimension`, `:94-106`) — a checked-link funnel inspects `from`/`to`/`displacedUs`/
`displacedIt`/rank tails rather than each callsite open-coding the check — and refuses
(`TypeContractError` — not per-cell-empty, so a `for … link()` over a mixed relation aborts loudly
rather than silently writing a subset) when a write would land on a session-owned cell**; the correct write is `lensEdit` (content) — projected structure is the
session's to maintain (Q7B).

## 4. Proposed surface

### Anchor (sole position — produces cells from nothing)

| generator | args | yields | notes |
|---|---|---|---|
| `fsLens(path)` `gitLens(path)` `sqlLens(dsn)` `logLens(path)` `journalLens(path)` `cfgLens(path)` `unionLens(spec…)` | args | session's root cells | `openLensUri` underneath; binds `$lens`; refusal → empty relation (a session projecting zero roots still registers — `lensDirty`/`lensClose` can reach it; an empty source is not a refusal) |

### Step functions (`path/fn(...)`, mutating = last-step only)

| step | args | yields | notes |
|---|---|---|---|
| `lensMount` | — | delegate root cells | `sessionMountAt`; `Err`→empty |
| `lensUnmount` | — | the mount-point cells | `mountBoundary`→`cellFor` resolved *before* `sessionUnmountAt` (which returns `expected<void>`); `Err`→empty |
| `lensNext` | — | materialized stub cells | `sessionNextPage` — mount-scoped via `windowedFor` on a Router session, session-level on a plain `WindowedLens` |
| `lensStub` | — | pending stub cells (deduped like the other session-scoped ops — one probe per mount; N cells in one mount yield one stub) | `nextStubAt` (const — not dirty-gated) |
| `lensCommit` | — | applied count (scalar cell) | `sessionCommitAll` → `commit()` applied count |
| `lensEdit(text)` | `String` | the cell | `sessionEdit` — `Err` for ref-bound/unkeyed/non-writable |
| `lensRevert` | — | the cell | `sessionRevert` — `Err` on source-vanished |
| `lensBrowse(table)` | `String` | the new query's root cell | `sessionBrowseAt` — sql-lens-only; `browse` returns `expected<void>` so the root comes from a `SqlLens` accessor or a browse-returns-key change (Q6) |
| `lensDirty` | — | `dirtyCount` (scalar cell) | `session.dirtyCount()` |
| `lensClose` | — | empty | releases the session — deferred (`U-V2`) |

### Value expressions (`fn(args)` inside a where/predicate/return)

| fn | args | returns | notes |
|---|---|---|---|
| `cellKey(cell)` | `Cell` | `String`/empty | `ExternalKey` if a projected node cell; empty/`""` yield for attr/authored (VQL has no `NULL` — `CellValue` is `{string,double,int64_t,bool}`; note `cellKey($c) = ""` is *true* for attr cells — a residual wrong-answer comparison, the `cellKey`-vs-`""` collision) |
| `cellEq(a, b)` | `Cell, Cell` | `Bool` | identity on `CellRef` — `=` compares rendered text; each arg must resolve to exactly one cell (`cellsOfValueExpr` N≠1 → `TypeContractError`) |
| `cellScalar(cell)` | `Cell` | `Int`/`Float`/`Bool`/empty | scalar `valueBits`; empty yield on span-cells and non-lens kinds (`Timestamp`/handle kinds — scope: lens-minted kinds only) |

## 5. Worked example

```vql
// open a dir lens; $lens binds the root
fsLens("/tmp/lensj")

// walk the listing -- /d.seq::rank re-seeks the head, reaching ALL
// children (d.contains alone yields only the firstborn link's target)
$lens/d.contains/d.seq::rank

// mount git under the repo cell (find it first)
$lens/d.contains/d.seq::rank[cellKey(.) = "fs:52:794498"]/lensMount()

// query commits under the delegate root
$lens/d.contains/d.seq::rank[cellKey(.) = "fs:52:794498"]/d.contains/d.seq::rank

// back out: unmount under the repo cell
$lens/d.contains/d.seq::rank[cellKey(.) = "fs:52:794498"]/lensUnmount()

// a sql: lens: schema-only until browsed
sqlLens("/tmp/lensj/data.db")
$lens/d.contains/lensBrowse("t")
$lens/d.contains/d.seq::rank/d.contains/d.seq::rank

// edit + commit a cell
$lens/d.contains/d.seq::rank/d.contains/d.seq::rank[cellScalar(.) = 1]/lensEdit("99")
$lens/d.contains/lensCommit()
```

**The `d.contains` spine**: `d.contains` is the single firstborn link and `traverseDimension`'s
Default arm walks it *transitively* (`linkedMinting` iterated, `:1104-1112`) — `/d.contains` from a
dir root yields the whole left spine (firstborn, its firstborn, …), not just the firstborn, whenever
a firstborn has children. `/d.contains/d.seq::rank` (a `Placement::Rank` suffix, `parser.cpp:963`,
re-seeks the head then streams, `:1142-1153`) yields the sibling runs at *every* spine level —
root's children ∪ firstborn-dir's children ∪ … — a multi-level union when dirs nest (the common
case). The `[cellKey(.) = …]` filters in §5 are what scope each example back to one level.

A step function takes no arg when the relation *is* the arg — `path/lensMount()` mounts every cell in
the relation. **The parens are mandatory**: a bare `path/lensMount` parses as a `SignedDimensionStep`
(`parser.cpp:928-935`) — a traversal on a minted `lensMount` dim yielding empty, silently; only
`Identifier + ('` reaches the `FunctionInvocation` dispatch (`:912-915`). `lensEdit("99")` takes the
new text; `lensBrowse("t")` the table name. An empty relation maps to a no-op — `[]/lensCommit()`
yields empty without running the gesture (`:1294-1298` guards only `value`/`link`; the lens dispatch
must check `currentCells.empty()` first).

## 6. Open questions

1. **`lensError`** — refusal = empty relation; a generator yielding the last error's reason as a
   cell is the honest read, but its state must outlive the per-query engine (coordinator-owned;
   `U-V1`). Alternatively throw — `vquery` reports `Error:` (`main.cpp:528-533`).
2. **`lensClose`** — releases a session (`U-V2`).
3. **`meta:`** — the engine has no `MetaSource` fold; refused (P6).
4. **`?cellKey($c)`** — the `PredicateTest::FunctionInvocation` arm (`ast.hpp:176`) silently
   evaluates false; needs a dispatch (Q2).
5. **`mount` vs `lensMount`** — VQL's `mount … as +d.dim` statement (`parser.cpp:300-323`) is a
   different thing; the step-function names stay unreserved (`Identifier`-steps, not keywords).
6. **`executeScriptBody`** runs all statements in one engine, returning only the last non-empty
   result (`vql_engine.cpp:164-175`) — a `*Lens` + `lensMount` script sees both; separate REPL
   lines need D2's coordinator-side sessions.

## 7. Proposal Register

| Q | files | change | why not caller/vql |
|---|---|---|---|
| Q1 | `vql_engine.{hpp,cpp}` | the `*Lens` generator dispatch → `openLensUri` (intercepted **before** the silent named-store→home fallback at `:944-954`) + `List` roots + `$lens` auto-bind; step-function dispatch for `lensMount`/`lensUnmount`/`lensNext`/`lensStub`/`lensCommit`/`lensEdit`/`lensRevert`/`lensBrowse`/`lensDirty`/`lensClose` in `evaluateStep`'s `FunctionInvocation` arm; mutating-step single-eval check (parser/type-contract) | The generator + step seams live only here. |
| Q2 | `vql_engine.{hpp,cpp}` | `cellKey`/`cellEq`/`cellScalar` as `FunctionInvocation` value-expr builtins + `?fn()`/bare-predicate function resolution through `evaluateValueExpr`'s table for all names | Same engine, different surface. |
| Q3 | `vql/{lexer,parser,token}.{hpp,cpp}` | `lens` joins `parseAnchorNode`'s generator keywords (`:523-528`); the step names stay unreserved `Identifier`-steps | `KwLens` is lexed but unreachable today. |
| Q4 | `vql_engine.{hpp,cpp}` + coordinator | `sessions_`/`sessionFor` on the coordinator's session registry (per-query engine sees it); `$lens` vector-bind; `LensRepl` exposes `session()` so the REPL's session joins the registry | The session-outlives-engine requirement + shared `:lens`/`*Lens` recency. |
| Q5 | `vql/compiler.cpp` OR doc-scope | `StoreGenerator` `lens` under `--engine vortex` maps to `home()` — either compile it or scope the design to the direct engine | The compiler path is a separate surface. |
| Q6 | `session_ops.{hpp,cpp}` + `sql_lens.{hpp,cpp}` | `sessionBrowseAt(session, key, table)` — mount-scoped resolver + `browse` + `refresh()`; a `SqlLens` accessor (or browse-returns-key) to yield the new query's root; `sessionUnmountAt` pre-resolves `mountBoundary`→`cellFor`; new `session_ops.cpp` keeps `SqlLens` out of the header | The browse resolver shared between vlace and VQL. |
| Q7B | `vql_engine.cpp` + `vquery/main.cpp` | `sessionFor` guard at every link-writing site: `link(...)` step + `cloneGenerator`, `performCreates` (both `in` and `attachPoint`), `PathMorphism`, `cloneTail`/perspective clones, `shapeProjection`, `executeWatch`; **and** on the `promote`/`--in-place`/`writeResultSlice`/`:save` result-persistence path | D7 — the provenance guard no other row owns. |
| Q7 | `tests/` (new) | VQL lens journey — `fsLens` → traverse → `lensMount` → back → `lensCommit`/`lensRevert`/`lensNext`/`lensStub`/`lensBrowse`; sole-anchor refusal (mutating step inside a `where`) | A test sits with its code — the acceptance artifact. |

The session machinery (`session_ops`, `ownsCell`, `canApply`, `revertKey`, `commit()`→`expected<int>`,
`windowedFor`/`nextStubAt`, `openLensUri` guard) is the P5–P8 layer — VQL's steps call it.

## 8. What this is not

- Not the `:lens` REPL verbs — those stay.
- Not a pipe operator (`|` is union-type-only) — the step-function surface is the existing
  "relation as arg" model.
- Not a session stack — "back" is a bound variable.
- Not yet xuzz's picker surface — different frontend's doc.
