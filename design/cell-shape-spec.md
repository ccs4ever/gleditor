# Cell-Shape Specification: One Neighborhood-Match Kernel for the Vortex Frontends (v1.7)

A design specification for the **shared cell-shape kernel** — `zigzag/cell_shape.{hpp,cpp}`, beside
`session_ops.hpp` (`apps/common/xanadu/zigzag/`) in the "one copy of the protocol" role — and for how
each Vortex frontend lowers its own pattern surface into it. Design deliverable only: the signatures
are proposals; `cell_views.hpp`'s `CellGraph` and `session_ops.hpp`'s precedent are the seam, not a
new layer. The sibling specs are [`vql-lens-bridge.md`](vql-lens-bridge.md),
[`vpl-lens-bridge.md`](vpl-lens-bridge.md), [`vicon-lens-bridge.md`](vicon-lens-bridge.md),
[`vprolog-lens-bridge.md`](vprolog-lens-bridge.md), and
[`vlace-lens-bridge.md`](vlace-lens-bridge.md) — the shape capability is the same for all five; only
the surface is re-thought per language.

**The thesis**: shape matching is a *flat* per-cell neighborhood test — for each candidate cell,
does `linked(cell, dim, dir)` answer a presence, an absence, a value, a binding — under conjunction.
**Recursion is outside the kernel**: each frontend iterates by its own means (VQL's `*`/`+` fixpoint
and `for` FLWOR; VPL's view algebra; vicon's suspended requests and re-drive; vprolog's
backtracking; vlace's laziness). The kernel's one concession to depth is *structural*: a field
constraint whose expected value is itself a `ShapeExpr` descends one level — the descent is in the
expression, not the driver.

______________________________________________________________________

## 1. Intent

A shape spec written once should mean the same thing in every frontend:

- **VQL** — `[{shape}]` step filters, `?{shape}` predicate tests, `for {…} in path` destructuring,
  and `Cell {…}` refinement types all lower to one matcher instead of three readings that disagree;
- **VPL** — a `⎕SHAPEIDX` verb over the Q10 cell-list `VplView` yields the *indices* of matching cells
  (the `⍸`-like mask-idiomatic form — `V[idx]` then selects with identity, `vpl-lens-bridge.md:365`;
  `⌿` copies, `vpl_engine.cpp:604-644`, so a filtered `⎕MATCH` view stays as the convenience but
  `⎕SHAPEIDX` is the array-native form);
- **vicon** — `match(c, spec)` is a suspended request and `descendants(c)` a generator, with Int64
  ref-token results like every other vicon cell crossing;
- **vprolog** — `lens_shape(C, Pattern)` unifies a clause-tree pattern term against a handle, its
  leaf vars binding Int64 handles and text (cells are never unification targets);
- **vlace** — `shapeOf :: Cell -> Shape` reads a neighborhood into an ADT and `matchShape :: Cell ->
  ShapeSpec -> Option (List (String, Cell))` answers bindings, lazily forced.

And the shared helpers every frontend re-implements today: `cellKey`/`cellScalar`/`cellEq`/`keyCell`
(spec'd for VQL at `vql-lens-bridge.md:245-248`; implemented only in vlace, `compiler.cpp:1215-1281`,
minus `keyCell`, which is unimplemented anywhere) and `compareCellValues` (the lift of VQL's
`compareValues`, `vql_engine.cpp:2145-2199`).

Acceptance: the worked example of §5 — one shape ("a `dir` entry whose firstborn has a `d.seq`
successor") — expresses in all five syntaxes over one `dir:` session and answers the same cells.

## 2. Current surface (already in the build)

### The shared substrate

| surface | what it does |
|---|---|
| `CellGraph` concept | `linked`/`dimensionsOf`/`cells`/`cellCount`/`traversalBound`/`contains`/`slot` — satisfied by `Manifold` **and** `ArenaManifold`, so a kernel written once reads a store fold and a live arena alike (`cell_views.hpp:52-61`) |
| `step`/`hop`/`rank`/`neighbours`/`dimensionRefsOf`/`cellRefs` | the lazy ranges a matcher enumerates with (`cell_views.hpp:71-95,:213-217,:310-341`) |
| `linksAlong`/`holdsValue`/`rankContains` | the predicate-factory idiom (`views::filter`-ready lambdas) and the `if constexpr requires` optional-method pattern (`cell_views.hpp:346-387`) |
| `session_ops.hpp` | the precedent this kernel follows — one copy of the protocol, caller names the cell, helpers take `std::expected`-returning verbs (`session_ops.hpp:3-25`) |
| `linked` vs `linkedMinting` | `linked` is a pure read — "answers noCell for a foreign neighbour nobody has proxied"; `linkedMinting` mints a `proxyFor` cell for a foreign neighbour — **a write inside a read** (`arena_manifold.hpp:482-504`, `arena_manifold.cpp:919-950`) |
| `dimensionsOf` | the DimLink span — the valence substrate **through an explicit effective-neighborhood seam** (`CellGraph` doesn't expose one graph: `linked` delegates proxies, `slot`/`contains` answer through base, `dimensionsOf` returns local-OR-foreign not union, `cells()` returns `slots_` only; the kernel injects `effectiveNeighbours`/`effectiveCandidates` — or scopes `cellsMatching` to caller-supplied cells) (`dimensionsOf` on a proxy delegates to the foreign manifold's run — foreign DimRefs + foreign CellRefs with no `dimOut` to translate back, `arena_manifold.cpp:960-971`; Closed/base/`{*}`/`{!*}`/`descendants`/`shapeOf` must scope to locally-named structure or `dimensionsOf` gains a translating variant) (`arena_manifold.cpp:952-964`) |
| `denseOf` | ref→dense-index decoding/membership arithmetic, unmasking the ephemeral bit (`arena_manifold.cpp:257-270`) — **not a shape operator**: it answers "does this arena hold this ref", never "what links does the cell carry" (§3, D4) |
| `ownsCell`/`keyFor`/`cellFor` | the session identity layer on `ProjectionSession` (`projection_session.hpp:41-60`) |
| `sessionFor` | the `sessions_`→`ownsCell` scan — exists only in vlace's `Compiler` (`compiler.cpp:301-306`, `compiler.hpp:227-236`) |
| `CellValue`/`render` | `variant<string,double,int64_t,bool>` — no `CellRef` alternative — plus `VortexCore::render`/`isTruthy` (`vortex_core.hpp:29,:202-203`) |

### VQL — three readings of one grammar, and they disagree

| reading | site | semantics today |
|---|---|---|
| `[{…}]` step predicate | `shapePredicates` (`ast.hpp:308`, parsed `parser.cpp:744`, evaluated `vql_engine.cpp:1953-1970`) → `evaluateShapePredicate` (`:2369-2422`) | flat conjunctive match — with gaps, below |
| `?{…}` / bare `{…}` predicate | `PredicateTest` (`ast.hpp:173-179`, parsed `parser.cpp:1398-1422`) | bare `{…}` → `evaluateShapePredicate` (`vql_engine.cpp:2226-2230`); `?{…}` → `evaluateValueExpr`'s ShapeLiteral arm **`return true` unconditionally** (`:2128-2130`) — a vacuous-true wrong answer |
| `for {…} in path` / `Cell {…}` / `watch … as {…}` / lambda params | `DestructurePattern` (`ast.hpp:358-370,:421-425,:465,:496`) → `matchDestructure` (`vql_engine.cpp:177-221`) | binding — `MissingField::Refuse`/`BindEmpty` (`vql_engine.hpp:170`), typed fields via `satisfiesType` (`:209-211`) |
| `$path{…}` projection | `shapeProjection` (`ast.hpp:337`, parsed `parser.cpp:446`, applied `vql_engine.cpp:1046-1089`) | **mutating link-write** — `link(c,dim,dir,sprout)` + reciprocal (`:1080-1083`) — a fourth reading, not a match at all |

`evaluateShapePredicate`'s gaps, against the spec's own §4.8.1 table
(`design/vql-query-language.md:997-1019`):

- `{}` — spec'd "valence-0 check"; an empty `rules` vector falls through the loop → **vacuous true
  for every cell** (`:2374-2421`);
- `{*}` — spec'd "valence ≥ 1"; `ShapeWildcard::Everything` hits `continue` → **vacuous true**
  (`:2376-2381`);
- `{!*}` — spec'd "empty base"; `ShapeWildcard::Nothing` hits `return false` → **unconditional
  fail** (`:2378-2380`);
- nested `{…}` — a `shared_ptr<ShapeLiteral>` rule re-matches **the same cell** (`:2412-2418`) —
  there is no descent form (§3, D3);
- `+d.dim: {…}` — parses (`targetValue` is a `ValueExpr`, and `ValueExpr` admits
  `shared_ptr<ShapeLiteral>`, `ast.hpp:122`); evaluated it compares `render(neighbor)` against
  `evaluateValueExpr`'s vacuous `true` — **a wrong answer, not an error** (`:2404-2410` +
  `:2128-2130`);
- `$shape`/`func(.)`/`d.a=>d.b`/`d.x = expr` rules — parsed (`parser.cpp:1781-1795,:1823-1837`) but
  no evaluator branch → **silently ignored** (the `if`-chain at `:2376-2419` has no arm and no
  `else`);
- every neighbor test runs `linkedMinting` (`:2387-2389`) — a mark-context mint hazard (D4);
- `PredicateTest::FunctionInvocation` — a bare `fn(args)` predicate parses (`parser.cpp:1413-1417`)
  but `evaluateFactor`'s `holds_alternative` chain has no arm for it (`vql_engine.cpp:2212-2231`) →
  `result` stays `false` — **the dead arm**. (`?fn(…)` is a different path: `?` parses to the
  `ValueExpr` variant, `parser.cpp:1404-1407`, which *does* dispatch through `evaluateValueExpr`'s
  function table, `:2057-2116`.)

### The other four — no shape surface at all

- **VPL** — `VplView` is origin+axes (`view.hpp:36-37`); `collectCells`/`shape` walk links lazily
  (`:201-258,:289-332`); the only mask, `mask ⌿ view`, **copies** the selected cells
  (`makeCell`/`makeScalarCell`/`core_->value`, `vpl_engine.cpp:622-633`) — identity destroyed; no
  cell-list mode (`vpl-lens-bridge.md` Q10).
- **vicon** — produce/`Bind`/`Resolve` copy *rendered* values; the Int64 ref-token is the only cell
  currency (`vicon-lens-bridge.md:52-53`, `vortex_vm.cpp:48-64`). No traversal verb beyond the
  proposed `kids`/`step`.
- **vprolog** — `cell_link`/`cell_value`/`cell_span` decode/emit Int64 handles
  (`vortex_stdlib.cpp:987-995,:1029,:1063-1067`); `Vlog::bind` writes a clone-rank tail splice
  (`vlog.cpp:86-94`) — a *projected* cell must never reach `unify`; `=` on two Int64 scalar cells is
  already identity (`vlog.cpp:115-123`).
- **vlace** — `linked`/`childrenAll`/`dimensionNamed`/`cellText` exist (`compiler.cpp:1002-1028`,
  `vlace-lens-bridge.md:36-42`); `cellKey`/`cellEq`/`cellScalar` are the only implemented identity
  helpers (`compiler.cpp:1215-1281`); recursion is the program's `case`/`foldr`, lazily forced
  (`force`/`Thunk`, `compiler.cpp:308+`).

## 3. Design decisions

### D1 — the kernel is the flat predicate; every driver stays frontend-side

`shapeMatches(graph, cell, shape, …)` answers one question about one cell: do the field constraints
hold of `cell`'s immediate neighborhood — `linked(cell, dim, dir)` presence/absence, `render` of the
neighbor against an expected value, a `Bind` capturing the neighbor — all under conjunction, plus the
valence bounds the wildcard algebra spec'd. It never walks a rank, never iterates the arena for a
caller, and never decides how many times to run.

`cellsMatching(graph, shape, render, candidates)` is the one concession to enumeration — the caller supplies the candidate range (`cellRefs(m)` = `slots_` only — unshadowed base cells that `contains()` answers true for are never candidates; the caller walks `traversalBound()`/`neighbours`/its own enumeration and passes the range in) filtered
through `shapeMatches` — because "which cells of this manifold satisfy" is itself a per-frontend
question VQL asks as a generator anchor and VPL as a mask; it ticks `consumeWork` per candidate via
the `if constexpr requires` pattern `cell_views.hpp:364-367` already uses for `rankContains`.

Everything transitive — `*`/`+` fixpoint (`vql_engine.cpp:1373-1388` iterates `runOnce` to a
`next == cur || next.empty()` bound, capped at 1000), `for` FLWOR (`:2274-2299`), vicon re-drive,
vprolog backtracking, vlace laziness — stays where it is. The kernel matching one cell means each
frontend's multiplicity rules are untouched: no new hidden-eval hazard for VQL's D3 positional rule,
no new mark interaction for vicon/vprolog beyond D4 below.

### D2 — the grammar-neutral form

A shape is a set of field constraints over a cell's neighborhood, a base, and a valence bound:

```cpp
namespace zigzag {

enum class ShapeBase : std::uint8_t {
  Open,   // {*} / plain {…} — dims the shape does not name are unconstrained
  Closed, // {!*} / {}      — the cell may link only on the dims fields name
};

enum class ShapeMode : std::uint8_t {
  Require,      // the signed link must exist
  Forbid,       // it must not
  Bind,         // it must exist (or `optional`) and the neighbor binds `bindName`
  RequireValue, // it must exist and the neighbor satisfies `expected`
};

enum class ShapeCmp : std::uint8_t { Eq, Ne, Lt, Gt, Le, Ge };

struct ShapeExpr;
/// What a RequireValue/Bind field tests the neighbor against.
using ShapeExpected =
    std::variant<std::monostate,                    // presence/absence only
                 vortex::CellValue,                 // render-compare (Eq)
                 std::shared_ptr<ShapeExpr>,        // descent — D3
                 std::function<bool(CellRef,        // frontend-defined test —
                                    CellRef)>>;     //   VQL's arbitrary `: expr`

struct ShapeField {
  DimRef dim{noCell};                 // resolved DimRef — name→dim is the frontend's
  DimVector dir{DimVector::POS};
  bool bidir{false};                  // `~` — either direction satisfies, both must be
                                      // absent under Forbid
  ShapeMode mode{ShapeMode::Require};
  ShapeExpected expected{};
  std::string bindName{};             // empty binds nothing
  bool optional{false};               // Bind-only: absent binds nothing instead of
                                      // failing (VQL's `T?` / `MissingField::BindEmpty`)
};

struct ShapeExpr {
  ShapeBase base{ShapeBase::Open};
  std::size_t minValence{0};          // `*` asserts ≥1 (the spec's "has any connection")
  std::vector<ShapeField> fields;
  std::vector<std::shared_ptr<ShapeExpr>> subShapes;   // same-cell conjunction — a bare nested `{…}`
                                                        // has no dim to hang a field on; `$shape`/`func(.)`/`d.x = expr`/`d.a<=>d.b` rules also land here (shape-level, not field-level — the `std::function` arm can't carry them since they have no hop and no neighbor)
};

/// Flat name → bound cells; nested binds merge into the same map (D3).
using ShapeBindings = std::unordered_map<std::string, std::vector<CellRef>>;
```

The lowering rules that make this the least common denominator:

- **bidir folds into the field** (`isBidir`, `ast.hpp:136`) — a `~d.tag` Require is satisfied by
  either side, a `~d.tag` Forbid requires both sides absent — and Forbid tests **edge-existence, not name** (`linked` on a proxy returns `nullopt` whether the edge is absent or merely unproxied — a vacuous pass while the foreign edge exists; the per-mode rule: Require/Bind want the name via `linked`, Forbid wants existence via a non-minting `edgeExists` probe — the `OptionalCell` hop can't express "exists-but-unnamed" so Forbid needs the probe arm — `edgeExists` joins the `CellGraph` optional-method set (the `if constexpr requires` pattern `rankContains` uses, `cell_views.hpp:364-367`); a proxy subject's `dimensionsOf`/`Forbid`/Closed/`{*}`/`{!*}` are **scoped to locally-named structure** (a proxy returns the foreign run verbatim — foreign DimRefs never equal local `field.dim`; the kernel refuses or skips proxies for base/valence checks)) — exactly
  `evaluateShapePredicate`'s `linkedPos`/`linkedNeg` pair (`vql_engine.cpp:2386-2394`).
- **`expected` carries the value channel orthogonally to mode** — VQL's `: "mp4"` is `RequireValue`
  with a `CellValue`; `: $t` on a destructure field is `Bind`; `: {…}` is the descent arm; an
  arbitrary ValueExpr becomes the `std::function` arm so `+d.tag: ./d.old` still evaluates per
  context cell (`vql_engine.cpp:2405` evaluates `dr.targetValue` with `context` — the candidate —
  the closure captures the expr, not a precomputed value).
- **valence is two facts, not a syntax**: `base` (Open/Closed — the `*` vs `!*` distinction) and
  `minValence` (what bare `*` contributes). `dimensionsOf` is the counting substrate
  (`arena_manifold.cpp:952-964`); a Closed base checks every live DimLink — **edge-bearing** (`pos != noCell || neg != noCell` — `unlink`/`link(…,nullopt)` zeroes a side and leaves the entry, `arena_manifold.cpp:1318-1376`; `linkCount`/`liveLinks_` count entries not edges; `{*}`/`{!*}`/`{}`/Closed must filter dead entries)'s dim is named by a
  non-Forbid field. **`{}` lowers to `Closed` with no fields** — the empty literal is the one place
  the surface defaults Closed, honoring the spec's "valence-0" row (`vql-query-language.md:1003`)
  without making bare `{+d.a}` exclusive.
- **`: $v` in a predicate shape is a scoped-commit bind** — `[{…}]`/`?{…}` sites pass a trial
  `ShapeBindings`; on match the frontend commits the names to its scope (VQL `env_`, vprolog
  `unify`, vlace `EnvPtr`); on fail the trial is discarded — a bare `VariableRef` `targetValue`
  lowers to `Bind` (not `RequireValue`), matching `vql-query-language.md:1050-1051`.
- **`MissingField` is a callsite policy, not a field property**: `shapeMatches` takes
  `MissingPolicy { Refuse, BindEmpty }` — VQL passes Refuse from `Cell {…}` types and `watch … as`,
  BindEmpty from `for {…}` (`vql_engine.cpp:213-214,:2283-2293`) — and per-field `optional`
  overrides it (the `T?` arm, `:209-211`).

### D3 — descent lives in the expression; bindings are flat

The honest answer to the recursive question: **a field's `expected` holds a nested `ShapeExpr`,
meaning "the neighbor on this signed dim must itself match this shape."** `shapeMatches` recurses on
the *neighbor* — not a depth parameter on the driver, because a depth parameter answers a question no
frontend asked (each has its own iteration) and can't express the asymmetric shapes programs actually
write ("the `d.contains` neighbor has a `d.seq` neighbor", not "depth 2").

The binding-currency decision: **the kernel walks the nested shape and merges its binds into the
caller's `ShapeBindings` — flatly**. Three options were live:

1. *Flat merge* — a `bind` inside a nested field writes the same map the outer fields write.
   Matches VQL's flat `Bindings`/`env_` (`vql_engine.cpp:216-218` overwrites by name), vprolog's
   clause-scope vars, vicon's per-site flat scope; the two frontends without named binds (VPL's
   mask, vicon's produce order) never look at the map. **Chosen.** Collision rule: later fields win
   — the same assignment semantics every frontend's env already has; a frontend that wants scoping
   namespaces names at lowering.
2. *Hierarchical bindings* — a tree of maps. Buys shadowing nobody's surface exposes (VQL has no
   nested-scope bind syntax); every consumer would flatten anyway.
3. *Handles only* — nested fields bind a cell and the frontend re-calls. That is each frontend's
   own loop rendered as boilerplate; the `bindName` on the field carrying the nested shape already
   gives this for free (below), so the option survives as a composition, not a mode.

A field carrying a nested shape may also `bindName` — `+d.contains` with `expected = {…}` and
`bindName = "first"` binds the neighbor *and* asserts its subshape — the escape hatch that hands a
frontend the handle to recurse on itself (vlace's lazy `List (String, Cell)` needs the handle, not a
kernel-walked tree).

VQL's parser changes are minimal: `+d.dim: {…}` already parses (`targetValue` admits a
`ShapeLiteral`-`ValueExpr`, `parser.cpp:1839-1842` + `ast.hpp:122`); the lowering recognizes a
`ShapeLiteral` there and produces `expected = nested` instead of a value-compare. The *bare* nested
`{…}` rule keeps its current semantics — re-matching the same cell is the spec's own
shape-composition reading (`{ $shape_a, $shape_b }`, `vql-query-language.md:1026-1027`), now honest
rather than a gap: a bare nested shape is a conjunction on the same cell, a field-carried one is a
descent.

### D4 — `linked` by default; `linkedMinting` is opt-in

`evaluateShapePredicate` and `matchDestructure` reach neighbors through `linkedMinting`
(`vql_engine.cpp:2387-2389,:194-199`), which mints a `proxyFor` cell when the neighbor lives in a
foreign space (`arena_manifold.cpp:919-950`). A predicate that mints is a hazard in every marking
context — vicon's choice-point marks and vprolog's per-alternative marks truncate mints on
`release()`, and `ProjectionSession::rewind` refuses on foreign mints
(`projection_session.cpp:415-422`, per vlace D3). The kernel therefore takes its hop as an injected
resolver — `function_ref<OptionalCell(CellRef, DimRef, DimVector)>` defaulting to the graph's
`linked` — and a frontend that *wants* federation reach (VQL's store-walking steps genuinely need
it, `vql_engine.cpp:1102-1112`) passes `linkedMinting` explicitly. Matching stays a read.

Valence uses `dimensionsOf`, never `denseOf` — `denseOf` is ref-decoding arithmetic
(`arena_manifold.cpp:257-270`), and an ephemeral ref unmasking to a live slot says nothing about
links. The correction matters because `denseOf(c).has_value()` is a tempting wrong shortcut for
"cell exists"; the `{}`/`{*}`/`{!*}` checks count live `DimLink`s.

### D5 — the value channel: `compareCellValues` lifted, `render` injected

`compareValues` (`vql_engine.cpp:2145-2199`) is already the cross-frontend comparison contract —
numeric coercion with a `1e-9` epsilon, string fallback, the six `CompOp`s — and every frontend
needs the identical function (vprolog's `cell_value` comparison and vlace's `cellScalar` arms both
re-derive it). The kernel lifts it to `zigzag::compareCellValues(CellValue, ShapeCmp, CellValue)`.

The kernel never renders on its own: `render` is `VortexCore` state (`vortex_core.hpp:202`), and a
`Manifold` fold has none — so `shapeMatches`/`cellsMatching` take
`function_ref<vortex::CellValue(CellRef)> render`. `RequireValue` with a `CellValue` expected lowers
to `compareCellValues(render(neighbor), Eq, expected)` — the semantics `vql_engine.cpp:2404-2410`
already implements, now shared.

`cellScalar` is the one helper that reads `render` the other way: it yields the cell's *scalar*
payload only for the lens-minted kinds `Int64`/`Double`/`Bool` (`mintNodeCell`,
`projection_session.cpp:95-116`) — text cells and the handle kinds (`OpHandle`/`ExternRef`/
`ForeignOpHandle`, `arena_manifold.cpp:1086-1094`) yield `nullopt` by design, matching both existing
implementations (vlace `compiler.cpp:1255-1281`; VQL `vql-lens-bridge.md:247`).

### D6 — session identity helpers take an injected `sessionFor` until Q4 lands

`cellKey`/`cellScalar`/`cellEq`/`keyCell` are the same four functions in every frontend. The kernel
carries them, taking the session-resolution seam as a parameter because the registry
(`VortexCore::lensSessions()` — VQL's Q4 row, shared by vicon's V4 and vprolog's register) is a
proposal, not code:

```cpp
using SessionScan =
    gleditor::cpp26::function_ref<ProjectionSession *(CellRef)>; // ownsCell scan
using SessionList = std::span<ProjectionSession *const>;

[[nodiscard]] std::optional<ExternalKey> cellKey(SessionScan sessionFor, CellRef cell);
[[nodiscard]] bool cellEq(CellRef a, CellRef b) noexcept;        // CellRef identity —
[[nodiscard]] std::optional<vortex::CellValue>                   //   `=` compares renders
    cellScalar(function_ref<vortex::CellValue(CellRef)> render, CellRef cell);
[[nodiscard]] std::optional<CellRef> keyCell(SessionList sessions,
                                             const ExternalKey &key);  // cellFor first-hit
[[nodiscard]] ProjectionSession *sessionForCell(SessionList sessions, CellRef cell);
```

When `lensSessions()` lands, each frontend's `SessionScan` closure becomes a one-line scan of the
registry (vlace's `sessionFor` loop, `compiler.cpp:301-306`, verbatim); until then vlace passes its
own `sessions_`, VQL/vicon/vprolog inject the coordinator-side list their bridge docs already
specify, and VPL injects the engine's. `cellEq` is deliberately trivial — naming the contract is the
point: `=` on cells stringifies operands and compares rendered text (`vql_engine.cpp:2145-2199` — a
wrong-answer comparison, not a type error), `==` on vlace cells is a `TypeMismatch`
(`compiler.cpp:195-225`), and only vprolog's `=` is already identity (`vlog.cpp:115-123`).

### D7 — each frontend lowers its own idiom

The kernel is grammar-free on purpose; the per-frontend lowering is where syntax lives.

**VQL** — the richest surface and the most gaps to close:

- `[{…}]` step predicates and bare `{…}` predicate tests lower `ShapeLiteral` → `ShapeExpr`
  rule-for-rule (`*`/`!*` → base+minValence, `!`/`~`/signed dims → mode/flags, `: expr` →
  `RequireValue`, `: {…}` → nested `expected`, `$var`/`fn(.)`/pivot/virtual-dim rules → the
  `std::function` arm or refused-as-unimplemented, no longer silently ignored);
- `for {…} in path`, `Cell {…}` refinements, `watch … as {…}`, lambda params — `DestructurePattern`
  → `ShapeExpr` with `Bind` fields, per-field `optional` from `T?`, callsite `MissingPolicy`
  (`matchDestructure`'s two existing policies, `vql_engine.hpp:170`), typed fields still checked by
  `satisfiesType` *after* the kernel binds (the type layer stays VQL's);
- `?{…}` — `evaluateValueExpr`'s `return true` arm (`:2128-2130`) routes to `shapeMatches` on the
  context cell;
- **the `PredicateTest::FunctionInvocation` dead arm** — `evaluateFactor` gains the arm, dispatching
  through `evaluateValueExpr`'s function table (`:2057-2116`) — a bare `contains(.)`/`fn(…)`
  predicate stops silently evaluating false;
- `$path{…}` shape projection stays a VQL-side mutating weave (`:1046-1089`) under the Q7B funnel —
  the kernel is the *read* half only;
- `linkedMinting` → `linked` in the matcher (D4); the traversal steps keep `linkedMinting` because
  store-walking genuinely mints.

**VPL** — `⎕MATCH` is the mask verb, and it **requires Q10's cell-list `VplView`** (`vpl-lens-bridge.md:450`):
`'(+d.contains: (+d.seq), +d.role: "dir")' ⎕MATCH V` parses the string operand through the shared
grammar into a `ShapeExpr`, filters `V`'s cells, and yields the surviving cells as a cell-list view —
identity-preserving, because `⌿` copies (`vpl_engine.cpp:622-633`) and cannot be the mechanism. A
`⎕SHAPE` quad compiling the string once into a reusable spec cell is the optional second form for
loops. Per-cell failure drops like every other VPL probe (reasons on `⎕LERR`); a malformed spec
string throws.

**vicon** — `match(c, spec)` is a **suspended request** on the same second-`Halt` gate the lens
verbs use (`vicon-lens-bridge.md` D1): `spec` is the shared textual grammar written into a request
arg cell (compile-time mint, like `step`'s dim), the Driver decodes and runs `shapeMatches`
host-side, and the request **fails** on non-match — `if match(c, spec) then …` is the guard idiom.
On success it produces the subject's ref-token, then — through the site's answer queue,
REPLACE-on-refire like `kids` — each `Bind` field's cell in field order, so `every b := match(c,
s)` yields subject-then-binds. `descendants(c)` is a **generator** producing every cell reachable
from `c` along posward links on any dim — `neighbours(m, c, POS)` (`cell_views.hpp:310-318`)
breadth-first under the `traversalBound` cycle guard — each result an Int64 ref-token, validated
`contains()`+epoch on queue pop like every vicon ref (D5 there).

**vprolog** — `lens_shape(C, Pattern)` lowers a **clause-tree term** to `ShapeExpr`: `Pattern` is a
list of `dim(Name, Dir, Constraint)` terms where `Constraint` ∈ `present | absent | bind(V) |
text(Atom) | sub(V, FieldList) | sub('_', FieldList)`, optionally preceded by `'*'`/`'!*'` for the
base. The builtin runs `shapeMatches` host-side, then unifies each `bind`/`sub` var with a fresh
`makeScalarCell(int64(cell))` handle under the mark/unify/recurse/release idiom
(`vortex_stdlib.cpp:1020-1053`) — **leaves are Int64 handles and text, never cells**, because
`Vlog::bind`'s clone-tail splice on a session cell is the D1 hazard. `sub(F, …)` binds `F` to the
neighbor and descends — the flat-merge rule means `F` is just another clause var. Failure prunes;
a re-driven goal re-matches deterministically.

**vlace** — two natives in the `Option`/`List` idiom:

```vlace
data Shape      = Shape (List (String, Dir, Cell))      -- the observed neighborhood
data Constraint = Present | Absent | Bind String
                | TextOf String | Sub ShapeSpec
data Field      = Field String Dir Constraint
data ShapeSpec  = Spec ShapeBase (List Field)

shapeOf    :: Cell -> Shape                        -- lazily forced (D3 there)
matchShape :: Cell -> ShapeSpec -> Option (List (String, Cell))
```

`matchShape` is pure-typed with vlace's stated force-time semantics — a `Cell` is a stable handle
into mutable structure, the match observes the arena as of when the thunk is forced
(`vlace-lens-bridge.md` D3). `Some` carries the flat binding list (order = field order); `None` is
no-match — a refusal can't exist (matching never `Err`s). The `Sub` field's bound cell is the
handle a program recurses `matchShape` on itself — D3's escape hatch used in the lazy idiom.
`shapeOf` is the reflective half: `neighbours`/`dimensionRefsOf` read into an ADT a program can
`foldr`/`case` over without a spec at all.

## 4. Proposed surface

### The kernel — `apps/common/xanadu/zigzag/cell_shape.hpp` (+`.cpp` for the non-template helpers)

```cpp
namespace zigzag {

// D2 types: ShapeBase, ShapeMode, ShapeCmp, ShapeField, ShapeExpr, ShapeBindings
enum class MissingPolicy : std::uint8_t { Refuse, BindEmpty };

/// One-cell neighborhood match. `hop` defaults to the graph's linked() —
/// pass linkedMinting explicitly where federation reach is wanted (D4).
template <CellGraph M>
[[nodiscard]] bool
shapeMatches(const M &graph, CellRef cell, const ShapeExpr &shape,
             cpp26::function_ref<vortex::CellValue(CellRef)> render,
             ShapeBindings *bindings = nullptr,
             MissingPolicy missing = MissingPolicy::Refuse,
             cpp26::function_ref<OptionalCell(CellRef, DimRef, DimVector)> hop);

/// The cells of `graph` satisfying `shape` — cellRefs | filter, ticking
/// consumeWork per candidate where the graph provides it (D1).
template <CellGraph M>
[[nodiscard]] std::vector<CellRef>
cellsMatching(const M &graph, const ShapeExpr &shape,
              cpp26::function_ref<vortex::CellValue(CellRef)> render);

/// The predicate factory, cell_views idiom — cells | filter(matchesShape(…)).
template <CellGraph M>
[[nodiscard]] auto matchesShape(const M &graph, const ShapeExpr &shape,
                                cpp26::function_ref<vortex::CellValue(CellRef)> render);

/// VQL's compareValues, shared — numeric-epsilon then string fallback (D5).
[[nodiscard]] bool compareCellValues(const vortex::CellValue &left, ShapeCmp op,
                                     const vortex::CellValue &right);

// D6 — the session identity helpers (injected until lensSessions() lands)
}
```

### Per-frontend surfaces

| frontend | surface | lowering |
|---|---|---|
| VQL | `[{…}]`, `{…}`, `?{…}` predicates; `for {…} in`, `Cell {…}`, `watch … as`, lambda params | `ShapeLiteral`/`DestructurePattern` → `ShapeExpr` per D7 |
| VPL | `'spec' ⎕MATCH view` → cell-list view; `⎕SHAPE` compile (optional) | shared text grammar → `ShapeExpr`; needs Q10 |
| vicon | `match(c, spec)` request; `descendants(c)` generator | string arg → `ShapeExpr` host-side; ref-token produces |
| vprolog | `lens_shape(C, Pattern)` | term → `ShapeExpr`; `unify` on minted Int64 handles |
| vlace | `shapeOf`, `matchShape` natives + `Shape`/`ShapeSpec`/`Constraint`/`Field` ctors | ADT → `ShapeExpr`; lazy `Option (List (String, Cell))` |

## 5. Worked example — one spec, five spellings

The same `dir:` session over `/tmp/lensj` (the bridge docs' fixture): a `RouterLens` projects the
listing — a root, `+d.contains` to the firstborn entry, entries chained `+d.seq`, attribute cells on
`d.role`/`d.mime`/`d.size`/`d.mtime`/`d.perm` (`vlace-lens-bridge.md:164-168`). The shape: **"a cell
whose `+d.role` neighbor renders `dir` and whose `+d.contains` neighbor itself links `+d.seq`"** —
one value constraint, one nested descent, one bind.

```vql
-- VQL: the spec is the literal; [ … ] is the step filter
fsLens("/tmp/lensj")/d.contains/d.seq::rank[
    { +d.role: "dir", +d.contains: $first { +d.seq } } ]
-- (: $first {…} is the proposed bind+descent form; today write the two
--   constraints on one dim as two fields — Open base allows it :)
```

```vpl
      ⍝ VPL: spec string -> ShapeExpr; MATCH yields a cell-list view (Q10)
      L ← (lens ⍴ d.contains)[1] ⍴ d.seq
      M ← '(+d.role: "dir", +d.contains first (+d.seq))' ⎕MATCH L
```

```vicon
# vicon: suspended request — fails to prune, produces subject then binds
every c := kids(r) do
  if match(c, "+d.role: \"dir\", +d.contains $first {+d.seq}") then
    write(ltext(c))
every d := descendants(r) do write(ltext(d))
```

```prolog
% vprolog: the pattern is a term; vars bind Int64 handles
listing_entry(C, First) :-
    lens_open(dir('/tmp/lensj'), R),
    lens_children(R, C),
    lens_shape(C, [ dim('d.role',    pos, text('dir')),
                    dim('d.contains', pos, sub(First,
                        [ dim('d.seq', pos, present) ])) ]).
```

```vlace
-- vlace: the spec is data; the match is lazily forced
spec = Spec Open
         [ Field "d.role"    Pos (TextOf "dir")
         , Field "d.contains" Pos (Sub (Spec Open
                                    [ Field "d.seq" Pos Present ])) ]
case matchShape entry spec of
  None      => putStrLn "no"
; Some binds => …                  -- binds :: List (String, Cell)
```

## 6. Open questions

1. **Flat-merge collisions** — D3 picks last-wins to match `env_` semantics; a `!` strictness flag
   (rebinding a live name fails the match) is cheap insurance if a lowering ever generates
   overlapping names. Deferred until one does.
2. **Closed-base counting** — under `Closed`, must every *live* DimLink's dim be named by a
   non-Forbid field, or do Forbid-named dims count as "named" (matching nothing by construction)?
   The former is implemented; the spec doesn't distinguish `{!*, +d.a}` from `{!*, +d.a, !d.b}`.
3. **`shapeMatches` under `Manifold` vs `ArenaManifold`** — `CellGraph` covers both, so `meta:` fold
   cells are matchable where a frontend has one (vprolog's REPL); the render injection differs (a
   fold has no `VortexCore::render`) — each frontend supplies its own.
4. **`descendants` direction** — posward-only is specified; a bidir form wants a second request arg,
   deferred with `step`'s negward (vicon §6.7).
5. **The `std::function` `expected` arm** — owning because a `ShapeExpr` outlives a call
   (`function_ref` is call-bound only, per `cpp26` guidance); a type-erased non-owning form is a
   later optimization.
6. **`?{…}` fix placement** — routing inside `evaluateValueExpr` (minimal) vs. rewriting `?` to
   parse to the `PredicateTest::ShapeLiteral` arm (cleaner, touches the grammar) — decided at
   implementation.
7. **keyCell across epoch boundaries** — the handle survives by re-resolution (`cellFor`), but the
   vicon/vprolog epoch machinery (D5/D4 there) is per-frontend today; the kernel helper takes the
   session list and stays neutral.

## 7. Proposal register

| S | files | change | why not caller-level |
|---|---|---|---|
| S1 | `apps/common/xanadu/zigzag/cell_shape.{hpp,cpp}` (new) | `ShapeExpr`/`ShapeField`/`ShapeBindings`/`ShapeMode`/`ShapeBase`/`ShapeCmp`/`MissingPolicy`; `shapeMatches`/`cellsMatching`/`matchesShape` over `CellGraph`; `compareCellValues` (lifted `vql_engine.cpp:2145-2199`); `cellKey`/`cellScalar`/`cellEq`/`keyCell`/`sessionForCell` with injected `SessionScan`/`SessionList` | The kernel is the "one copy" — beside `session_ops.hpp`, over the existing `CellGraph` substrate |
| S2 | `vql/vql_engine.{hpp,cpp}` + `vql/parser.cpp` + `vql/ast.hpp` | `evaluateShapePredicate`/`matchDestructure` re-implemented over the kernel; `MissingPolicy` per callsite; `{}`/`{*}`/`{!*}` valence arms; `: {…}` nested-descent lowering; `linked` default; `PredicateTest::FunctionInvocation` arm + `?{…}` `return true` arm routed to `shapeMatches`; silently-ignored rule variants (`$shape`/`fn(.)`/pivot/virtual-dim) refuse or lower | The three readings converge; the dead arms and vacuous truths are wrong answers today |
| S3 | `vpl/view.hpp` + `vpl/vpl_engine.cpp` + `vpl/{token,lexer,parser}` | `⎕MATCH`/`⎕SHAPE` quads; **dependency: the Q10 cell-list `VplView`** (`vpl-lens-bridge.md:450`) — the mask yield has no representation without it; spec-string parse through the shared spec-grammar — owned by a new `zigzag/cell_shape_parse.{hpp,cpp}` row — the grammar is:

```
shape    := "{" field ("," field)* "}" | "{}" | "{*}" | "{!*}"
field    := dir dim [":" value | bindName nested? | "!"]
nested   := "{" field ("," field)* "}"
dir      := "+" | "-" | "~"
bindName := "$" ident | ident
value    := scalar | "$" ident | shape
```

`{ }` = Closed/valence-0, `{*}` = valence>=1, `{!*}` = forbidden-wildcard; a bare nested `{...}` re-matches the SAME cell (conjunction — a new semantics for the same syntax, since the spec'd `{$a,$b}` composition is rule-union not conjunction); `dir dim: {...}` descends to the neighbor. (a neutral parser producing `std::expected<ShapeExpr, …>` through an injected dim resolver — **`findDimension` (non-minting) at every match-time resolution** (`evaluateShapePredicate` resolves per-rule through `resolveDimension`→`findOrMintDimension`, `vql_engine.cpp:2385` → `multi_store.cpp:354` — a `d.dims`-appending write inside a read; an unresolvable dim fails Require and passes Forbid honestly; compile-time mint is the vicon `step` precedent, per-call mint is not); the VQL `ShapeLiteral` parser can't be a dependency — it's frontend-specific — so each frontend lowers its own syntax through the shared parse or its own) — `field := dir? dim (":" value | bindName nestedShape? | "!" )` where `bindName` is a `$`-sigil identifier (VQL/vicon) or a bare identifier (VPL/vprolog — the sigil is frontend-lexical, the *shape* is shared; the VQL surface needs the S2 grammar addition for `+d.dim: $name {…}` — no AST home today (`DimensionShapeRule.targetValue` is one `ValueExpr` — can't hold both a bind name AND a nested `ShapeLiteral`, `ast.hpp:139`); the lowering needs `ShapeField{mode=Bind, expected=nested}` — S2's grammar row must extend the rule shape (a `bindName` + `targetValue` pair, or `bindName` on the field carrying the nested `ShapeExpr`)) | `⌿` copies — a mask preserving identity needs the cell-list mode first |
| S4 | `vicon/compiler.{hpp,cpp}` + `vicon/driver.{hpp,cpp}` | `match`/`descendants` builtins on the V1 request/`V2` gate rows — `match` produces subject-then-binds through the site queue; `descendants` BFS over `neighbours` under `traversalBound` | The second-`Halt` protocol is the only host channel |
| S5 | `vortex_stdlib.{hpp,cpp}` | `lens_shape(C, Pattern)` arm — term→`ShapeExpr` lowering, `bind`/`sub` vars unified with `makeScalarCell(int64(…))` under mark/unify/release; dims via `findDimension` (no mint) | A builtin keeps cells out of `unify` (vlog.cpp:86-94's clone-tail hazard) |
| S6 | `vlace/compiler.{hpp,cpp}` | `shapeOf`/`matchShape` natives + `Shape`/`ShapeSpec`/`Constraint`/`Field`/`ShapeBase` ctor registration under P1's reserved-tag discipline; lazy `List (String,Cell)` result | The ADT surface lives only in `installXanadu` |
| S7 | `vortex_core.{hpp,cpp}` + the Q4 row | `lensSessions()` registry lands → each frontend's injected `SessionScan`/`SessionList` rebinds to the registry scan | VQL Q4 / vicon V4 / vprolog's register — shared, already proposed |
| S8 | `tests/` (new) | the §5 shape in each frontend's gate test (`lens_*_gate_test.cpp` pattern); kernel unit tests in `tests/lib/` | A test sits with its code — the acceptance artifact |

## 8. What this is not

- Not a traversal engine — ranks, transitive closure, and fixpoint stay frontend-side (D1).
- Not a write path — VQL's `$path{…}` projection and every mutating weave are untouched (the kernel
  never links; the Q7B funnel governs the other direction).
- Not a cell currency — `CellValue` gains no `CellRef` member; Int64 ref-tokens and handles stay
  per-frontend.
- Not a replacement for `cell_views` — the kernel is built on it; `linksAlong`/`rankContains`/
  `neighbours` remain the primitives.
- Not the session registry — `sessionFor` is injected until Q4 lands (D6).
- Not yet xuzz's picker surface — different frontend's doc.
