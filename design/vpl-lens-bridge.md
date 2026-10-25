# VPL Lens Bridge: Lenses as System Verbs in an Array Language (v1.0)

A design specification for how a **VPL program** — not the `vpl` REPL's `:lens` verbs — opens,
traverses, mounts, writes back, and releases projection lenses. Design deliverable only: the
signatures are proposals; `openLensUri` and the `session_ops` gesture layer are the seam, not a new
layer. The sibling spec is [`vql-lens-bridge.md`](vql-lens-bridge.md) v6.0 — the capability set is
the same; only the surface is re-thought for an applicative array language.

VPL is **applicative**: an expression is a verb applied to a `VplView` producing a `VplView`. There
is no call syntax — `f(x)` strands into a vector (`parser.cpp:223-234`) — and no backtracking —
`evaluateNode` runs each AST node exactly once (`vpl_engine.cpp:206-245`). The design center
therefore shifts from VQL's "where may a mutating step appear" to two array-native questions: the
**multiplicity** of a mutating verb applied to an N-cell view, and which **operand positions** may
carry a session-mutating construct.

______________________________________________________________________

## 1. Intent

A VPL program should be able to write, in its own terms:

- **open a source** as a projected root — `dir:`/bare path, `git:`, `sql:`, `cfg:`, `log:`,
  `journal:`, `union:`, `meta:` — the `openLensUri` scheme table (`lens_uri.cpp:23-125`);
- **traverse** with the view algebra it already uses — `lens ⍴ d.contains d.seq`, `V[i]`;
- **mount a delegate** under a projected cell and come **back** — "back" is a bound view;
- **edit, commit, revert** on write-capable lenses, and **materialize** deferred pages;
- **connect stores/slices** — the coordinator's `d.stores` rank as a connection rank — so a
  multi-store or multi-slice program is an *array of sources*, not a flag.

Acceptance journey (the ux-validation shape):
`⎕LENS 'dir:/x' → lens ⍴ d.contains d.seq → ⎕MOUNT repo-cell → walk commits → ⎕UNMOUNT → keep walking → ⎕LENS 'sql:/x.db' → 't' ⎕BROWSE lens → '99' ⎕EDIT row → ⎕COMMIT lens → ⎕REVERT a refused edit → ⎕LNEXT → ⎕LCLOSE`.

## 2. Current surface (already in the build)

| surface                                                 | what it does                                                                                                                                                                                                   |
| ------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `:lens <uri>` / mount / unmount / next / commit / close | REPL verbs — `LensRepl` on the engine's own core (`main.cpp:464`); `bindRank` already falls back to `lensRepl.root()` when a dim is not in the open store (`main.cpp:609-631`)                                 |
| `--lens <uri>`                                          | programmatic raster — `runLensRaster`; `meta:` goes through `lensRepl` with the coordinator's `MetaSource` (`main.cpp:449-476`)                                                                                |
| `:open <store>` / `:rank <dim>`                         | store connection + rank binding → `data` (`main.cpp:693-718`, `:603-658`); `proxyFor` mints the foreign cells and `bindDimension` binds the axis (`:435-442`, `:643-653`)                                      |
| `data`, `H`, `d.*`                                      | `env_` variables, the home cell, dimension nouns (`evalIdentifier`, `vpl_engine.cpp:364-378`)                                                                                                                  |
| `⎕` / `⎕READ` / `⎕SPLIT`                                | the quad system-function family — the only host-effect surface (`token.hpp:82-84`; `evalQuad`, `vpl_engine.cpp:692-748`)                                                                                       |
| `view ⍴ dims`, `view[i]`, `⊃`, `≡`                      | view construction over a dim list (`vpl_engine.cpp:1013-1016`), indexing — whose scalar yield keeps `origin()=cell` (`:399-412`), clone-master disclose (`:883-898`), and origin-identity match (`:1176-1183`) |

VPL has **no lens construct reachable from a program**, **no session registry the engine can see** —
the REPL's session is `LensRepl::session_` (`lens_repl.hpp:110`); the multi-session precedent is
vlace's `Compiler::sessions_` `vector<shared_ptr>` + `sessionFor`/`ownsCell` scan
(`compiler.hpp:227`, `compiler.cpp:301-307`), and **no `sessionFor` guard** anywhere in
`vpl_engine.cpp`'s write paths.

## 3. Design decisions

### D1 — the `⎕` system verb is the surface; one `⎕LENS` replaces VQL's seven generators

APL routes host interaction through `⎕`-functions (`⎕READ`/`⎕SPLIT` is VPL's own precedent), and J
routes it through `w.` foreign words — both idioms land on the same tokens here. A new `Quad*` token
family carries the whole surface; `⎕` scanning is extended from the three literals to `⎕`+NAME
(`lexer.cpp:405-422`) and the J-spellings (`lens.`, `mount.`, `edit.`, `commit.`…) join
`scanIdentifierOrKeyword`'s table (`lexer.cpp:271-296`). `⎕LENS` takes the **URI string as its right
operand** — `⎕LENS 'dir:/x'`, `⎕LENS 'sql:/x.db'`, `⎕LENS 'union:dir:/a|git:/b'` — so VQL's
`fsLens`/`gitLens`/`sqlLens`/`logLens`/`journalLens`/`cfgLens`/`unionLens` spellings and the
`union:` spec fns (`dir`/`sql`/`git`/…) collapse into the one verb plus the scheme grammar
`openLensUri` already parses; custom lens keywords arrive through VQL Q1's `registerLensScheme`.
`⎕LENS` is **session-mutating**; **open dedupe**: a URI already open in the registry re-binds the
live session's roots — two sessions over one writable source would split `dirty_` and cross-clobber
on `commit()` (`keyToCell_` is per-session, `projection_session.hpp:200-201`); a *different* URI
mints its own projection into `core`'s arena, where `bindRank` and `directEngine` already look
(`main.cpp:462-464`).

Two dyadic gestures need a left operand — `'99' ⎕EDIT cellview`, `'t' ⎕BROWSE cellview` — so
`QuadExpr` gains an optional left (the parser's quad arm *drops* `leftNoun` today,
`parser.cpp:164-168`). Dyadic lens verbs zip left-right to `min(l,r)` like every other dyadic verb
(`vpl_engine.cpp:1264-1266`) — `'a' 'b' 'c' ⎕EDIT cells` edits each cell with its text.

### D2 — sessions live on `VortexCore`; `lens` is the `$lens` equivalent

`vpl` builds one long-lived `VPLEngine` for the process (`main.cpp:399`), so sessions survive across
lines either way — but the `:lens` REPL session lives in `LensRepl` while `⎕LENS`-opened sessions
would live in the engine: the registry belongs on the **shared core** so both coexist and
`sessionFor` resolves either (the same `VortexCore` row VQL's Q4 registers: arena-level `weak_ptr`
membership + owning `vector<shared_ptr>`; `LensRepl::session_` migrates `unique_ptr`→`shared_ptr`).
A `⎕LENS` open binds **`lens`** in `env_` to `VplView{roots().front(), {}}` — a valence-0 cell view
(the `bindRank` lens fallback's `VplView(first, {d.seq})` precedent at `main.cpp:613-631`, minus the
axis) — and re-binds per evaluated line from the registry's most recent *live* open (expired
`weak_ptr`s skipped; a `:lens close` leaves no stale `lens`). It also **yields a rank of ExternalKey
strings** — minted authored cells, not session-owned — because the key, not the ephemeral `CellRef`,
is the stable cross-line handle (VQL D2's `fs:52:794498` argument holds verbatim; a `CellRef` in a
bound view is meaningless after the arena moves). `⎕LKEY`/`⎕LCELL` convert between the two
currencies.

### D3 — multiplicity lives inside the verb; session-mutating quads refuse derived-verb position

A quad applied to an N-cell view `collectCells` **once** (`view.hpp:289-332`) and the verb decides:
**per-cell** ops (`⎕MOUNT`, `⎕EDIT`, `⎕REVERT`, `⎕LNEXT`) run per input cell with `Err`→drop (a
per-cell refusal removes the cell from the result rank, not the whole evaluation);
**session-scoped** ops dedupe — `⎕COMMIT`/`⎕DIRTY`/`⎕LSTUB` resolve each input cell to its session
via `sessionFor` and run **once per distinct session**; `⎕UNMOUNT` once per distinct `mountBoundary`
(`router_lens.hpp:70-71`); `⎕LNEXT` is deliberately **not** deduped — N cells in one mount
materialize N pages (`session_ops.hpp:106-146`). Arguments resolve through
`keyFor`/registry-`cellFor`: an operand may carry **either** projected cells (a view whose origins
are cells — `S[2]` yields a scalar with `origin()=cell`, `vpl_engine.cpp:404-412`) **or** key
strings (a rank of ExternalKey text — the `⎕LENS` yield); mixing is a `LengthError`-style refusal.

Single-eval rule, restated for mapping: **a session-mutating quad may not appear as the operand of
an adverb or conjunction** — the derived-verb arms re-invoke the verb *token* per cell
(`evalMonadic` `¨`/Reduce/Scan arms, `vpl_engine.cpp:477-535`; `isVerb` lists the quads,
`token.hpp:271-273`, so `⎕MOUNT¨` parses and must refuse at eval; `⎕MOUNT view` already maps — the
adverb is redundant, not lost). Everything else is free: `env_` replay is a pure lookup
(`vpl_engine.cpp:373-377`), assignment stores a view not a thunk (`:380-384`), and each AST node
evaluates once — there is no `where`/join/`for` multiplicity to ban. The mutating set is
`{⎕LENS, ⎕STORE, ⎕MOUNT, ⎕UNMOUNT, ⎕LNEXT, ⎕COMMIT, ⎕EDIT, ⎕REVERT, ⎕BROWSE, ⎕LCLOSE}`; the exempt
const probes are `{⎕LSTUB, ⎕DIRTY, ⎕LKEY, ⎕LCELL, ⎕LSCAL, ≡}`. Under `--engine vortex` the new kinds
hit `compileQuad` (`vpl/compiler.cpp:1085-1104`), which knows only the three existing quads — they
**refuse at compile time** (VQL Q7B's boundary: the VM runs raw `arena().link` with no session
funnel).

`⎕LENS` over a **rank of URI strings** — `⎕LENS ('dir:/a' 'sql:/b.db' 'git:/c')` — opens N sessions,
each deduped; the yield is a rank of key strings. This is the array-native multi-open: an array of
sources ↦ an array of sessions. `⎕STORE` does the same for store paths (D8).

### D4 — reads observe the arena at walk time

`collectCells`/`shape` walk links lazily at use (`view.hpp:201-332`), so a bound view sees whatever
`refresh()` last stated — gestures `refresh()` into the same arena. `⎕LSCAL` reads the lens-minted
kinds `Int64`/`Double`/`Bool` (`mintNodeCell`, `projection_session.cpp:95-116`) into a VPL scalar
payload; `view[i]`'s existing scalar extraction (`vpl_engine.cpp:399-412`) is the same read with
`textOf` for span cells. Handle kinds (`OpHandle`/`ExternRef`/`ForeignOpHandle`) yield a refusal,
matching `cellScalar`'s empty-by-design.

### D5 — `session_ops` carries the gesture protocol

`sessionMountAt`/`sessionUnmountAt`/`sessionNextPage`/`sessionCommitAll`/`sessionEdit`/
`sessionRevert`/`sessionDirty` (`session_ops.hpp:42-171`) are the implementation — the quads call
them, inheriting the dirty-gate (edit/commit/revert exempt), `canApply` inside `editContent`, and
`commit()`→`expected<int>`. `⎕BROWSE` needs the shared `sessionBrowseAt(session, key, table)` —
mount-scoped resolve → `dynamic_cast<SqlLens*>` → `isBrowsed` → `browse` (`sql_lens.cpp:442`) →
`refresh()` — and `sessionMountScopeAt` for the cell→`keyFor`→`mountBoundary`→`windowedFor` preamble
shared by next/stub/browse/unmount (triplicated today; vlace's is `compiler.cpp: 1441-1470`). Both
are the same `session_ops.cpp` rows VQL's Q6 registers — one row serves both frontends. `browse`
returns `expected<void>`, so the new query's root comes from a `SqlLens` accessor or a
browse-returns-key change (registered there).

### D6 — `union:` key-level only; `meta:` supported via the coordinator

`UnionLens` is neither `RouterLens`/`WindowedLens`/`SqlLens` (`union_lens.hpp:31`), so the scoped
verbs (`⎕MOUNT`/`⎕UNMOUNT`/`⎕LNEXT`/`⎕LSTUB`/`⎕BROWSE`) `Err` on union cells; only
`⎕EDIT`/`⎕REVERT`/`⎕COMMIT`/`⎕DIRTY` and the key probes work — with the inherited
**duplicate-ExternalKey refusal**: a key two children emit refuses ambiguous (`applyContentChange`
first-success vs `restateContent` last-emission, `union_lens.cpp:78-92`). Unlike VQL, `vpl` already
builds a `MetaSource` fold from the coordinator (`main.cpp:449-461`), so `⎕LENS 'meta:x'` is
*supportable* here — it needs `VPLEngine` to hold the coordinator's `MetaSource` (injected from
`main.cpp`, same seam `⎕STORE` uses); absent one it refuses like any unknown scheme.

### D7 — every VPL write site on an existing cell checks `sessionFor`

VPL's write surface is *linking existing cells onto dims*: `cells ⍴ d.1` (`linkChain(cells, d)`,
`vpl_engine.cpp:1020-1028`), `n↑V`/`n↓V` (`:1125`, `:1148`), the mixed-subview vector link (`:352`)
— each overwrites the cell's posward slot on that dim, and on a session-managed dim that is the
tombstone hazard VQL D7 names. Fresh-mint paths (`catenate` :769+, `mintRank` :1393+, `evalIndexing`
copies :437-461, `∘.`/`⌸` mints) are safe by construction; `⍫`'s `rebuildManifoldFromIndex`
(`:1190-1193`) writes the *store* fold, not the arena. Rule — same as VQL D7: **a checked-link
funnel inspects every existing `CellRef` about to be linked or spliced — including displaced slots
(`link()` clears the reciprocal edge) — and refuses** (VPL's error channel is the exception →
`Error:` at `main.cpp:871-874`, not per-cell drop: a `⍴`/`↑` over a mixed rank aborts loudly rather
than relinking a subset). The correct content write is `⎕EDIT`; projected structure is the session's
to maintain.

### D8 — multi-store is a rank of connections; multi-slice is a dim walk

`⎕STORE 'path'` runs `coordinator.loadAndAddStore` (`multi_store.hpp:88-91`): a representative cell
on the `d.stores` rank off coordinator home, `d.clone`-ed to the slice home master, `d.name`/
`d.role` attrs (`multi_store.cpp:57-93`). Then the connection rank *is* an array: `H ⍴ d.stores`
yields the connected stores, `stores[i]` picks one, `⊃`/`-d.clone` discloses to the slice home, and
`home ⍴ d.result` views the slice's ranks — multi-slice browsing in three tokens. A store's cells
are foreign (`resolveForeign`, `main.cpp:142-154` provenance) — `⎕STORE`/`bindRank`'s `proxyFor` +
`bindDimension` prelude (`main.cpp:435-442`) is what mints the proxies and binds the axes, and the
`sessionFor` guard treats proxies of session-unowned space as ordinary cells. `union:` (D6) is the
*content* union across sources; `d.stores` is the *store* union — both surfaces of "multi" land in
the same view algebra.

## 4. Proposed surface

### Generator verbs (session-mutating; sole-operand position)

| verb                | operand                                                         | yields                        | notes                                                                                                                                   |
| ------------------- | --------------------------------------------------------------- | ----------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| `⎕LENS` (`lens.`)   | URI string, or rank of URI strings                              | rank of ExternalKey strings   | `openLensUri` per cell, deduped by URI; binds `lens` to `VplView{roots().front(), {}}`; `union:`/bare path/`meta:` per the scheme table |
| `⎕STORE` (`store.`) | path string, or rank of paths — dyadic: `'label' ⎕STORE 'path'` | rank-0 view of the store cell | `coordinator.loadAndAddStore` + `proxyFor`/`bindDimension`; needs the coordinator injected into the engine (Q6)                         |

### Gesture verbs (`Err`→drop per cell; session-scoped ops dedupe per D3)

| verb          | operand                 | yields                    | notes                                                                                        |
| ------------- | ----------------------- | ------------------------- | -------------------------------------------------------------------------------------------- |
| `⎕MOUNT`      | cell view or key        | delegate roots (key rank) | `sessionMountAt`; per-cell                                                                   |
| `⎕UNMOUNT`    | cell view or key        | the mount-point keys      | `mountBoundary` resolved *before* `sessionUnmountAt`; deduped per mount                      |
| `⎕LNEXT`      | cell view or key        | materialized stub keys    | `sessionNextPage` — mount-scoped on Router, session-level on `WindowedLens`; **not** deduped |
| `⎕LSTUB`      | cell view or key        | pending stub keys         | `nextStubAt` — const, not dirty-gated; deduped                                               |
| `⎕COMMIT`     | any cell of the session | applied count (scalar)    | `sessionCommitAll` → `commit()` count; once per distinct session                             |
| `text ⎕EDIT`  | cell view or key        | the edited keys           | `sessionEdit`; dyadic — texts zip cells `min(l,r)`; per-cell                                 |
| `⎕REVERT`     | cell view or key        | the reverted keys         | `sessionRevert` — `Err` on source-vanished                                                   |
| `'t' ⎕BROWSE` | cell view or key        | the new query's root key  | `sessionBrowseAt` — sql-only, mount-scoped                                                   |
| `⎕DIRTY`      | any cell of the session | `dirtyCount` (scalar)     | `session.dirtyCount()`                                                                       |
| `⎕LCLOSE`     | any cell of the session | empty                     | releases the session — deferred (`U-V2`, as VQL)                                             |

### Pure probes

| verb                 | operand          | returns                             | notes                                                                                                                          |
| -------------------- | ---------------- | ----------------------------------- | ------------------------------------------------------------------------------------------------------------------------------ |
| `⎕LKEY` (`key.`)     | cell view        | ExternalKey strings (rank)          | `keyFor` per cell; authored/attr cells yield `""` — the `cellKey`-vs-`""` collision VQL notes applies                          |
| `⎕LCELL` (`cell.`)   | key string(s)    | rank-0 cell views / rank of origins | `cellFor` via registry scan — the stable handle; absent key → empty                                                            |
| `⎕LSCAL` (`scalar.`) | cell view        | Int64/Double/Bool scalars           | `asInt64`/`asDouble`/`asBool` on `valueBits`; non-minted kinds refuse                                                          |
| `≡` / `-:`           | two rank-0 views | `Bool`                              | **already** origin-identity (`vpl_engine.cpp:1176-1183`) — `cellEq` needs no new verb; per-cell identity is `⎕LKEY` comparison |

## 5. Worked example

```vpl
      ⍝ open a dir lens; yields the root's key; binds lens ← VplView{root,{}}
      ⎕LENS 'dir:/tmp/lensj'
fs:52:794498

      ⍝ the listing: firstborn down d.contains, siblings down d.seq —
      ⍝ one 2-valence view, same cells bindRank walks
      S ← lens ⍴ d.contains d.seq
      S[2]                              ⍝ scalar view, origin() = repo cell
/repo

      ⍝ mount the git delegate under it (view or 'fs:…' key both work)
      ⎕MOUNT S[2]
git:…:HEAD
      (lens ⍴ d.contains d.seq)[2] ⍴ d.contains d.seq   ⍝ the commit walk

      ⍝ back out — the mount cell is the boundary
      ⎕UNMOUNT S[2]

      ⍝ a sql lens: schema-only until browsed
      ⎕LENS 'sql:/tmp/lensj/data.db'
      't' ⎕BROWSE lens
      R ← lens ⍴ d.contains d.seq
      '99' ⎕EDIT R[0]
      ⎕DIRTY lens
1
      ⎕COMMIT lens
1
      'x' ⎕EDIT R[0]
      ⎕REVERT R[0]      ⍝ drop the pending edit; restate reads the source back

      ⍝ pages, keys, identity — statements separate by newline or ;
      ⎕LSTUB lens ; ⎕LNEXT lens
      ⎕LKEY R[0]
sql:…:t:1
      (⎕LCELL 'sql:…:t:1') ≡ R[0]
1

      ⍝ multi-source: one union session, or N sessions off a rank
      ⎕LENS 'union:dir:/a|cfg:/b'
      ⎕LENS ('dir:/a' 'sql:/b.db')      ⍝ two opens, deduped by URI

      ⍝ multi-store: the d.stores rank as a connection array
      'lib' ⎕STORE '/path/to/slice'
      stores ← H ⍴ d.stores
      home ← ⊃ stores[0]                ⍝ disclose → clone master → slice home
      home ⍴ d.result                   ⍝ the slice's result rank, as a view

      ⎕LCLOSE lens
```

A verb takes the view as its operand — `⎕MOUNT S[2]` mounts under that cell; `⎕MOUNT S` mounts under
*every* cell `S` reaches (per-cell yield; refused cells drop). `⎕COMMIT lens` over any cells of one
session commits **once** (D3's dedupe), yielding `[applied]`, not `[applied,0,0…]`. An empty operand
is a no-op: `⎕COMMIT (empty)` runs no gesture.

## 6. Open questions

1. **Forest yield** — `union:`/rank-opens bind `lens` to `roots().front()` (the `LensRepl::root()`
   precedent, `lens_repl.hpp:82-86`); a view of *all* roots needs either a session-declared roots
   dim or a `VplView` cell-list mode — deferred (the key rank already carries every root).
1. **`⎕LERR`** — whole-verb refusals throw (`Error:`); per-cell refusals drop. A `⎕LERR` probe of
   the last refusal's reason mirrors VQL's open question 1, same coordinator-lived state.
1. **`meta:` under `⎕LENS`** needs the `MetaSource` injected — with it, it works; without, the
   scheme guard refuses (Q6 names the injection).
1. **`≡` on rank-N views** compares only origins; per-cell identity is `⎕LKEY` + `=` on strings —
   acceptable, or `⎕LEQ` per-cell (deferred).
1. **J-spelling collision** — `lens.`/`store.`/`commit.` are free today; the table must be checked
   against `scanIdentifierOrKeyword` before naming lands.
1. **`⎕LCLOSE`** — releases a session (`U-V2`, shared with VQL).

## 7. Proposal register

| Q   | files                                           | change                                                                                                                                                                                                                                                                                                                             | why not caller/vpl                                                  |
| --- | ----------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------- |
| Q1  | `vpl/token.hpp` + `vpl/lexer.{hpp,cpp}`         | `QuadLens`/`QuadStore`/`QuadMount`/`QuadUnmount`/`QuadNext`/`QuadStub`/`QuadCommit`/`QuadEdit`/`QuadRevert`/`QuadBrowse`/`QuadDirty`/`QuadClose`/`QuadKey`/`QuadCell`/`QuadScalar` TokenKinds; `⎕`+NAME generic scan (`lexer.cpp:405-422` takes literals); J-spellings in `scanIdentifierOrKeyword` (`:271-296`); `isVerb` entries | The quad surface is the only host-effect idiom.                     |
| Q2  | `vpl/ast.hpp` + `vpl/parser.cpp`                | `QuadExpr` gains an optional left operand; the quad arm keeps `leftNoun` (`parser.cpp:164-168` drops it) so `'t' ⎕BROWSE c` parses dyadic                                                                                                                                                                                          | The two payload gestures have nowhere else to take an arg.          |
| Q3  | `vpl/vpl_engine.{hpp,cpp}`                      | `evalQuad` lens dispatch — operand cells→`keyFor`/strings→registry-`cellFor`; `session_ops` calls; per-cell drop vs per-session dedupe (D3); `lens` auto-bind; open dedupe by URI; derived-verb refusal for mutating quads; `sessionFor` accessor over the core registry                                                           | The verb seam lives only here.                                      |
| Q4  | `vortex_core.{hpp,cpp}` + `lens_repl.{hpp,cpp}` | **the VQL Q4 row, shared**: `lensSessions()` registry on `VortexCore` (arena-level `weak_ptr` membership + owning `vector<shared_ptr>`); `LensRepl::session_` `unique_ptr`→`shared_ptr` + join; `sessionFor` arena-scoped                                                                                                          | One registry serves vquery, vlace and vpl.                          |
| Q5  | `session_ops.{hpp,cpp}` + `sql_lens.{hpp,cpp}`  | **the VQL Q6 row, shared**: `sessionBrowseAt`, `sessionMountScopeAt`, `SqlLens` root-key accessor, `session_ops.cpp` keeps `SqlLens` out of the header                                                                                                                                                                             | The resolvers both frontends need.                                  |
| Q6  | `apps/vpl/main.cpp` + `vpl/vpl_engine.hpp`      | inject `MultiStoreCoordinator *` + `MetaSource` into `VPLEngine` (`⎕STORE`, `⎕LENS 'meta:'`); re-bind `lens` per REPL line from the most-recent live session; `--lens` + `-e` composition                                                                                                                                          | The engine holds neither the coordinator nor the fold source today. |
| Q7  | `vpl_engine.cpp`                                | `guardWritable(span<CellRef>)` funnel — `sessionFor` refusal at every existing-cell link site: `cells ⍴ d.1` (`:1020-1028`), `↑`/`↓` linkChains (`:1125`,`:1148`), mixed-subview vector (`:352`), displaced-slot writes                                                                                                            | D7 — the provenance guard no other row owns.                        |
| Q8  | `vpl/compiler.cpp`                              | the new quad kinds refuse in `compileQuad` (`:1085-1104`) — mutating constructs have no VM op                                                                                                                                                                                                                                      | Compile-time is the honest boundary (VQL Q7B).                      |
| Q9  | `tests/xuzz/vpl_lens_test.cpp` (new)            | the §1 journey: `⎕LENS` → `⍴` walk → `⎕MOUNT` → back → `⎕EDIT`/`⎕COMMIT`/`⎕REVERT`/`⎕LNEXT`/`⎕LSTUB`/`⎕BROWSE`; derived-verb refusal (`⎕MOUNT¨`)                                                                                                                                                                                   | A test sits with its code — the acceptance artifact.                |

The session machinery (`session_ops`, `ownsCell`, `canApply`, `revertKey`, `commit()`→
`expected<int>`, `windowedFor`/`nextStubAt`, the `openLensUri` scheme guard) is the P5–P8 layer —
VPL's quads call it.

## 8. What this is not

- Not the `:lens` REPL verbs — those stay.
- Not a call syntax — `f(x)` stays a strand; the `⎕` system verb is the idiom.
- Not a session stack — "back" is a bound view.
- Not a lazy-mapped view over sessions — a quad evaluates once; `env_` holds views, not thunks.
- Not yet xuzz's picker surface — different frontend's doc.
