# vprolog Lens Bridge: Lenses as Builtin Predicates in a Backtracking Logic Engine (v1.21)

A design specification for how a **vprolog program** — not the `vprolog` REPL's `:lens` verbs —
opens, enumerates, mounts, writes back, and releases projection lenses. Design deliverable only: the
signatures are proposals; `openLensUri` and the `session_ops` gesture layer are the seam, not a new
layer. The sibling specs are [`vql-lens-bridge.md`](vql-lens-bridge.md) v6.9,
[`vlace-lens-bridge.md`](vlace-lens-bridge.md) v7.4, [`vpl-lens-bridge.md`](vpl-lens-bridge.md)
v1.16, and [`vicon-lens-bridge.md`](vicon-lens-bridge.md) v1.12 — the capability set is the same;
only the surface is re-thought for unification and backtracking.

vprolog is a **recursive resolver, not a VM program**: a query compiles to `makeTerm` goal cells
(`compiler.cpp:148-203`) and solves through `solveQueryHelper` — a recursive, mark/release-driven
continuation-passing resolver in `vortex_stdlib.cpp:289-1423`. The `VortexVM` exists on the
`Compiler` for stdlib bootstrap but **no goal runs through `vm_.run`** (`Compiler::solve` calls
`stdlib_.solveQuery`, `compiler.cpp:231-237`). There is no step-dispatch to extend (contrast VQL's
`evaluateStep`), no `IO a` to carry effects (contrast vlace), and no suspended-request channel
(contrast vicon's `lensGateOp`) — a builtin is a `goalFunctor == "name" && goalArgs.size() == N` arm
*inside the recursion*, and it talks to the host directly. The design center therefore shifts again:
not "where may a mutating step appear" (VQL), not "which operand carries a mutating quad" (VPL), not
"how a suspended generator talks to the Driver" (vicon), but **what a session means to a resolver
whose undo is `slots_.resize` — the mark stack above which session mints live, and the C++ session
truth the arena image cannot hold**.

______________________________________________________________________

## 1. Intent

A vprolog program should be able to write, in its own terms:

- **open a source** as projected root handles — `dir(Path)`, `sql(Dsn)`, `git(Path)`, `cfg(Path)`,
  `log(Path)`, `journal`/`journal(Unit)`, `meta(Label)`, `union([Spec, …])`, `kw(Spec)` — the
  `openLensUri` scheme table (`lens_uri.cpp:23-125`) underneath, spelled as *terms*, never as
  `scheme:`-prefixed strings;
- **enumerate** with the backtracking idiom it already has — `lens_children(C, K)` binding `K` to
  successive children on `;`, the way `member/2` binds list elements;
- **mount a delegate** under a projected cell and come **back** — "back" is a bound variable;
- **edit, commit, and revert** projected cells on write-capable lenses (`sql:`/`cfg:` today);
- **materialize** a deferred page, and let sessions go out of scope;
- **name a cell across queries** — the ExternalKey string, re-resolved by `lens_cell/2`.

Acceptance journey:
`lens_open(dir('/x')) → lens_children listing → lens_mount git under the repo cell → walk commits → lens_unmount → keep walking → lens_open(sql(…)) → lens_browse → lens_edit+lens_commit → lens_revert a refused edit → lens_next a page → program ends`.

## 2. Current surface (already in the build)

| surface                             | what it does                                                                                                                                                                                                                                                                                                                                                                                                            |
| ----------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `solveQueryHelper`                  | the resolver — `goalFunctor`+arity dispatch on `goals[0]`, recurse on `restGoals` (`vortex_stdlib.cpp:289-331`). Builtin arms run at `:334-1361` **before** the `candidatePreds` clause loop at `:1364-1422`: a user clause whose head names a builtin is silently shadowed                                                                                                                                             |
| the enumerate idiom                 | multi-solution builtins take `arena.mark()`, mint answer cells, `unify`, recurse, `release(mark)` — per alternative (`vortex_module/1` at `:388-407`, `cell_value`'s unbound arm at `:1020-1053`); loops honor `solutionsCount`/`cutToFrame` (`:1026`) and `consumeWork` (`:1022`)                                                                                                                                      |
| Int64 cell handles                  | `cell_value/2` (`:986-1054`), `cell_link/4` (`:1057-1134`), `cell_span/2` (`:1213-1262`), `bridge_edge` (`:1266-1361`) decode an arg `asInt64` else `static_cast<int64_t>` (`:987-995`) and yield `makeScalarCell(int64(cell))` (`:1029`,`:1063-1067`) — **refs already travel as Int64 scalars in the term space**; the gate test mints `makeScalarCell(int64(root))` to bind one (`lens_vprolog_gate_test.cpp:90-93`) |
| `=(X,X)` clause                     | `std:logic` fact (`vortex_stdlib.cpp:2900-2904`) — `unify` on two scalar-kind cells compares `ValueKind`+`valueBits` (`vlog.cpp:115-123`), so `=` on two handles IS cell identity                                                                                                                                                                                                                                       |
| `Vlog::bind`                        | a binding is a clone-rank splice — `link(endOfRank(t, clone, POS), clone, POS, …)` (`vlog.cpp:86-94`) — a **write to the rank's tail cell**                                                                                                                                                                                                                                                                             |
| `\\+`                               | negation runs a **nested `solveQueryHelper`** under its own `mark()`, released after the sub-solve (`vortex_stdlib.cpp:763-787`)                                                                                                                                                                                                                                                                                        |
| `freshenTerm`                       | clause refresh clones terms via `endOfRank(master, clone)` splices (`vortex_stdlib.cpp:102-160`) — authored-term writes, minted under the clause-try mark                                                                                                                                                                                                                                                               |
| `;` / `->` / `==` / `=..`           | lexed and parsed (`parser.cpp:43-58`) but have **no builtin arms** — `extractConjunction` splits only `,` (`parser.cpp:119-128`), so `(A ; B)` reaches the resolver as one `;(A,B)` goal and silently fails: a pre-existing wrong-answer channel                                                                                                                                                                        |
| `:lens` verbs                       | `LensRepl` on `compiler.core()` (`main.cpp:184`) — the projection lands in the **same arena the solver reads**; `session_ops` drives mount/unmount/next/commit (`lens_repl.cpp:177-209`); `adopt` destroys the prior session per open (`:105-113`)                                                                                                                                                                      |
| `lens_entry(Key, Text)`             | facts seeded into the clause database on every `:lens` open (`main.cpp:230-265`, `lens_repl.hpp:89-92` — "what a language without a cell term (vprolog) seeds as facts") — **snapshot text, not handles**: no link to live cells, stale after any refresh                                                                                                                                                               |
| `meta:`                             | the REPL holds a `MetaSource` (`main.cpp:187-201`) routing `meta:` through `LensRepl`'s owned-fold path (`lens_repl.cpp:118-137`) — the one sibling where `meta:` already works, at the REPL only                                                                                                                                                                                                                       |
| `--lens <uri>`                      | `runLensRaster` — own fold, exits before the REPL (`main.cpp:570-573`)                                                                                                                                                                                                                                                                                                                                                  |
| the fact builtins over a projection | proven: `cell_link`/`cell_value` answer a `FilesystemLens` projection's cells when session and solver share the arena (`lens_vprolog_gate_test.cpp:73-132`)                                                                                                                                                                                                                                                             |
| `solve`/`solveOnce`                 | `candidatePreds` = stdlib preds + `customPredicates()` (`vortex_stdlib.cpp:4230-4282`); `queryVars` collect for `sol.formatted` — `renderTerm` prints an Int64 scalar as digits (`:4101-4103`), so a bound handle prints `C = 2147483657`                                                                                                                                                                               |

vprolog has **no lens surface inside the language**: no open predicate, no session registry the
solver can see (the REPL's `LensRepl::session_` is `unique_ptr`-private, `lens_repl.hpp:110`), no
way for a goal to name a projected root — the gate test stitches `ProjectionSession` to the core by
hand (`lens_vprolog_gate_test.cpp:81-86`). The projected cells are already *queryable*; what is
missing is every verb that opens, gestures, or names them. The bridge introduces all three.

## 3. Design decisions

### D1 — The currency is the Int64 handle; a projected cell must never be a unification target

`cell_link`/`cell_value`/`cell_span` established it
(`vortex_stdlib.cpp:987-995`,`:1029`,`:1063- 1067`): a cell crosses the term boundary as
`makeScalarCell(static_cast<int64_t>(cellRef))` — the raw ref, `ephemeralBit | dense`
(`arena_manifold.hpp:452-460`; `denseOf` unmasks, `arena_manifold.cpp:267-269`, and `contains`
resolves through `slot`, `arena_manifold.hpp:464- 466`). `lens_children(R, K)` binds `K` to a fresh
Int64 scalar per alternative; `lens_edit(K, "text")` decodes `K` back `toInt64 → CellRef`.

The alternative — unify the variable with the projected cell itself — is not merely ugly, it is
**silently wrong**: `Vlog::bind(v, t)` writes `link(endOfRank(t, clone, POS), …)` — a link on the
*session-owned* clone-rank tail — which the `shadow()` funnel refuses (D8). And `expectWritten` is
`noexcept` — a refused write **logs on `zigzag.arena` and returns** (`arena_manifold.hpp:222- 230`),
so `bind` completes, `unify` reports `{}`, and the variable stays unbound: a wrong answer, not a
failure. The scalar handle keeps every unification over authored cells — the binding writes only the
var's own clone-rank tail and fresh minted cells. No builtin ever passes a projected cell to
`unify`; they mint scalars.

Consequences, absorbed:

- **`=` on two handles IS identity** — the `ValueKind`+`valueBits` arm (`vlog.cpp:115-123`). No
  `lens_eq` builtin needed; a *validated* identity is `lens_key(A) = lens_key(B)` or
  `lens_cell`-roundtrip (below).
- **Handles are untyped.** `lens_children(42, K)` — `toInt64` coerces strings too
  (`vortex_stdlib.cpp:38-49`) — silently resolves dense index 42: the `LiteralCellId` wrong-answer
  channel the VQL doc guards (`vql-lens-bridge.md` D2). Rule: **every cell arg is validated**
  `asInt64`-else-`static_cast<int64_t>(dId)` → `CellRef` → `contains()` → epoch (D4) →
  `sessionFor`/`ownsCell` — the established `cell_value`/`cell_link` decode
  (`vortex_stdlib.cpp:987-995`); NOT `toInt64` (it `stoll`s strings and returns 0 on failure,
  `:38-53` — `refOf(0)` can pass `contains()`) where the op is session-scoped; a failing arg makes
  the goal **fail**, never a wrong answer. `X is C + 1` produces a well-formed-but-wrong handle —
  arithmetic cannot tell a ref from an integer; documented, not fixed (vicon's `c + 1` wart, same
  class).
- **Scalar answers collide with ref-tokens.** `lens_dirty(S, 1)` producing `1` is indistinguishable
  from a handle to cell 1. Positional, declared per predicate — the program knows `lens_dirty`
  yields a count (vicon's identical residue, §6).
- **Display is a probe.** `renderTerm` prints the ref-int (`vortex_stdlib.cpp:4101-4103`), so a
  solution shows `C = 2147483657`. `lens_text/2`/`lens_key/2` are the readable verbs — required for
  §5 to print anything.

### D2 — Opens take spec terms, not URIs: `lens_open(Spec, Root)` is the generator

URIs are not xanadulogical, and a Prolog program's spec idiom is the **term**: the functor names the
lens, the argument carries its spec. `lens_open(Spec, Root)` accepts:

| spec term                               | maps to                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| --------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `'/x'` / `"/x"` (Atom or String)        | bare path → `openLensUri`'s auto-detect (`lens_uri.cpp:104-124`: directory → `RouterLens`, delegable file → its delegate, else `FilesystemLens`); **a `scheme:`-shaped operand fails** — spell the functor, not the scheme                                                                                                                                                                                                                                                              |
| `dir(Path)`                             | `dir:` → `RouterLens`                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `sql(Dsn)`                              | `sql:` — the DSN keeps `openSqlUri`'s own option grammar: a *trailing* `?write=1` is stripped (sqlite strips only the suffix, `sql_lens.cpp:1203-1206`; the `&` form is pg-conninfo-only, `:1220`) into the writable flag (`sql_lens.cpp:1184-1207`), so `sql('sqlite:///x.db?write=1')` opens writable (the `?write=1` strip runs only inside the `sql:<backend>:` branch, `sql_lens.cpp:1184-1207` — a bare path treats `?` as a literal filename); it is a spec-suffix, not a scheme |
| `git(Path)` / `cfg(Path)` / `log(Path)` | `git:` / `cfg:` / `log:`                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| `journal` / `journal(Unit)`             | `journal:` / `journal:<unit>` (`lens_uri.cpp:93-96`)                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| `meta(Label)`                           | the `MetaSource` fold path (D9) — **not** an `openLensUri` scheme (`kSchemes` refuses it, `lens_uri.cpp:42-45`)                                                                                                                                                                                                                                                                                                                                                                         |
| `union([Spec, …])`                      | one `openLensUri` child per list element — `lowerTerm` canonicalizes `[a,b]` to `.(a,.(b,[]))` (`compiler.cpp:74-76`), so the arm decodes the `.`-chain; the \`                                                                                                                                                                                                                                                                                                                         |
| `kw(Spec)`                              | any other functor → the `registerLensScheme` keyword registry (VQL's Q1 row, shared)                                                                                                                                                                                                                                                                                                                                                                                                    |

The spec term lowers like any argument — `dir('/x')` is `Compound{"dir", [String "/x"]}`
(`parser.cpp:264-274`) → `makeTerm("dir", …)` — and the builtin reads it back via
`functorOf`/`argumentsOf` + `core.arena().textOf` (`vortex_stdlib.cpp:4075-4088`; `textOf` is
`ArenaManifold`'s, not `VortexStdLib`'s). **Zero lexer/parser changes**: atoms, quoted atoms
(`lexer.cpp:385-398`), strings, and compounds already cover every form.

`lens_open` is the generator arm: it resolves the spec, dedupe/opens the session (D6), then
enumerates `session->roots()` under the mark/unify/recurse/release idiom (D3) — `Root` binds each
root's handle in turn on `;`. A refused open records the reason (D7) and fails. A zero-root source
still registers the session (dedupe/`lens_close`) and yields nothing — an empty source is not a
refusal (the siblings' rule).

### D3 — Gestures are builtin arms, not suspensions; effects fire host-side *before* the yield-mark

No opcodes, no gate: each `lens_*` name lands as a `goalFunctor` arm in `solveQueryHelper`, between
the existing builtins and the `candidatePreds` loop (`vortex_stdlib.cpp:1361`/:1364). Inside an arm:

- **Generators** (`lens_open`, `lens_children`, `lens_roots`) run the `cell_value` shape: snapshot
  the answer set at fire time, then per element
  `mark() → mint answer scalar → unify(arg, answer) → recurse(restGoals) → release(mark)` — honoring
  `consumeWork` (`:1022`), `solutionsCount` (`:1026`), and `cutToFrame`, so `lens_children(R, K), !`
  yields one child and stops (the `!` arm at `:377-385` propagates the frame; the generator loops
  break on it like every existing loop).
- **Single-answer gestures** (`lens_mount`, `lens_edit`, …) run the `session_ops` call **host-side
  first**, `Err` → `lens_error` record → fail; `Ok` →
  `mark() → mint/yield scalar → unify → recurse → release`. The ordering is load-bearing: session
  mints (the whole `refresh()` after a mount) must land **below** the arm's own yield-mark, or the
  arm's `release` would truncate the very cells it is about to name.
- **Const probes** (`lens_key`, `lens_text`, `lens_scalar`, `lens_dirty`, `lens_stub`, `lens_cell`,
  `lens_error`, `lens_current`) mint only their answer cells under the mark.

The mutating set is
`{lens_open, lens_mount, lens_unmount, lens_next, lens_browse, lens_edit, lens_commit, lens_revert, lens_refresh, lens_close}`;
the exempt probes are everything else. There is no positional multiplicity rule to write — VQL
needed one because its relation re-evaluates *silently*; Prolog re-fires a goal only by re-entering
it through the clause machinery (a re-dispatched arm, an alternation arm, a recursive call — a
written generator behind every fire, vicon's D4 argument verbatim). Per-op re-fire honesty — the
arena image reverts on backtrack, so "did the second fire corrupt" is a session-truth question (D5):

| op                       | second fire                                                                                                                                                                                |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `lens_open`              | dedupe by canonical identity — benign; on an epoch-invalidated session it re-projects (D4)                                                                                                 |
| `lens_mount`             | **idempotent** — `isMounted` short-circuits before the dirty-gate (`session_ops.hpp:62-66`)                                                                                                |
| `lens_unmount`           | no boundary → `Err` → fails — a re-entered goal simply prunes                                                                                                                              |
| `lens_edit`              | re-writes the pending text — idempotent in session truth *while the handle is live*; epoch-stale handles fail validation first (D4)                                                        |
| `lens_commit`            | applies the pending tail — **non-atomic**, a refusal returns with ≥0 applied (`projection_session.cpp:355-393`)                                                                            |
| `lens_revert`            | non-dirty is a true no-op (`session_ops.hpp:168-171`)                                                                                                                                      |
| `lens_browse`            | re-fires mint duplicate queries — `browse()` appends unconditionally (`sql_lens.cpp:441-463`); `isBrowsed` is a read probe (`sql_lens.hpp:124`); the Q6 `sessionBrowseAt` gate is required |
| `lens_next`              | materializes one more page — consumption, the honest cost of re-fire                                                                                                                       |
| `lens_stub`/`lens_dirty` | const reads — safe                                                                                                                                                                         |
| `lens_refresh`           | `refreshIfChanged` — mints, restates, tombstones; dirty-gate-exempt, not const                                                                                                             |

`\\+`-wrapped gestures are the sharp edge: `\\+ G` runs a **nested `solveQueryHelper`** under its
own `mark` and releases it on exit (`vortex_stdlib.cpp:763-787`). A gesture inside succeeds in
*session truth* (the mount happened, the dirty key recorded), fails the `\\+`, and its arena mints
are truncated by the `\\+`'s own `release` — a **half-leaked, half-reverted** gesture. Named
residue, documented not banned: `\\+` is the wrong tool around effects; the recovery idiom is clause
alternation (D7).

### D4 — Session mints live above the solver's marks: truncation, *aliasing*, and the epoch repair

Every ancestor choice point holds its `mark()` **across** the recursive call that eventually
dispatches a lens arm — the clause-try mark (`vortex_stdlib.cpp:1376`), every yielding builtin's
(`:1027`,`:1061`). A gesture inside mints session cells above all of them; when a later backtrack
exhausts any ancestor alternative, `release()` truncates `slots_` to `m.cellCount`
(`arena_manifold.cpp:1667-1670`). Two failure modes, vicon's D5 verbatim:

- **Dangle**: `keyToCell_`/`cellToKey_`/`roots_`/`presentKeys_`/`managedDims_` (DimRefs are
  `ephemeralBit|dense` — `ownsCell`'s negward-hop + `tombstone`'s `managedDims_.contains`
  wrong-positive on a re-minted dim,
  `projection_session.cpp:75-80`/`:418`/`:437`)/`mintOrder_`/`mintCount_` (rewind's foreign-mint
  delta + recomputeRoots)/`cps_` (checkpoints hold `Mark`s — a stale `Checkpoint` can *equal* the
  post-re-mint arena state and `rewind` releases a mark that isn't that boundary, `:485-496`);
  `pins_`/`refKeys_` survive (ExternalKey-keyed) and bound handles name dead `CellRef`s.
- **Alias**: freed dense indices are reused by subsequent mints; a stale handle *passes
  `contains()`* and silently names a live foreign cell — `ownsCell` wrong-positives, `lens_edit`
  writes one key's pending text to another cell. **`contains()` cannot be the repair.**

For vprolog this is **ordinary backtracking**, not a driver edge case:
`member(X, [a,b]), lens_open('/x', R)` mints the session under `member`'s mark, and `member`'s
*second* alternative truncates it — the open-containing path dies mid-query. The repair is the
shared vicon V4 row:

- `ArenaManifold` bumps `releaseEpoch()` on every `release()` truncation;
- session `keyToCell_`/`cellToKey_`/`roots_`/`presentKeys_`/`managedDims_` (DimRefs are
  `ephemeralBit|dense` — `ownsCell`'s negward-hop + `tombstone`'s `managedDims_.contains`
  wrong-positive on a re-minted dim,
  `projection_session.cpp:75-80`/`:418`/`:437`)/`mintOrder_`/`mintCount_` (rewind's foreign-mint
  delta + recomputeRoots)/`cps_` (checkpoints hold `Mark`s — a stale `Checkpoint` can *equal* the
  post-re-mint arena state and `rewind` releases a mark that isn't that boundary, `:485-496`);
  `pins_`/`refKeys_` survive (ExternalKey-keyed) and the registry entries stamp the epoch at record
  time; a stamped read at a different epoch **selectively-invalidates** — a per-holder
  `minFreedIndex` watermark (the same fix vicon took, `vicon-lens-bridge.md` D5: a global epoch
  bumps on every `release()` — including each yielding builtin's per-alternative release of its own
  answer mints — and wholesale-invalidates a session whose cells all survived; the honest rule is
  invalidate only entries whose dense index intersects a post-stamp freed range) — and stale
  `keyToCell_` entries are retained by design (`projection_session.cpp:283-294` — a delegate's map
  survives an unmount so held handles still resolve; probes read them until the next epoch boundary)
  — the session clears its maps and re-projects on the next `refresh()` (same key → deterministic
  re-mint); `cellToKey_` re-mint uses insert-or-assign (the `emplace` sites keep stale reverse
  bindings otherwise — `projection_session.cpp:121-122`,`:239-240`,`:397-399`);
- every handle decode validates `contains()` **and** epoch **and the issued-token ledger** (a bare
  Int64 carries no epoch — a stale handle presented after the session re-stamps passes
  `contains`+epoch+`sessionFor` and `keyFor` resolves a wrong-but-valid key; the session keeps
  `issuedTokens_` token→(key, issue-generation) — the same packed-generation ticket vicon's v1.21
  adopted (`issueGen << 32 | dense` — membership alone can't distinguish a stale token from a
  re-issued one); within a solve a stale handle can't be re-presented (every binding is trailed
  under the mark that kills the session cells — handle and binding die together), so the residual is
  only cross-solve literals (`C = 2147483657` re-typed, ints baked into program text, mesh-carried
  programs) — same fix as vicon's v1.13); a stale handle fails the goal;
- **dedupe-open refreshes invalidated sessions** (the vicon v1.11 repair) — a re-entered `lens_open`
  finds the dead registry entry, re-projects it, yields fresh handles;
- `lens_cell(K, C)` — `cellFor` re-resolution through the registry — is the **epoch-surviving
  handle**: the ExternalKey string is what crosses a truncation boundary.

One more mark pathology, vprolog's own: `solveQueryHelper` throws mid-recursion — the nesting-budget
`runtime_error` at `vortex_stdlib.cpp:298-299`, `executionLimited` work limits, the mesh
`onSolution` budget throw (`mesh_evaluator.cpp:948`) — propagating past every `release(mark)` in the
recursion. `outstandingMarks_` never returns to 0 (`arena_manifold.hpp:755`), so `compact()` is
refused forever (`arena_manifold.cpp:1698-1701`) and `ProjectionSession::rewind`'s foreign-mint
check fails permanently — the same stranded-mark class as vicon's V9 `start()`-drain. `solve()`
gains the drain: on unwind (exception or return), release outstanding marks in stack order —
`floors_` keeps only `{cells,links,content}` (3 of `Mark`'s 12 fields,
`arena_manifold.hpp:764-769`), so either a `discard()` loop heals `outstandingMarks_` (leaving
mints/trails un-truncated — different semantics) OR an arena-side full-`Mark` floor stack
(`drainMarks()`) truncates to pre-solve state, which the epoch invalidation absorbs.

### D5 — Session truth carries the payload; `commit()` must never read the arena image

Half of every gesture's arena write is trailed and backtracks; the lens-side truth does not.
`editContent` writes cell content on a cell **older than the enclosing marks** → trailed
(`arena_manifold.hpp:711-720`) → **restored away** on `release()` (`arena_manifold.cpp:1653- 1657`).
But `dirty_` is C++ state — it keeps the key. `commit()` today pushes `core_.arena().textOf(*cell)`
(`projection_session.cpp:377`) — the **reverted** bytes — to the source while reporting
`applied = N`. Worse for vprolog than for vicon: the trigger is ordinary backtracking —
`(lens_edit(C,"x") ; true), …, lens_commit(S,N)` needs no exotic re-drive, just a failure downstream
of the edit.

The repair is the shared vicon V4/D4 row: **`dirty_` becomes `unordered_map<ExternalKey,string>`
key→pending-text** — `commit()` applies `dirty_[key]`, never `arena().textOf`. The arena cells are a
disposable view; a post-release `lens_refresh` re-derives them (dirty-preserving `restateContent`
already skips `dirty_` keys, `projection_session.cpp:250`). A backtrack between `lens_edit` and
`lens_commit` then reverts only the arena image; `dirty_[key]` keeps the pending text and the commit
still writes it — session truth, by design.

### D6 — Sessions live on `VortexCore`; the cell arg names the session

The `Compiler` is process-lived (`main.cpp:559-561`) and the `LensRepl` shares its core
(`main.cpp:184`), so the **same `lensSessions()` row VQL's Q4 registers** — arena-level `weak_ptr`
membership + owning `vector<shared_ptr>` — serves both: program-opened sessions are owned by the
registry; the REPL's session joins **weak-only, re-registered per `:lens open`** (`adopt` destroys
the prior session — `lens_repl.cpp:105-113` — dropping uncommitted `dirty_` and ghosting its cells;
the seeded `lens_entry` facts from the previous open linger stale too — noted). `sessionFor(cell)`
scans the registry's `ownsCell` (node O(1), attr one negward hop, `projection_session.hpp:49-60`).

**Open dedupe by canonical source identity**: `openLensUri` does no normalization —
`lens_open(dir('/x'),_)`, `lens_open('/x',_)` (auto-detected directory → `RouterLens`), and
`:lens dir:/x/` open three sessions over one directory (`lens_uri.cpp:104-124`) unless the registry
records the canonical identity (filesystem paths canonicalized, scheme spelled once) at open on
**every** path — `lens_open`, `registerLensScheme` arrivals, and `LensRepl::adopt`. Two sessions
over one writable source split `dirty_` and cross-clobber on `commit()` (`keyToCell_` is
per-session, `projection_session.hpp:200-201`); a deduped open re-binds the live session's roots as
its yield.

**Session handles, not operands**: `lens_commit(C, N)`/`lens_revert(C)`/`lens_dirty(C, N)`/
`lens_refresh(C, R)` take a member cell purely to name the session — `sessionFor(ownsCell(C))` — and
the effect is session-wide (all dirty keys, the whole lens). `lens_current(R)` — fails when
`roots()` is empty (a zero-root session registers for dedupe but has no root to yield) binds the
most recent *live* session's first root — the `&lens` analog, weak-expiry + epoch validated —
failing when no session is open.

`lens_close` is deferred (U-V2, as the siblings).

`mesh_evaluator`'s vprolog frontend (`mesh_evaluator.cpp:928-963`) builds a fresh `Compiler` on the
worker's `context->core` — a remote program's `lens_open` projects into the *worker's* arena and
filesystem, and its session joins that core's registry. That is either the point (remote sources are
why the mesh exists) or an effect to gate — VPL's `withConsoleEffects(false)` precedent (`:916-917`)
maps to a `VortexStdLib` effects flag the mesh sets; named in §6.

### D7 — Refusal is Prolog failure; `lens_error/1` carries the reason; recovery is clause alternation

Every `session_ops` verb returns `std::expected`; the arm maps `Err` to *failure* — the goal yields
nothing and resolution backtracks — recording the reason in stdlib-held state first. `lens_mount`
inside `lens_children` enumeration simply prunes that child; `if lens_mount` does not exist — guards
are written as **clause alternation**:

```prolog
mount_or(C, Root) :- lens_mount(C, Root), !.
mount_or(_, Err)  :- lens_error(Err).
```

`lens_error(E)` produces the recorded reason and fails when clean — **cleared at every `lens_*` arm
entry **except `lens_error` itself** (a self-clearing `lens_error` always fails — the recovery idiom
depends on the slot surviving the probe's own entry); recorded on every failure path including
decode** (validation-miss and refusal both record — no stale reason leaks into the recovery idiom) —
a **produce-time read of stdlib-held C++ state**, not a trailed cell: the builtin's own `release`
would restore a stored cell to its *pre-refusal* value before the recovery clause reads it (vicon's
`&lenserr` argument, D2/D3 there — the constraint is identical and tighter here: the failure itself
is what runs the releases).

Two honest-failure notes specific to this surface:

- **`;`/`->` are not available for recovery** — they parse (`parser.cpp:43-48`) but no builtin arm
  implements them, and `extractConjunction` splits only `,` (`parser.cpp:119-128`): the term
  `;(A,B)` falls through to `candidatePreds` and silently fails — a wrong-answer channel the surface
  must not route recovery through. Same for `==`/`=..`/unimplemented operators: they fail as unknown
  predicates. Named, not fixed — the gap predates the bridge.
- **Builtin names are reserved.** The dispatch at `:334-1361` precedes `candidatePreds` (`:1364`),
  so a user clause `lens_mount(A, B).` compiles into `predicates_` but is **silently shadowed** —
  `compileClause` should refuse (or warn on) the enumerated 23-name reserved list (`lens_entry`
  exempt — the REPL's fact-seeding shares `compileClause`, `main.cpp:255-258`; the gate is the list,
  not a `starts_with("lens_")` prefix check) (register R7).

### D8 — The write funnel is inside `ArenaManifold::shadow()`; vprolog's own surface is audited

`core_.link` is a thin wrapper over `arena_.link` (`vortex_core.cpp:128-167`), and vprolog's writers
go straight to the arena — `compileClause`'s `arena().link`/`endOfRank` (`compiler.cpp:119-139`),
`addClause` (`vortex_stdlib.cpp:4163-4178`), `lowerTerm`'s `makeCell`/`makeScalarCell`/`makeTerm`
mints (`compiler.cpp:40-88`), `makeVar`'s `d.vars` rank splice (`vlog.cpp:23-31`), `freshenTerm`'s
`endOfRank(master, clone)` splices (`vortex_stdlib.cpp:117-121`,`:136-146`), `makeTerm`'s arg-dedup
clone splice (`vlog.cpp:38-47`), the `bind` splice itself (`vlog.cpp:86-94`), and every builtin's
`makeCell`/`makeScalarCell` answer mints. So the `sessionFor` guard — VQL's Q7B row — lives **inside
`ArenaManifold::link`/`linkChain`/`setValueBits`/`setContent`**, session-internal writes exempt via
the `ProjectionSession` inside-session-write flag.

The audit: every site above writes an authored tail (`endOfRank` of a predicate/`d.vars`/term cell)
or mints fresh — none targets a session-owned cell **by construction, except `bind`**: unifying a
variable with a projected cell writes its clone-rank tail. D1 removes the reach (no builtin ever
hands `unify` a session cell — handles are scalars), and the funnel converts a miss into a logged
refusal rather than a silent tombstone hazard — though `expectWritten`'s noexcept log-and-return
makes the failure quiet, which is exactly why the currency rule exists (D1). `findOrMintDimension`
appends the `d.dims` rank tail (`vortex_core.hpp:181`) — authored; lens arms resolve dims through
**`findDimension` only** (`vortex_core.hpp:183-185`, mints nothing) so a gesture can't mint above
live marks on a read path.

### D9 — `session_ops` carries the gestures; mount-scoping is the shared Q6 row; `meta:` needs an injected fold

The arms call `sessionMountAt`/`sessionUnmountAt`/`sessionNextPage`/`sessionCommitAll`/
`sessionEdit`/`sessionRevert`/`sessionDirty` (`session_ops.hpp:42-171`), inheriting the dirty-gate
(edit/commit/revert exempt), `canApply` inside `editContent`, and `commit()`→`expected<int>`
non-atomicity. Key-needing ops resolve `keyFor` and fail unkeyed — attr cells are session members
but carry no key (`ownsCell` climbs one negward hop for *session* discovery,
`projection_session.hpp:49-60`).

**Mount-scoped ops** (`lens_next`/`lens_stub`/`lens_browse`/`lens_unmount`) route cell → `keyFor` →
`mountBoundary` (`router_lens.hpp:66-71`) → `windowedFor`/`nextStubAt` (`:126-132`) through the
**proposed** shared resolvers — `sessionMountScopeAt`, `sessionBrowseAt` (gated on `isBrowsed` —
`browse` appends unconditionally, `sql_lens.cpp:441-463`), the `SqlLens` root-key accessor, and
`sessionUnmountAt`'s `mountBoundary→cellFor` pre-resolve (it returns `expected<void>`; the yield
needs the mount-point cell) — **VQL's Q6 register rows, not existing code**. Scope rule: on a
`RouterLens` session a cell outside every mount refuses (`mountBoundary`→`nullopt`); on a plain
`WindowedLens` session (`sql:`/`log:`/`journal:`/`git:` opened directly) the session is the scope —
`sessionNextPage`'s nullopt arm (`session_ops.hpp:128-134`).

**Union**: `UnionLens` is neither `RouterLens`/`WindowedLens`/`SqlLens` (`union_lens.hpp:31`) —
scoped ops fail on union cells; `lens_edit`/`lens_revert`/`lens_commit`/`lens_dirty` and the probes
work via `ownsCell`/`keyFor`/`canApply`/`commit()`. The duplicate-`ExternalKey` refusal
(first-success write vs last-emission display) is the proposed `UnionLens::claimedBy` probe — shared
row.

**Meta**: vprolog is one of three siblings wiring an app-level `MetaSource`
(`vquery`/`vpl`/`vprolog` `main.cpp` each construct one,
`vquery/main.cpp:408-420`/`vpl/main.cpp:449-461`; none injects it engine-side — the gap R8 owns) —
`main.cpp:187-201` resolves `meta:` through `LensRepl`'s fold path (`lens_repl.cpp:118-137`). But it
is **REPL-bound**: `LensRepl::metaFold_` is one slot for a one-session object (the member-ordering
comment at `lens_repl.hpp:102-110` exists for exactly this), and the closure's
`shared_ptr<Store> metaStore` is single-slot — safe only because `:lens open` replaces the session
in the same breath. Program-side `meta(Label)` needs the `MetaSource` injected into `VortexStdLib`
(the solver's own state) plus an **owned fold slot per session** — VPL D6's rule: `MetaLens` borrows
the fold, so each meta session's fold must outlive it (registry entry or session-owned); absent one,
`meta(…)` fails like any unimplemented spec — under `-e` batch and the mesh there is no MetaSource
to inject, and it fails the same way.

## 4. Proposed surface

### The open generator (session-mutating; spec term per D2)

| predicate               | yields                               | notes                                                                                                                                     |
| ----------------------- | ------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------- |
| `lens_open(Spec, Root)` | each projected root's handle, on `;` | `openLensUri`/`MetaSource` underneath; dedupe by canonical identity; refusal → `lens_error` + fail; zero-root → registers, yields nothing |

### Generators (multi-solution; mark/unify/recurse/release per alternative)

| predicate             | yields                                                                                             | notes                                                                                                              |
| --------------------- | -------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| `lens_roots(C, Root)` | each of the session's *current* roots' handles                                                     | `roots()` recomputes per `refresh()` (`projection_session.hpp:64-66`) — the post-refresh root list                 |
| `lens_children(C, K)` | each child's handle — `d.contains` head then the `d.seq` run (`filesystem_lens.cpp:164-191` shape) | dims resolved via `findDimension` (mints nothing, `vortex_core.hpp:183-185`); `cell_link` remains the generic form |

### Gestures (single-solution; `Err` → fail, reason on `lens_error`)

| predicate                                                                                                                                                                        | yields                                                                  | notes                                                                                                                                                 |
| -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| `lens_step(C, Dim, To)`                                                                                                                                                          | the posward-linked cell's handle                                        | **single-produce**; `Dim` an atom like `'d.contains'` via `findDimension` — no mint, so no gate-time dim cell above live marks; negward deferred (§6) |
| `lens_mount(C, Root)`                                                                                                                                                            | the delegate root's handle                                              | `sessionMountAt`; idempotent re-mount                                                                                                                 |
| `lens_unmount(C, MountPoint)` — arity-2 (the boundary is at-or-above `C` — a delegate-interior `C` mounts out to a *different* cell, so the mount-point handle must be bindable) | the mount-point cell's handle                                           | `mountBoundary→cellFor` resolved *before* `sessionUnmountAt` (`expected<void>` — Q6)                                                                  |
| `lens_next(C, Stub)`                                                                                                                                                             | the materialized stub's handle; fails at exhaustion                     | `sessionNextPage`, mount-scoped — plain `WindowedLens` sessions scope to themselves                                                                   |
| `lens_stub(C, Stub)`                                                                                                                                                             | the pending stub's handle; fails when none                              | `nextStubAt` — const, not dirty-gated                                                                                                                 |
| `lens_browse(C, Table, Root)`                                                                                                                                                    | the new query's root cell's handle                                      | `sessionBrowseAt` — sql-only, mount-scoped; needs the Q6 `isBrowsed` gate or re-fire duplicates                                                       |
| `lens_edit(C, Text)`                                                                                                                                                             | the cell's handle                                                       | `sessionEdit`; fails on ref-bound/unkeyed/non-writable/epoch-stale                                                                                    |
| `lens_commit(C, N)`                                                                                                                                                              | applied-count integer                                                   | `sessionCommitAll`; `C` names the session; **non-atomic** — a refusal can leave ≥0 applied (D5)                                                       |
| `lens_revert(C)`                                                                                                                                                                 | the cell's handle                                                       | `sessionRevert`; fails on source-vanished (`presentKeys_` check, `projection_session.hpp:124-129`)                                                    |
| `lens_refresh(C, Root)`                                                                                                                                                          | the session's first root post-refresh, or fails when `roots()` is empty | `refreshIfChanged` — mints/restates, dirty-gate-exempt; session handle                                                                                |
| `lens_close(C)`                                                                                                                                                                  | —                                                                       | releases the session — deferred (`U-V2`, as the siblings)                                                                                             |

### Pure probes (single-solution; fail honestly where the answer is absent)

| predicate                                                                                                             | yields                                                                                                                                                                                                                                                | notes                                                                                                                                                  |
| --------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `lens_key(C, K)`                                                                                                      | the `ExternalKey` atom                                                                                                                                                                                                                                | **fails** on attr/authored cells — honest absence, strictly better than a `""` sentinel that `=` can't distinguish (VQL's `cellKey`-vs-`""` collision) |
| `lens_cell(K, C)`                                                                                                     | the key's current handle; fails when unprojected                                                                                                                                                                                                      | `cellFor` via registry scan — the **epoch-surviving** handle: re-resolution across D4 truncation; the cross-query name                                 |
| `lens_text(C, T)`                                                                                                     | the cell's rendered content                                                                                                                                                                                                                           | the readable-print probe — a bound handle prints its ref-int (`renderTerm`, `vortex_stdlib.cpp:4101-4103`)                                             |
| `lens_scalar(C, V)`                                                                                                   | `Int64`/`Double`/`Bool` scalar                                                                                                                                                                                                                        | `valueBits` of lens-minted kinds — **kind preserved** via `makeScalarCell(bool)`/`(double)` (`arena_manifold.hpp:567-569`); fails on span/handle kinds |
| `lens_dirty(C, N)`                                                                                                    | `dirtyCount` integer                                                                                                                                                                                                                                  | `session.dirtyCount()` — const read; session handle                                                                                                    |
| `lens_current(R)` — fails when `roots()` is empty (a zero-root session registers for dedupe but has no root to yield) | the most recent *live* session's first root                                                                                                                                                                                                           | `&lens` analog — weak-expiry + `contains()` + epoch validated at produce time; fails when none is open                                                 |
| `lens_pending(K, Text)`                                                                                               | the pending-text for `K` — reads `dirty_[key]` (the session-truth probe; `lens_text` reads the arena image which shows pre-edit bytes after a backtracked edit) — resolves `K` through the registry scan, not `keyFor` (a key arg, not a cell handle) |                                                                                                                                                        |
| `lens_mounted(C)`                                                                                                     | succeeds iff `mountBoundary(keyFor(C))` is non-null (a delegate-interior `C` resolves through its mount — `router_lens.hpp:70-71`; discriminates gone-vs-refused in `unmount_if`)                                                                     |                                                                                                                                                        |
| `lens_error(E)`                                                                                                       | the last refusal's reason                                                                                                                                                                                                                             | produce-time read of stdlib-held state (D7); fails when clean                                                                                          |
| `A = B`                                                                                                               | —                                                                                                                                                                                                                                                     | on two handles IS identity (`vlog.cpp:115-123`); validated identity is `lens_key` equality or `lens_cell` roundtrip                                    |

**Reserved names**: `lens_open`, `lens_roots`, `lens_children`, `lens_current`, `lens_step`,
`lens_mount`, `lens_unmount`, `lens_next`, `lens_stub`, `lens_browse`, `lens_edit`, `lens_commit`,
`lens_revert`, `lens_refresh`, `lens_close`, `lens_key`, `lens_cell`, `lens_text`, `lens_scalar`,
`lens_dirty`, `lens_error`, `lens_mounted`, `lens_pending(K, Text)` (reads `dirty_[key]` — the
pending-text probe; `lens_text`/`lens_scalar` read the arena image which shows pre-edit bytes after
a backtracked edit) (the mount-presence probe — discriminates gone-vs-refused in `unmount_if`;
`isMounted`, `router_lens.hpp:78`) — builtin dispatch precedes `candidatePreds`
(`vortex_stdlib.cpp:334-1361` vs `:1364`), so a clause head with one of these functors is silently
shadowed; `compileClause` refuses them (R7).

## 5. Worked example

A self-contained program, `lensj.pl`, then the REPL session it drives:

```prolog
% the listing predicate — backtracks one child handle per solution;
% lens_text renders what the opaque handle can't
child_text(Dir, Child, Text) :-
    lens_open(dir(Dir), Root),
    lens_children(Root, Child),
    lens_text(Child, Text).

% find a cell by its ExternalKey — keys are the stable name
find_key(Dir, Key, Child) :-
    lens_open(dir(Dir), Root),
    lens_children(Root, Child),
    lens_key(Child, Key), !.        % lens_key fails on attr cells — the ! is honest

% mount a delegate, read commit texts, unmount, keep walking —
% "back" is a bound variable (no findall/3 exists — enumerate at the ?-
% prompt or accumulate through a recursive clause). unmount_if is the
% re-fire idiom: a bare trailing lens_unmount would FAIL on the second
% child (no mount left to drop), pruning every alternative after the
% first; the wrapper makes "gone" a success.
commit_text(Dir, Key, Text) :-
    lens_open(dir(Dir), Root),
    lens_children(Root, Repo),
    lens_key(Repo, Key), !,
    lens_mount(Repo, Git),
    lens_children(Git, K),
    lens_text(K, Text),
    unmount_if(Repo).

unmount_if(C) :- lens_unmount(C, _M), !.
unmount_if(C) :- \+ lens_mounted(C).

% the recovery idiom: clause alternation + lens_error, not ; or \+
mount_or(C, Root) :- lens_mount(C, Root), !.
mount_or(_, Err)  :- lens_error(Err).

% sql: schema-only until browsed; the :v: cell is the editable
fix_row(Db, Applied) :-
    lens_open(sql(Db), Conn),       % Db = 'sqlite:///x.db?write=1' for the writable half
    lens_browse(Conn, 't', Q),
    lens_step(Q, 'd.contains', Page),
    lens_step(Page, 'd.contains', Sight),
    lens_step(Sight, 'd.subject', Row),
    lens_step(Row, 'd.contains', Val),
    lens_edit(Val, "99"),
    lens_commit(Conn, Applied).
```

```text
?- child_text('/tmp/lensj', C, T).
C = 2147483657, T = a.txt ;
C = 2147483662, T = b.txt ;
C = 2147483667, T = sub ;
C = 2147483670, T = repo.

?- commit_text('/tmp/lensj', 'fs:52:794498', T).
T = HEAD ;
T = commit a1b2c3 ;
T = commit d4e5f6.

% cross-query resumption: the key is the durable name, the handle is per-arena
?- lens_open(dir('/tmp/lensj'), R), lens_children(R, C), lens_key(C, K), !.
R = 2147483650, C = 2147483670, K = fs:52:794498.

?- lens_cell('fs:52:794498', C), mount_or(C, G), lens_children(G, K), lens_text(K, T).
C = 2147483670, G = 2147483712, K = 2147483720, T = HEAD ; …

?- fix_row('sqlite:///tmp/lensj/data.db?write=1', N).
N = 1.

% the D5 hazard, demonstrated on the writable sql cell: the edit's arena
% text reverts on the failure-driven backtrack; the pending edit does not
?- lens_open(sql('sqlite:///tmp/lensj/data.db?write=1'), S),
|  lens_browse(S, 't', Q),
|  lens_step(Q, 'd.contains', P), lens_step(P, 'd.contains', Si),
|  lens_step(Si, 'd.subject', Row), lens_step(Row, 'd.contains', V),
|  lens_edit(V, '77'), fail.
false.
?- lens_cell('sql:…:t:1:v:c0', V), lens_dirty(V, D), lens_commit(V, N).
V = 2147483802, D = 1, N = 1.       % D5: commit pushes the pending text
```

`lens_open` at the head of a conjunction is the safe shape — its mints sit below every mark the rest
of the query takes. `member(X, [a,b]), lens_open(…)` is the fragile shape (D4): the session dies at
`member`'s second alternative, and the re-fired open re-projects it (dedupe-open refresh) with fresh
handles — correct, but the intermediate `C` handles from the dead epoch **fail** validation rather
than aliasing. `lens_children(R,C), !` is the once-idiom: the generator loop honors `cutToFrame`, so
the `!` stops the enumeration — and `!` after `lens_mount` pins the mount's single solution.
`lens_error` in the second `mount_or` clause reads the actual refusal —
`no router at the root -- mount needs a dir: view`, `session holds 2 pending edit(s)` — because it
is a live read, not a trailed cell. The `sql:` walk is the lens's real shape — query `+d.contains`
page `+d.contains` sight `+d.subject` row `+d.contains` `:v:` cell — and `lens_edit` refuses
anything but a materialized, browsed `:v:` cell (the `canApply` route inside `editContent`).

## 6. Open questions

1. **`lens_next` re-fire consumption** — a re-entered goal consumes another page; a per-goal
   activation token could make re-fires replay rather than consume (vicon's identical residue) —
   deferred.
1. **`lens_close`** — releases a session (`U-V2`, shared with the siblings). Registry erase is the
   easy half; what it does to a REPL-joined (weak) entry is the detail VPL names.
1. **`meta:` under `-e` and the mesh** — the `MetaSource` is REPL-built today (`main.cpp:187- 201`);
   batch and remote paths have nothing to inject. `meta(…)` fails cleanly there.
1. **`;`/`->`/`==`/`=..`** — parsed but arm-less (`parser.cpp:43-58`, `extractConjunction` `,`-only
   at `:119-128`): a pre-existing wrong-answer channel the bridge routes around, not one it closes.
1. **`\\+`-wrapped gestures** — session truth fires, arena mints revert under the `\\+`'s own
   release (`vortex_stdlib.cpp:763-787`): half-leaked, half-reverted. Documented; a lint or a
   solve-time effects flag is the honest fix.
1. **`lens_entry` facts** — the `:lens` seeding (`main.cpp:230-265`) stays (snapshot surface, dead
   text after any refresh); the live predicates supersede it for program access. Deprecate or keep —
   noted.
1. **Negward `lens_step`** and an `lens_boundary` mount-climb — deferred like vlace's `linked`
   negward half.
1. **A tagged cell currency** — the Int64 handle's collision space (`lens_children(42,K)`,
   `X is C + 1`, count-vs-ref answers) is inherent; a tagged wrapper scalar would close it at
   `asInt64`-consumer cost — deferred; `contains`+epoch+`sessionFor` validation is the mitigation
   that ships.
1. **Mesh/effect gating** — `mesh_evaluator` runs vprolog on the worker's `context->core`
   (`mesh_evaluator.cpp:928-963`); session-mutating builtins under a `withLensEffects(false)` flag
   (VPL's `withConsoleEffects` precedent, `:916-917`) or allowed as worker-local effects —
   undecided.

## 7. Proposal register

| R   | files                                                                                 | change                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        | why not caller/vprolog                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| --- | ------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| R1  | `vortex_stdlib.{hpp,cpp}`                                                             | the `lens_*` dispatch block in `solveQueryHelper` — `goalFunctor` arms between the existing builtins and the `candidatePreds` loop (`:1361`/`:1364`); spec-term decode via `functorOf`/`argumentsOf`/`textOf`; generator arms in the mark/unify/recurse/release idiom honoring `consumeWork`/`solutionsCount`/`cutToFrame`; gesture arms firing `session_ops` **before** the yield-mark; arg validation `asInt64`/`static_cast<int64_t>(dId)` → `contains` → epoch → `sessionFor`/`ownsCell`; `lastLensError_` + `lens_error`/`lens_current` state; `MetaSource` slot + per-session owned fold; `createPredicate`+`exportSymbol` for the names (the `cell_link` pattern, `:2986-3002`) — **plus the unwind drain** (the V9 analog): `solve()` releases outstanding marks in stack order on exception/return, healing `outstandingMarks_` (`arena_manifold.hpp:755`, `compact()` refusal `arena_manifold.cpp:1698-1701`)                                                                                       | The resolver seam lives only here — the VM is untouched.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| R2  | `vortex_core.{hpp,cpp}` + `lens_repl.{hpp,cpp}` + `union_lens.{hpp,cpp}`              | **the VQL Q4 row, shared**: `lensSessions()` registry (arena-level `weak_ptr` + owning `vector<shared_ptr>`); canonical source identity recorded at open on every path incl. `adopt`/`open`; `LensRepl::session_` `unique_ptr`→`shared_ptr`, weak-only join re-registered per `:lens open` (`adopt` destroys the prior session, `lens_repl.cpp:105-113`); `sessionFor` arena-scoped; `UnionLens::claimedBy`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   | One registry serves every frontend.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| R3  | `arena_manifold.{hpp,cpp}` + `projection_session.{hpp,cpp}`                           | **the vicon V4 row, shared — the D4+D5 repairs**: `releaseEpoch()` bumped per `release()` truncation (`arena_manifold.cpp:1608-1696`); session `keyToCell_`/`cellToKey_`/`roots_`/`presentKeys_`/`managedDims_` (DimRefs are \`ephemeralBit                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   | dense`—`ownsCell`'s negward-hop + `tombstone`'s `managedDims\_.contains`wrong-positive on a re-minted dim,`projection_session.cpp:75-80`/`:418`/`:437`)/`mintOrder\_`/`mintCount\_` (rewind's foreign-mint delta + recomputeRoots)/`cps\_`(checkpoints hold`Mark`s — a stale `Checkpoint`can *equal* the post-re-mint arena state and`rewind`releases a mark that isn't that boundary,`:485-496`); `pins\_`/`refKeys\_`survive (ExternalKey-keyed) stamped and selectively-invalidated on a`minFreedIndex`-intersect (the per-holder watermark — a wholesale bump on every `release()`kills a session whose cells survived;`issuedTokens\_`token→key ledger on every handle decode is mandatory, not optional);`cellToKey\_`insert-or-assign on re-mint;`dirty\_`→`unordered_map\<ExternalKey,string>`pending-text so`commit()`applies`dirty\_[key]`not`arena().textOf(\*cell)` (`projection_session.cpp:377\`) |
| R4  | `session_ops.{hpp,cpp}` + `sql_lens.{hpp,cpp}`                                        | **the VQL Q6 row, shared — still proposals**: `sessionBrowseAt` (gated on `isBrowsed`, `sql_lens.cpp:441-471`), `sessionMountScopeAt`, the `SqlLens` root-key accessor, `sessionUnmountAt` boundary pre-resolve; new `session_ops.cpp` keeps `SqlLens` out of the header                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      | The resolvers all frontends need.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| R5  | `arena_manifold.{hpp,cpp}` + `projection_session.{hpp,cpp}` + `vortex_core.{hpp,cpp}` | **the VQL Q7B row, shared**: the `sessionFor` funnel at the mutator level — `shadow()` is a `nullopt`-returning COW lookup, not a guard (a refused `to`/`displaced*` yields `nullopt` mid-`link()` and produces a silent one-sided half-link, `arena_manifold.cpp:1339-1374`); the guard **pre-flights the complete slot set before the first mutation** — each mutator evaluates `sessionFor` over `{from, to, displacedUs, displacedIt}`/the rank tail and refuses *before* any `setOneSide`/trail runs (a mid-sequence refusal is not an available semantic — `setOneSide` is `void` and called 4× per `link`, `:1363`-`:1374`; `insertIntoRank` already `std::ignore`s `link()`'s `ArenaResult`, `:1433-1435`, so its refusal is log-and-skip) — every existing-cell mutator (`link`/`linkChain`/`unlink`/`insertIntoRank`/`setValueBits`/`setContent`); session-internal writes exempt via the `ProjectionSession` flag; `masterOf` injected (the clone-DimRef is core state, `vortex_core.cpp:327-329`) | The funnel no writer can bypass — `bind`'s clone splice included.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| R6  | `lens_uri.{hpp,cpp}`                                                                  | **the VQL Q1 row, shared**: `registerLensScheme(keyword, factory)` — the closed `kSchemes` if-chain (`lens_uri.cpp:42-45`) becomes a registry so `kw(Spec)` functors resolve                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  | Custom lenses by functor name.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| R7  | `vprolog/compiler.{hpp,cpp}`                                                          | `compileClause` refuses (or warns on) clause heads naming the enumerated 23-name reserved list (`lens_entry` exempt — the REPL's fact-seeding shares `compileClause`, `main.cpp:255-258`; the gate is the list, not a `starts_with("lens_")` prefix check) — builtin dispatch precedes `candidatePreds` (`vortex_stdlib.cpp:334-1361` vs `:1364`), so they would silently shadow                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              | The shadow is silent today; the compiler is the only place that sees the head name.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| R8  | `apps/vprolog/main.cpp`                                                               | inject the `MetaSource` into `compiler.stdlib()` (the closure at `:187-201` moves or is shared); `lens` REPL verbs untouched; no new verbs                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    | The REPL owns the fold source today.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| R9  | `mesh_evaluator.{hpp,cpp}` (optional)                                                 | `withLensEffects(false)`-style gate on `VortexStdLib` for remote programs (`mesh_evaluator.cpp:928-963`), or document worker-local sessions as the intended semantics                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         | §6.9 — undecided.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| R10 | `tests/xuzz/vprolog_lens_test.cpp` (new)                                              | the §1 journey: `lens_open` → `lens_children` → `lens_mount` → back → `lens_edit`/`lens_commit`/`lens_revert`/`lens_next`/`lens_stub`/`lens_browse` with Int64 handles; refusal-as-failure + `lens_error` reads the real reason; clause-alternation recovery; `!` once-idiom; epoch invalidation (a backtrack truncating session mints does not alias; `lens_cell` re-resolves); dedupe-open refresh; `\\+`-wrapped gesture residue documented                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                | A test sits with its code — the acceptance artifact.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |

The session machinery (`session_ops`, `ownsCell`, `canApply`, `revertKey`,
`commit()`→`expected<int>`, `windowedFor`/`nextStubAt`, the `openLensUri` scheme guard) is the P5–P8
layer — vprolog's arms call it; the proposed rows are named as VQL's Q1/Q4/Q6/Q7B and vicon's V4/V9,
not existing code.

## 8. What this is not

- Not the `:lens` REPL verbs — those stay (and their session joins the registry weak-only).
- Not the `lens_entry` fact seeds — those stay too; snapshot text, superseded for live access.
- Not URI strings at the user surface — schemes are functors; `lens_open` keeps only the bare-path
  auto-detect and the `registerLensScheme` keyword route.
- Not a new opcode or a suspension protocol — the resolver's `goalFunctor` arms are the seam; the
  `VortexVM` is untouched.
- Not a session stack with mount/back — "back" is a bound variable.
- Not `;`/`->`-style recovery — clause alternation is the idiom; the operators have no arms.
- Not yet xuzz's picker surface — different frontend's doc.
