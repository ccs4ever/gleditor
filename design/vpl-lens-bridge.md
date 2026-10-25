# VPL Lens Bridge: Lenses as System Verbs in an Array Language (v1.10)

A design specification for how a **VPL program** — not the `vpl` REPL's `:lens` verbs — opens,
traverses, mounts, writes back, and releases projection lenses. Design deliverable only: the
signatures are proposals; `openLensUri` and the `session_ops` gesture layer are the seam, not a new
layer. The sibling spec is [`vql-lens-bridge.md`](vql-lens-bridge.md) v6.6 — the capability set is
the same; only the surface is re-thought for an applicative array language. Round 1 critique folded
in.

VPL is **applicative**: an expression is a verb applied to a `VplView` producing a `VplView`. There
is no call syntax — `f(x)` strands into a vector (`parser.cpp:223-234`) — and no backtracking —
`evaluateNode` runs each AST node exactly once (`vpl_engine.cpp:206-245`). The design center
therefore shifts from VQL's "where may a mutating step appear" to two array-native questions: the
**multiplicity** of a mutating verb applied to an N-cell view, and which **operand positions** may
carry a session-mutating construct.

______________________________________________________________________

## 1. Intent

A VPL program should be able to write, in its own terms:

- **open a source** as a projected root — `⎕DIR`/`⎕SQL`/`⎕GIT`/`⎕LOG`/`⎕JOURNAL`/`⎕CFG` typed specs,
  `⎕UNION` over a rank of child specs, `⎕META` over the coordinator's `MetaSource`, the generic
  dyadic `'kw' ⎕LENS 'spec'` for registered lens keywords, and monadic `⎕LENS path` for the
  bare-path auto-detect — the `openLensUri` scheme table (`lens_uri.cpp:23-125`) underneath, never a
  URI string at the surface;
- **traverse** with the view algebra it already uses — `(lens ⍴ d.contains)[1] ⍴ d.seq`, `V[i]` (the
  listing idiom is a two-step; D4 names why the one-shot `lens ⍴ d.contains d.seq` does not work);
- **mount a delegate** under a projected cell and come **back** — "back" is a bound view;
- **edit, commit, revert** on write-capable lenses, and **materialize** deferred pages;
- **connect stores/slices** — the coordinator's `d.stores` rank as a connection rank — so a
  multi-store or multi-slice program is an *array of sources*, not a flag.

Acceptance journey (the ux-validation shape):

```text
⎕DIR '/x' → (lens ⍴ d.contains)[1] ⍴ d.seq → ⎕MOUNT repo-cell → walk commits →
⎕UNMOUNT → keep walking → ⎕SQL '/x.db' → 't' ⎕BROWSE lens → '99' ⎕EDIT row →
⎕COMMIT lens → ⎕REVERT a pending edit → 2 ⎕LNEXT view → ⎕LCLOSE
```

## 2. Current surface (already in the build)

| surface                                                 | what it does                                                                                                                                                                                                                                                                       |
| ------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `:lens <uri>` / mount / unmount / next / commit / close | REPL verbs — `LensRepl` on the engine's own core (`main.cpp:464`); `bindRank` already falls back to `lensRepl.root()` when a dim is not in the open store (`main.cpp:604-632`)                                                                                                     |
| `--lens <uri>`                                          | programmatic raster — `runLensRaster`; `meta:` goes through `lensRepl` with the coordinator's `MetaSource` (`main.cpp:449-476`)                                                                                                                                                    |
| `:open <store>` / `:rank <dim>`                         | store connection + rank binding → `data` (`main.cpp:693-718`, `:603-658`); `proxyFor` mints the foreign cells and `bindDimension` binds the axis (`:435-442`, `:643-653`)                                                                                                          |
| `data`, `H`, `d.*`                                      | `env_` variables, the home cell, dimension nouns (`evalIdentifier`, `vpl_engine.cpp:364-378`)                                                                                                                                                                                      |
| `⎕` / `⎕READ` / `⎕SPLIT`                                | the quad system-function family — the only host-effect surface (`token.hpp:82-84`; `evalQuad`, `vpl_engine.cpp:692-748`); `consoleEffects_` already gates `⎕` under remote evaluation (`:695-697`)                                                                                 |
| `view ⍴ dims`, `view[i]`, `⊃`, `≡`                      | view construction over a dim list (`vpl_engine.cpp:1010-1028`), indexing — whose scalar yield keeps `origin()=cell` (`:399-412`) but multi-axis `S[i;j]` silently uses `indices()[0]` only (`:392`) — clone-master disclose (`:883-898`), and origin-identity match (`:1176-1184`) |

VPL has **no lens construct reachable from a program**, **no session registry the engine can see** —
the REPL's session is `LensRepl::session_` (`lens_repl.hpp:110`); the multi-session precedent is
vlace's `Compiler::sessions_` `vector<shared_ptr>` + `sessionFor`/`ownsCell` scan
(`compiler.hpp:227`, `compiler.cpp:301-307`) — **no `sessionFor` guard** anywhere in
`vpl_engine.cpp`'s write paths, **no string equality** — `=`/`≡` on string scalars reduces both to
`0` and returns 1 always (`vpl_engine.cpp:1176-1184`, `:1197-1204`, `:1333-1334`), so any key-string
check is unfalsifiable today — and **no `VplView` cell-list mode**: a view is origin+axes only
(`view.hpp`), so "a rank of these cells" has no representation.

## 3. Design decisions

### D1 — the `⎕` system verb is the surface; opens take typed specs, not URI strings

APL routes host interaction through `⎕`-functions (`⎕READ`/`⎕SPLIT` is VPL's own precedent), and J
routes it through `w.` foreign words — both idioms land on the same tokens here. A new `Quad*` token
family carries the whole surface; `⎕` scanning is extended from the three literals to `⎕`+NAME
(`lexer.cpp:405-422`) — and must **word-match**: today's literal `starts_with("READ")` lets
`⎕READONLY` lex as `QuadRead` + `ONLY` (`lexer.cpp:409-420`). The J-spellings (`dir.`, `sql.`,
`mount.`, `edit.`, `commit.`…) join `scanIdentifierOrKeyword`'s table (`lexer.cpp:271-296`) — with
`key.` already taken by `TokenKind::Key` (⌸, `lexer.cpp:283-284`), `⎕LKEY` spells `lkey.`; the whole
set must be rechecked against that table before naming lands.

The URI-string surface VQL rejected stays rejected: **no `⎕LENS 'dir:/x'`**. Each scheme is its own
verb — `⎕DIR '/x'`, `⎕SQL '/x.db'`, `⎕GIT '/repo'`, `⎕LOG '…'`, `⎕JOURNAL '[unit]'`, `⎕CFG '…'` —
mapping VQL's `fsLens`/`gitLens`/`sqlLens`/`logLens`/`journalLens`/`cfgLens` one-to-one. `⎕UNION`
takes a **rank of child specs** — `⎕UNION ('dir:/a' 'cfg:/b')`, each element one complete
`openLensUri` input — so the `union:` scheme's `|`-delimited sub-grammar (the least xanalogical
form) disappears into the array; children dedupe per child source identity. `⎕META 'x'` goes through
the coordinator's `MetaSource` — `meta:` is **not** an `openLensUri` scheme (`kSchemes` refuses it,
`lens_uri.cpp:42-45`; D6). The generic dyadic `'kw' ⎕LENS 'spec'` serves custom lens keywords once
VQL Q1's `registerLensScheme` turns `kSchemes` into a registry; the **monadic** `⎕LENS path` keeps
only the bare-path auto-detect `openLensUri` already runs — a directory opens a `RouterLens`, a file
its delegate lens or a `FilesystemLens` (`lens_uri.cpp:108-124`), so `⎕LENS 'foo.db'` opens a
`SqlLens` with no scheme word — while a `scheme:`-prefixed operand refuses: spell the scheme as the
verb.

Every open is **session-mutating**; **open dedupe is by canonical source identity**, not operand
text — the normalized URI/resolved path recorded at open on *every* entry path (`⎕` opens,
`registerLensScheme` arrivals, and `LensRepl::adopt`/`open`), so `⎕DIR '/x'`, `'dir' ⎕LENS '/x'`,
and `:lens dir:/x/` through a symlink dedupe to one live session — two sessions over one writable
source would split `dirty_` and cross-clobber on `commit()` (`keyToCell_` is per-session,
`projection_session.hpp:200-201`); a *different* source mints its own projection into `core`'s
arena, where `bindRank` and `directEngine` already look (`main.cpp:462-464`).

Three dyadic gestures need a left operand — `'99' ⎕EDIT cellview`, `'t' ⎕BROWSE cellview`,
`n ⎕LNEXT view` — so `QuadExpr` gains an optional left (the parser's quad arm *drops* `leftNoun`
today, `parser.cpp:164-168` — Q2 names the full site list). A **scalar left broadcasts**, VPL's
dyadic convention (`vpl_engine.cpp:1207-1260`): `'99' ⎕EDIT R` writes the same text to every cell of
`R`; a rank left zips to `min(l,r)`. `⎕MOUNT¨` becomes a *parse* error once the quad arm widens —
the quad arm intercepts before the adverb check — but the conjunctions still smuggle quad kinds into
derived-verb position; D3 refuses them.

### D2 — sessions live on `VortexCore`; `lens` is the `$lens` equivalent

`vpl` builds one long-lived `VPLEngine` for the process (`main.cpp:399`), so sessions survive across
lines either way — but the `:lens` REPL session lives in `LensRepl` while `⎕`-opened sessions would
live in the engine: the registry belongs on the **shared core** so both coexist and `sessionFor`
resolves either (the same `VortexCore` row VQL's Q4 registers: arena-level `weak_ptr` membership +
owning `vector<shared_ptr>`; `LensRepl::session_` migrates `unique_ptr`→`shared_ptr`). The two kinds
of session join at **different ownership strengths**: a `⎕`-opened session is *owned* by the
registry's `vector<shared_ptr>`; a REPL-joined session is **weak-only** — the REPL owns it, the
registry just enumerates it, and `:lens close` lets the entry expire. And `LensRepl::adopt`
**destroys the prior session on every `:lens open`** — uncommitted `dirty_` drops silently and the
old session's projected cells ghost in the arena (still walkable, but `sessionFor` no longer owns
them, so D7's guard stops protecting them) — so the weak join must *re-register per `:lens open`*,
not once at `LensRepl` construction.

An open binds **`lens`** in `env_` to the session's roots — which needs a **`VplView` cell-list
mode** (Q10: an ordered `vector<CellRef>` payload; `collectCells` yields the list, `shape()` is
`{N}`), because today's origin+axes view cannot represent "a rank of roots", and `roots().front()`
both loses the forest and is **UB on a zero-root session** — an empty source binds an *empty* view
(the session still registers for dedupe and `⎕LCLOSE`; an empty source is not a refusal, matching
VQL). `lens` is a **reserved name** — `lens ←` refuses — because the engine re-binds it on every
open *and on dedupe recency*: a deduped `⎕DIR '/x'` re-binds `lens` to the live session's roots even
though nothing new was minted, and a silently shadowed `lens` would read as stale. Re-bind per
evaluated line picks the registry's most recent *live* open (expired `weak_ptr`s skipped; a
`:lens close` leaves no stale `lens`).

The open verbs **yield cell views** — a rank-1 cell-list view of the session's roots — and the
mutating gestures take cell views only (D3's operand rule): the ExternalKey string stays probe
currency (`⎕LKEY`/`⎕LCELL` convert between the two), so a minted text cell equal to a key confers no
gesture rights. The string remains the stable cross-line handle — VQL D2's `fs:52:794498` argument
holds; a `CellRef` in a long-held bound view is only valid while the arena is.

### D3 — multiplicity lives inside the verb; the dedupe taxonomy and the operand rule

A quad applied to an N-cell view `collectCells` **once** (`view.hpp:289-332`) and the verb decides.
The **operand-resolution chain is stated once**: a view operand contributes its cells' origins; each
origin → `sessionFor` → `keyFor`; a session-unowned origin is a per-cell `Err`; a rank-0 *string*
view bypasses `collectCells` entirely (an origin-less scalar yields nothing, `view.hpp:292-298`) and
resolves `scalarString()`/`textOf` → registry `cellFor` — **for probes only**: mutating gestures
take cell views, so a string operand to `⎕EDIT`/`⎕MOUNT`/… refuses (or, if string operands are ever
wanted there, adopts `keyArg`'s path translation, `lens_repl.cpp:19-30` — one rule, not both).
Per-cell refusals drop the cell and push the reason onto the **`⎕LERR`** reasons rank (D3's drop
channel is mute without it); whole-verb refusals throw.

Dedupe taxonomy — each verb runs **once per distinct scope** of its kind:

- **per cell** — `⎕MOUNT`, `⎕EDIT`, `⎕REVERT`;
- **per (session, mountBoundary)** — `⎕UNMOUNT` (`mountBoundary` resolved *before*
  `sessionUnmountAt`), `⎕LSTUB` and `⎕LNEXT` (stub scopes are mount-scoped, not session-scoped —
  `session_ops.hpp:106-146`), and `'t' ⎕BROWSE`, deduped **first-wins on the table arg**:
  `SqlLens::browse` appends a `SqlQuery` per call (`sql_lens.cpp:442-459`) and `isBrowsed` catches
  only a same-table re-browse (`:468-471`), so mixed table args across one mount's cells run the
  first and land the rest on `⎕LERR`. `n ⎕LNEXT view` makes page count the operand, not a function
  of operand size: `n` pages per distinct scope, monadic `⎕LNEXT` ≡ `1 ⎕LNEXT`;
- **per session** — `⎕COMMIT`, `⎕LDIRTY`, `⎕LREFRESH`, `⎕LCLOSE`;
- **per source identity** — the open verbs, `⎕UNION` children, and `⎕STORE` (D8).

Single-eval rule, restated for mapping: **no quad kind may occupy verb-operand position** — not
adverb `¨`/`/`/`\` and not the conjunctions `∘.`/`⌸`/`⍤`. `⎕MOUNT¨` is already a parse error once
the quad arm widens, but `parser.cpp:142` takes *any* token as `verbTok`, `computeDyadicScalar`'s
default arm is a silent identity, and `evalConjunction`'s `Key` arm never evaluates its right
operand — a quad smuggled through `∘.`/`⌸` is a silent drop. Q2 refuses quad kinds inside
`VerbExpr`/conjunction construction for **all** quad kinds — `⎕`/`⎕READ`/`⎕SPLIT` included. Nothing
else needs the rule: `env_` replay is a pure lookup (`vpl_engine.cpp:373-377`), assignment stores a
view not a thunk (`:380-384`), and each AST node evaluates once — there is no `where`/join/`for`
multiplicity to ban.

Remote evaluation: `mesh_evaluator` runs the VPL frontend with `withConsoleEffects(false)`
(`mesh_evaluator.cpp:916-917`) — session-mutating quads refuse under `!consoleEffects_` at least as
hard as `⎕` print does (`vpl_engine.cpp:695-697`); the const probes stay legal.

The mutating set is

```text
{⎕DIR, ⎕SQL, ⎕GIT, ⎕LOG, ⎕JOURNAL, ⎕CFG, ⎕META, ⎕UNION, ⎕LENS, ⎕STORE,
 ⎕MOUNT, ⎕UNMOUNT, ⎕LNEXT, ⎕COMMIT, ⎕EDIT, ⎕REVERT, ⎕BROWSE, ⎕LCLOSE}
```

and the exempt const probes are `{⎕LSTUB, ⎕LDIRTY, ⎕LREFRESH, ⎕LKEY, ⎕LCELL, ⎕LSCAL, ⎕LERR, ≡}`. The
`⎕L` prefix is a stated convention, not a rule the lexer enforces: it marks **session-machinery
constructs** — the lifecycle verbs (`⎕LENS`, `⎕LCLOSE`), the deferred-projection machinery
(`⎕LSTUB`, `⎕LNEXT`, `⎕LREFRESH`), the session-state readers (`⎕LDIRTY`, `⎕LERR`, `⎕LKEY`, `⎕LCELL`,
`⎕LSCAL`) — while the generic cell gestures keep short names
(`⎕MOUNT`/`⎕UNMOUNT`/`⎕EDIT`/`⎕COMMIT`/`⎕REVERT`/`⎕BROWSE`/`⎕STORE`). `⎕DIRTY` was the exception and
normalizes to `⎕LDIRTY`.

Under `--engine vortex` the new kinds hit `compileQuad` (`vpl/compiler.cpp:1085-1109`), which knows
only the three existing quads — and whose fallthrough returns `core_.home()` (`:1108`), a silent
wrong answer. Q9 makes the unknown-quad arm **throw**; the VM itself links through `core_.link`
(`vortex_vm.cpp:526-546`), which the core funnel (Q8) covers — see D7.

### D4 — reads observe the arena at walk time — and extents freeze

`collectCells`/`shape` walk links lazily at use (`view.hpp:201-332`), so a bound view sees whatever
`refresh()` last stated — gestures `refresh()` into the same arena, and **`⎕LREFRESH`** exposes the
`refreshIfChanged()` const probe VQL already carries for "did the source move". Two traps:

- `setExtents` **freezes** `shape()` (`view.hpp:103`, `:206-211`) — `⍴` case 1 bakes `res.shape()`
  at construction (`vpl_engine.cpp:1015`) — so a view bound before `⎕MOUNT`/`⎕LREFRESH` keeps the
  pre-gesture grid. The idiom is **re-`⍴` after the gesture** (or drop `setExtents` on
  session-derived views; either way the stale-extent hazard is named).
- `lens ⍴ d.contains d.seq` does **not** yield the listing: `⍴` case 1 measures the second axis's
  extent from row 0 — the root, which has no `d.seq` — and bakes `{D,1}`: a contains-spine, never
  the siblings. The listing is the **two-step** `(lens ⍴ d.contains)[1] ⍴ d.seq` —
  `VplView(firstborn, {d.seq})`, exactly `bindRank`'s fallback (`main.cpp:630-632`). §5 uses it
  throughout.

`⎕LSCAL` reads the lens-minted kinds `Int64`/`Double`/`Bool` (`mintNodeCell`,
`projection_session.cpp:95-116`) into a VPL scalar payload **keeping the kind**: `Bool` goes through
`makeScalarCell` carrying the cell's `valueBits` (or lands as 0/1, stated in Q11) — `mintRank`'s
`int64` path (`vpl_engine.cpp:1410-1425`) would silently lose it. `view[i]`'s scalar extraction
(`vpl_engine.cpp:399-412`) is the same read with `textOf` for span cells; `S[i;j]` quietly uses
`indices()[0]` only (`:392`) — a pre-existing silent drop §5 avoids by indexing rank-1 views. Handle
kinds (`OpHandle`/`ExternRef`/`ForeignOpHandle`) yield a refusal, matching `cellScalar`'s
empty-by-design. And `=`/`≡` need `isString` arms (`vpl_engine.cpp:1176-1184`, `:1197-1204`,
`:1333-1334`): today two different string scalars compare equal because both read as `0`, so a
string-equality check is unfalsifiable until Q11 lands — `≡` on *cell views* already compares
origins honestly, and per-cell identity stays `⎕LKEY` comparison.

### D5 — `session_ops` carries the gesture protocol — and `session_ops.cpp` is new

`sessionMountAt`/`sessionUnmountAt`/`sessionNextPage`/`sessionCommitAll`/`sessionEdit`/
`sessionRevert`/`sessionDirty` (`session_ops.hpp:42-171`) are the implementation — the quads call
them, inheriting the dirty-gate (edit/commit/revert exempt), `canApply` inside `editContent`, and
`commit()`→`expected<int>`. `⎕BROWSE` needs the shared `sessionBrowseAt(session, key, table)` —
mount-scoped resolve → `dynamic_cast<SqlLens*>` → `isBrowsed` → `browse` (`sql_lens.cpp:442-459`) →
`refresh()` — and `sessionMountScopeAt` for the cell→`keyFor`→`mountBoundary`→`windowedFor` preamble
shared by next/stub/browse/unmount (triplicated today; vlace's is `compiler.cpp:1441-1470`). Both
are the same `session_ops` rows VQL's Q6 registers — with one correction: **`session_ops.cpp` is a
new file**; the header is all-inline today. `browse` returns `expected<void>`, so the new query's
root comes from a `SqlLens` accessor or a browse-returns-key change (registered there).

### D6 — `⎕UNION` key-level only; `⎕META` via the coordinator's fold

`UnionLens` is neither `RouterLens`/`WindowedLens`/`SqlLens` (`union_lens.hpp:31`), so the scoped
verbs (`⎕MOUNT`/`⎕UNMOUNT`/`⎕LNEXT`/`⎕LSTUB`/`⎕BROWSE`) `Err` on union cells; only
`⎕EDIT`/`⎕REVERT`/`⎕COMMIT`/`⎕LDIRTY` and the key probes work — with the **proposed**
duplicate-ExternalKey refusal: today a key two children emit resolves first-success in
`applyContentChange` (`union_lens.cpp:87-99`) but last-emission in `restateContent`
(`projection_session.cpp:142+`) — VQL's `claimedBy` probe (its Q4/Q6 row, kept on
`union_lens.{hpp,cpp}`) is what makes "refuse ambiguous" implementable, and it is a proposal, not
existing code.

`meta:` is **not** an `openLensUri` scheme — `kSchemes` refuses it (`lens_uri.cpp:42-45`); it exists
only through `LensRepl`'s `MetaLens` over a `MetaSource` fold. `vpl` already builds that fold from
the coordinator (`main.cpp:449-461`), so `⎕META 'x'` is supportable: `VPLEngine` holds an **owned**
fold slot — the `MetaLens` *borrows* `metaFold_`, so the fold must outlive the session
(`lens_repl.hpp:102-110` orders the members for exactly this; the engine needs the same
`std::optional<Manifold>` slot, injected from `main.cpp`); absent one it refuses like any unknown
scheme.

### D7 — every VPL write site on an existing cell checks `sessionFor` — at both layers

VPL's write surface is *linking existing cells onto dims*, and the interpreter reaches
`ArenaManifold::link`/`linkChain` **directly** — `vpl_engine.cpp:352`, `:804`, `:1001`, `:1024`,
`:1075`, `:1083`, `:1126`, `:1149`, `:1401`, `:1418`, `:1435` — bypassing `VortexCore::link`
entirely. Sites: `cells ⍴ d.1` (`linkChain(cells, d)`, `:1020-1028`), `n↑V`/`n↓V` (`:1125`,
`:1148`), the mixed-subview vector link (`:352`), and the one v1.0 missed — **`n ⍳ d.x` (dyadic
Iota)**: `rankTail` + `link(prev, d, POS, c)` writes the rank's tail cell
(`vpl_engine.cpp:993-1003`). Two *contextual* writes join them:
`findOrMintDimension`→`mintNamedDimension` appends to the dims rank (`vortex_core.cpp:82-96`) and
`proxyFor` appends to `d.store-refs` (`arena_manifold.cpp:365-368`) — both existing-cell writes when
the tail is session-owned. Each overwrites a posward slot, and on a session-managed dim that is the
tombstone hazard VQL D7 names. Fresh-mint paths (`catenate` :769+, `mintRank` :1393+, `evalIndexing`
copies :437-461, `∘.`/`⌸` mints) are safe by construction; `⍫`'s `rebuildManifoldFromIndex`
(`:1186-1193`) writes the *store* fold, not the arena.

Rule — same as VQL D7, **two layers**:

1. The interpreter's `guardWritable(span<CellRef>)` funnel (Q7) inspects **every** existing
   `CellRef` about to be linked or spliced — including displaced slots (`link()` clears the
   reciprocal edge) — *before the first* `link()`: `linkChain` returns at the first refusal having
   already written the earlier links (`arena_manifold.cpp:1440-1461`), so checking inside the walk
   half-writes a mixed rank. Refusal throws → `Error:` at `main.cpp:871-874`, not per-cell drop: a
   `⍴`/`↑` over a mixed rank aborts loudly rather than relinking a subset.
1. The compiled path: `VortexVM` runs `core_.link` (`vortex_vm.cpp:526-546`), so the funnel *inside
   `VortexCore`'s write methods* — VQL's Q7B row, shared — covers `--engine vortex` whole.
   `guardWritable` is still required in the interpreter: its writes never cross `VortexCore::link` — and the `ArenaManifold` funnel needs `masterOf(cell)` (a clone *of* a session cell lands its writes on the master).

The correct content write is `⎕EDIT`; projected structure is the session's to maintain.

### D8 — multi-store is a rank of connections; multi-slice is a dim walk

`'label' ⎕STORE 'path'` — monadic `⎕STORE 'path'` defaults the label (path basename), `role`
(`"library"`), and `permascroll` (the coordinator's shared one) — runs
`coordinator.loadAndAddStore(label, role, path, permascroll)` (`multi_store.hpp:88-91`,
`multi_store.cpp:58-96`): a representative cell on the `d.stores` rank off coordinator home,
`d.clone`-ed to the slice home master, `d.name`/`d.role` attrs. **Store dedupe is by canonical
path** — a repeat `⎕STORE` re-yields the existing rep cell rather than reloading. The new store must
also reach the engine's store slot — `directEngine.setStore` or equivalent — or `⍫`
(`store_->rebuildManifoldFromIndex`, `vpl_engine.cpp:1186-1193`) still rebuilds the old one. And
binding needs more than `bindRank`'s prelude, which binds **one** axis (`main.cpp:650-652`): the
`DimensionBindingMode::NameMatch` sweep `bindRank` uses per-axis — a `bindDimensionsByNameMatch`
helper or the equivalent loop — is what makes `home ⍴ d.result` resolve at all.

Then the connection rank *is* an array: `H ⍴ d.stores` yields the connected stores, `stores[i]`
picks one, `⊃`/`-d.clone` discloses to the slice home, and `home ⍴ d.result` views the slice's ranks
— multi-slice browsing in three tokens. A store's cells are foreign (`resolveForeign`,
`main.cpp:142-154` provenance) — `⎕STORE`/`bindRank`'s `proxyFor` + `bindDimension` prelude
(`main.cpp:435-442`) mints the proxies and binds the axes, and the `sessionFor` guard treats proxies
of session-unowned space as ordinary cells. `⎕UNION` (D6) is the *content* union across sources;
`d.stores` is the *store* union — both surfaces of "multi" land in the same view algebra.

## 4. Proposed surface

### Generator verbs (session-mutating; sole-operand position)

| verb                    | operand                                                         | yields                      | notes                                                                                                                                                                                                               |
| ----------------------- | --------------------------------------------------------------- | --------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `⎕DIR` (`dir.`)         | path string, or rank of paths                                   | roots rank (cell-list view) | forces `dir:` — `RouterLens` for a directory                                                                                                                                                                        |
| `⎕SQL` (`sql.`)         | dsn/path string or rank                                         | roots rank                  | `sql:`                                                                                                                                                                                                              |
| `⎕GIT` (`git.`)         | repo path or rank                                               | roots rank                  | `git:`                                                                                                                                                                                                              |
| `⎕LOG` (`log.`)         | path or rank                                                    | roots rank                  | `log:`                                                                                                                                                                                                              |
| `⎕JOURNAL` (`journal.`) | unit spec or rank                                               | roots rank                  | `journal:` / `journal:<unit>`                                                                                                                                                                                       |
| `⎕CFG` (`cfg.`)         | path or rank                                                    | roots rank                  | `cfg:`                                                                                                                                                                                                              |
| `⎕UNION` (`union.`)     | rank of child specs — `('dir:/a' 'cfg:/b')`                     | union roots rank            | one `openLensUri` input per element — no pipe-delimited sub-grammar; children dedupe per child source identity                                                                                                        |
| `⎕META` (`meta.`)       | spec string                                                     | roots rank                  | the `LensRepl`/`MetaSource` path (D6) — `meta:` is **not** a `kSchemes` scheme                                                                                                                                      |
| `⎕LENS` (`lens.`)       | monadic: bare path — auto-detect; dyadic `'kw' ⎕LENS 'spec'`    | roots rank                  | monadic keeps `lens_uri.cpp:108-124`'s dir/file/delegate sniff (`'foo.db'` → `SqlLens`); a `scheme:`-prefixed operand refuses; dyadic serves `registerLensScheme` keywords — `'sql' ⎕LENS '/x.db'` ≡ `⎕SQL '/x.db'` |
| `⎕STORE` (`store.`)     | path string, or rank of paths — dyadic: `'label' ⎕STORE 'path'` | rank-0 view of the rep cell | `loadAndAddStore` + `proxyFor`/NameMatch sweep + `setStore` for `⍫` (D8); dedupe by canonical path; needs the coordinator injected (Q6)                                                                             |

### Gesture verbs (`Err`→drop per cell — reasons land on `⎕LERR`; scope dedupe per D3)

| verb          | operand   | yields                          | notes                                                                                                             |
| ------------- | --------- | ------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| `⎕MOUNT`      | cell view | the delegate root (rank-0 view) | `sessionMountAt` returns **one** `ExternalKey` — one root, not "delegate roots"; per-cell                         |
| `⎕UNMOUNT`    | cell view | the mount-point cells           | `mountBoundary` resolved *before* `sessionUnmountAt` (`router_lens.hpp:70-71`); deduped per mount                 |
| `n ⎕LNEXT`    | cell view | materialized stub cells         | `sessionNextPage` — mount-scoped on `Router`, session-level on `WindowedLens`; `n` pages per scope, monadic `n=1` |
| `⎕LSTUB`      | cell view | pending stub cells              | `nextStubAt` — const, not dirty-gated; deduped per (session,mount)                                                |
| `⎕COMMIT`     | cell view | applied count (scalar)          | `sessionCommitAll` → `commit()` count; once per distinct session                                                  |
| `text ⎕EDIT`  | cell view | the edited cells                | `sessionEdit`; scalar text **broadcasts** over the view, rank text zips `min(l,r)`; per-cell                      |
| `⎕REVERT`     | cell view | the reverted cells              | `sessionRevert` — `Err` on source-vanished                                                                        |
| `'t' ⎕BROWSE` | cell view | the new query's root view       | `sessionBrowseAt` — sql-only, mount-scoped, **first-wins** table arg (`sql_lens.cpp:442-459`)                     |
| `⎕LCLOSE`     | cell view | empty                           | releases the session — erases the registry entry + unbinds `lens`; once per session (in scope — see §6)           |

### Pure probes

| verb                      | operand          | returns                                | notes                                                                                                                                   |
| ------------------------- | ---------------- | -------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| `⎕LKEY` (`lkey.`)         | cell view        | ExternalKey strings (rank)             | `keyFor` per cell; authored/attr cells yield `""` — the `cellKey`-vs-`""` collision VQL notes applies                                   |
| `⎕LCELL` (`cell.`)        | key string(s)    | rank-0 cell views / cell-list rank     | `cellFor` via registry scan — the stable handle; absent key → empty; needs the cell-list mode (Q10) for rank>1                          |
| `⎕LSCAL` (`scalar.`)      | cell view        | Int64/Double/Bool scalars              | `asInt64`/`asDouble`/`asBool` on `valueBits` — keep the kind via `makeScalarCell` (Q11); non-minted kinds refuse                        |
| `⎕LREFRESH` (`lrefresh.`) | cell view        | the roots rank, post-refresh           | `refreshIfChanged()` — the const probe VQL carries; re-`⍴` anyway, D4's extents froze                                                   |
| `⎕LDIRTY` (`ldirty.`)     | cell view        | `dirtyCount` (scalar)                  | `session.dirtyCount()` — const probe (misfiled as `⎕DIRTY` under gestures in v1.0)                                                      |
| `⎕LERR` (`lerr.`)         | —                | per-cell refusal reasons (string rank) | the drop channel otherwise swallows *why*; engine-held last-gesture reasons                                                             |
| `≡` / `-:`                | two rank-0 views | `Bool`                                 | origin-identity on cell views today (`vpl_engine.cpp:1176-1184`); Q11's `isString` arms make string operands honest — `⎕LEQ` not needed |

## 5. Worked example

```vpl
      ⍝ open a dir lens — a typed spec, not a URI string; yields the roots
      ⍝ rank (a cell-list view) and binds lens to the same view
      R0 ← ⎕DIR '/tmp/lensj'
      ⎕LKEY R0                       ⍝ keys are probe currency
fs:52:794498

      ⍝ the listing is a two-step — the one-shot `lens ⍴ d.contains d.seq`
      ⍝ bakes {D,1} off the seq-less root (D4): down d.contains to the
      ⍝ firstborn, then across d.seq
      S ← (lens ⍴ d.contains)[1] ⍴ d.seq
      S[2]                             ⍝ scalar view, origin() = repo cell
/repo

      ⍝ mount the git delegate under it — operand is a cell view
      M ← ⎕MOUNT S[2]                  ⍝ yields the delegate's one root
      ⎕LKEY M
git:…:HEAD
      (M ⍴ d.contains)[1] ⍴ d.seq      ⍝ the commit walk — same two-step

      ⍝ back out — the mount cell is the boundary; then re-⍴, S's extents
      ⍝ froze at construction (D4)
      ⎕UNMOUNT S[2]
      S ← (lens ⍴ d.contains)[1] ⍴ d.seq

      ⍝ a sql lens — schema-only until browsed; the open re-binds lens
      ⎕SQL '/tmp/lensj/data.db'
      B ← 't' ⎕BROWSE lens             ⍝ yields the new query's root view
      R ← (B ⍴ d.contains)[1] ⍴ d.seq  ⍝ the query's row cells
      '99' ⎕EDIT R[0]                  ⍝ scalar text would broadcast over R
      ⎕LDIRTY lens
1
      ⎕COMMIT lens
1
      'x' ⎕EDIT R[0]
      ⎕REVERT R[0]                     ⍝ drop the pending edit; restate reads
                                       ⍝ the source back

      ⍝ pages, keys, identity — statements separate by newline or ;
      ⎕LSTUB lens ; 2 ⎕LNEXT lens      ⍝ pending stubs; two pages per scope
      ⎕LKEY R[0]
sql:…:t:1
      (⎕LCELL 'sql:…:t:1') ≡ R[0]      ⍝ origin identity on cell views;
1                                      ⍝ on *strings* ≡ is unfalsifiable
                                       ⍝ until Q11's isString arms

      ⍝ multi-source: one union session over child specs, or N opens
      ⎕UNION ('dir:/a' 'cfg:/b')       ⍝ children dedupe by source identity
      'sql' ⎕LENS '/c.db'              ⍝ generic scheme+spec ≡ ⎕SQL '/c.db'
      ⎕DIR ('/a' '/d')                 ⍝ two opens, deduped by source identity

      ⍝ multi-store: the d.stores rank as a connection array
      'lib' ⎕STORE '/path/to/slice'
      stores ← H ⍴ d.stores
      home ← ⊃ stores[0]               ⍝ disclose → clone master → slice home
      home ⍴ d.result                  ⍝ needs the NameMatch sweep (Q6)

      ⎕LCLOSE lens                     ⍝ releases the session; lens unbinds
```

A verb takes the view as its operand — `⎕MOUNT S[2]` mounts under that cell; `⎕MOUNT S` mounts under
*every* cell `S` reaches (per-cell yield; refused cells drop, their reasons on `⎕LERR`).
`⎕COMMIT lens` over any cells of one session commits **once** (D3's dedupe), yielding `[applied]`,
not `[applied,0,0…]`. An empty operand is a no-op: `⎕COMMIT (empty)` runs no gesture.

## 6. Open questions

1. **Forest yield** — *resolved by Q10*: the cell-list `VplView` makes the roots rank a first-class
   yield, so `⎕DIR`/dedupe re-binds lose nothing. Residual: `lens`'s `origin()` is the first list
   element, so `lens ⍴ d.contains` still walks from `roots()[0]` — an N-root traversal wants a per-
   cell `⍴` map, which is an ordinary view-map problem, not a lens one.
1. **`⎕LERR` retention** — the reasons rank is in the surface; how long the engine holds per-cell
   reasons (last gesture? per session?) mirrors VQL's open question 1, same coordinator-lived state.
1. **`⎕META` under the engine** needs the owned `MetaSource` fold injected — with it, it works;
   without, it refuses like any unknown scheme (Q6 names the injection and the fold slot).
1. **`≡` on rank-N views** compares only origins; `=`/`≡` on strings get `isString` arms in Q11 —
   per-cell identity is then `⎕LKEY` + `=` on strings, or `≡` on cell views; no `⎕LEQ` needed.
1. **J-spelling collisions** — `lkey.` replaces the `key.` collision (⌸); the *whole* table still
   needs rechecking against `scanIdentifierOrKeyword` before naming lands.
1. **`⎕LCLOSE`** — in scope for VPL (VQL still defers `lensClose` to U-V2): a `weak_ptr`-registry
   erase + `lens` unbind is cheap given Q4. What it does to a *REPL-joined* session — membership
   erase only, the REPL still owns — is the residual detail.

## 7. Proposal register

| Q   | files                                                                   | change                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | why not caller/vpl                                                                                                                                            |
| --- | ----------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Q1  | `vpl/token.hpp` + `vpl/lexer.{hpp,cpp}`                                 | `QuadDir`/`QuadSql`/`QuadGit`/`QuadLog`/`QuadJournal`/`QuadCfg`/`QuadUnion`/`QuadMeta`/`QuadLens`/`QuadStore`/`QuadMount`/`QuadUnmount`/`QuadNext`/`QuadStub`/`QuadCommit`/`QuadEdit`/`QuadRevert`/`QuadBrowse`/`QuadDirty`/`QuadClose`/`QuadKey`/`QuadCell`/`QuadScalar`/`QuadRefresh`/`QuadErr` TokenKinds; **word-matching** `⎕`+NAME scan (`lexer.cpp:405-422` — literal prefix today lets `⎕READONLY` lex `QuadRead`+`ONLY`); J-spellings in `scanIdentifierOrKeyword` (`:271-296`) — `key.` is `TokenKind::Key` (`:283-284`), so `lkey.`; `isVerb` entries (`token.hpp:233`); `tokenKindToString` is an exhaustive switch (`token.hpp:108-231`) → new kinds trip `-Wswitch` | The quad surface is the only host-effect idiom.                                                                                                               |
| Q2  | `vpl/ast.hpp` + `vpl/parser.cpp`                                        | `QuadExpr` gains an optional left operand; the new kinds land in **all three** sites — the quad arm's `check` list (`parser.cpp:164-168`, which today drops `leftNoun`), `parseSubExpr`'s literal verb-start list (`:110-112`), and the dyadic follow-set (`:123-125`) — `isVerb` alone falls through to `MonadicExpr`/generic `VerbExpr` → silent garbage; **refuse quad kinds as `verbTok`** in `VerbExpr`/conjunction construction (`:140-146`, `:181-188`) for all quads — `∘.`/`⌸`/`⍤` take any token (`computeDyadicScalar` default → identity; `evalConjunction` `Key` arm drops the right operand)                                                                        | The payload gestures have nowhere else to take an arg, and the smuggle must die at construction.                                                              |
| Q3  | `vpl/vpl_engine.{hpp,cpp}`                                              | `evalQuad` lens dispatch — the operand-resolution chain (D3: origins→`keyFor`, strings→`cellFor` for probes only); `session_ops` calls; the per-cell / per-(session,mount) / per-session / per-source dedupe taxonomy incl. browse first-wins; broadcast-left dyadics; `lens` reserved-name auto-bind + dedupe recency re-bind; zero-root → empty view; per-cell `Err`→drop + `⎕LERR` reasons rank; `!consoleEffects_` refusal; `sessionFor` accessor over the core registry                                                                                                                                                                                                      | The verb seam lives only here.                                                                                                                                |
| Q4  | `vortex_core.{hpp,cpp}` + `lens_repl.{hpp,cpp}`                         | **the VQL Q4 row, shared**: `lensSessions()` registry on `VortexCore` (arena-level `weak_ptr` membership + owning `vector<shared_ptr>`); `LensRepl::session_` `unique_ptr`→`shared_ptr`, **weak-only** join re-registered per `:lens open` (`adopt` destroys the prior session — uncommitted `dirty_` drops, orphaned cells ghost); canonical source identity recorded at open on every path incl. `adopt`/`open`; `sessionFor` arena-scoped                                                                                                                                                                                                                                      | One registry serves vquery, vlace and vpl.                                                                                                                    |
| Q5  | `session_ops.{hpp,cpp}` + `sql_lens.{hpp,cpp}` + `union_lens.{hpp,cpp}` | **the VQL Q6 row, shared**: `sessionBrowseAt`, `sessionMountScopeAt`, `SqlLens` root-key accessor — **`session_ops.cpp` is new** (the header is all-inline today); `UnionLens::claimedBy` ambiguous-key probe is a *proposal* (`union_lens.cpp:87-99` first-success vs `projection_session.cpp:142+` last-emission)                                                                                                                                                                                                                                                                                                                                                               | The resolvers both frontends need.                                                                                                                            |
| Q6  | `apps/vpl/main.cpp` + `vpl/vpl_engine.hpp`                              | inject `MultiStoreCoordinator *` + `MetaSource` into `VPLEngine` (`⎕STORE`, `⎕META`) with an **owned** fold slot (`MetaLens` borrows `metaFold_`, `lens_repl.hpp:102-110`); re-bind `lens` per REPL line from the most-recent live session; `directEngine.setStore` so `⍫` sees the new store; the `NameMatch` binding sweep `bindRank`'s one-axis prelude doesn't do (`main.cpp:650-652`) — `bindDimensionsByNameMatch` or equivalent; `--lens` + `-e` composition                                                                                                                                                                                                               | The engine holds neither the coordinator nor the fold source today.                                                                                           |
| Q7  | `vpl_engine.cpp`                                                        | `guardWritable(span<CellRef>)` funnel — `sessionFor` refusal at every existing-cell write site: `cells ⍴ d.1` (`:1020-1028`), `↑`/`↓` (`:1125`,`:1148`), mixed-subview vector (`:352`), **`n ⍳ d.x`** (`:993-1003`), `findOrMintDimension`/`mintNamedDimension` (`vortex_core.cpp:82-96`), `proxyFor`'s `d.store-refs` append (`arena_manifold.cpp:365-368`), displaced slots; pre-inspect ALL cells before the first `link()` — `linkChain` half-writes (`arena_manifold.cpp:1440-1461`)                                                                                                                                                                                         | D7's interpreter layer — the interpreter bypasses `VortexCore::link` (`:352`,`:804`,`:1001`,`:1024`,`:1075`,`:1083`,`:1126`,`:1149`,`:1401`,`:1418`,`:1435`). |
| Q8  | `arena_manifold.{hpp,cpp}` + `projection_session.{hpp,cpp}`                                                 | **the VQL Q7B row, shared**: the `sessionFor` funnel inside **`ArenaManifold::link`/`linkChain`/`setValueBits`** — the actual single boundary (`VortexCore::link` is a thin wrapper over `arena_.link`, `vortex_core.cpp:128-167`; `VortexVM` does ~14 bookkeeping `core_.arena().link` writes, `vortex_vm.cpp:88`-`:922`; `VPLCompiler`/`VortexStdLib`/`mesh_evaluator`/`or_parallel` all write the arena directly). Session-internal writes exempt via the `ProjectionSession` inside-session-write flag                                                                                                                                                                                                                                                                                                                                                                                                                                                         | One boundary every writer crosses; VPL's interpreter still needs Q7.                                                                                          |
| Q9  | `vpl/compiler.cpp`                                                      | `compileQuad` refuses the new quad kinds by **throwing** — the fallthrough returns `core_.home()` (`:1108`), a silent wrong answer (`:1085-1109`)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 | Compile-time is the honest boundary — but only if it refuses.                                                                                                 |
| Q10 | `vpl/view.hpp`                                                          | `VplView` cell-list mode — an ordered `vector<CellRef>` payload; `collectCells` yields the list (`view.hpp:289-332`), `shape()` is `{N}` (`:201-211`) — makes the roots-rank yields, `⎕LCELL` rank yields, and the `⎕STORE` connection rank implementable                                                                                                                                                                                                                                                                                                                                                                                                                         | The alternative was scalar-in/scalar-out only — rank yields unimplementable.                                                                                  |
| Q11 | `vpl_engine.cpp`                                                        | `isString` arms in `=`/`≡` (`:1176-1184`, `:1197-1204`, `:1333-1334` — compare `scalarString()`, not `0=0`); `⎕LSCAL` keeps `Bool` via `makeScalarCell` + `valueBits` (`mintRank(int64)` drops the kind, `:1410-1425`)                                                                                                                                                                                                                                                                                                                                                                                                                                                            | String equality is unfalsifiable today; the acceptance journey leans on it.                                                                                   |
| Q12 | `tests/xuzz/vpl_lens_test.cpp` (new)                                    | the §1 journey: `⎕DIR` → two-step listing → `⎕MOUNT` → back → `⎕EDIT`/`⎕COMMIT`/`⎕REVERT`/`⎕LNEXT`/`⎕LSTUB`/`⎕BROWSE`/`⎕LCLOSE`; quad-in-`VerbExpr` refusals (`⎕MOUNT¨` parse error, `∘.`/`⌸` smuggles refused); dedupe by source identity                                                                                                                                                                                                                                                                                                                                                                                                                                        | A test sits with its code — the acceptance artifact.                                                                                                          |

The session machinery (`session_ops`, `ownsCell`, `canApply`, `revertKey`, `commit()`→
`expected<int>`, `windowedFor`/`nextStubAt`, the `openLensUri` scheme guard) is the P5–P8 layer —
VPL's quads call it.

## 8. What this is not

- Not the `:lens` REPL verbs — those stay.
- Not a call syntax — `f(x)` stays a strand; the `⎕` system verb is the idiom.
- Not a URI-string surface — schemes are verbs; `⎕LENS` keeps only the bare-path auto-detect and the
  registered-keyword dyadic.
- Not a session stack — "back" is a bound view.
- Not a lazy-mapped view over sessions — a quad evaluates once; `env_` holds views, not thunks.
- Not yet xuzz's picker surface — different frontend's doc.
