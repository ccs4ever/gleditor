# SQL to ZigZag: A Read-Only Relational Lens with Windowed Replay and On-Demand Promotion

A design specification for `SqlLens`: a
[`ProjectionLens`](apps/common/xanadu/zigzag/projection_lens.hpp) that projects **SQL query
results** — rows, columns, and the schema they conform to — into a ZigZag manifold, so the same
visualizer, the same VQL/VPL/Vlog runtimes, and the same navigation that work over xanadocs and
directory trees work over a relational database. SQLite is the first backend (already a link
dependency, [`Makefile:277`](Makefile)); PostgreSQL follows behind a port the identity scheme is
designed around from the start.

Three requirements shape everything below:

1. **Windowed results with replay.** A query result is unbounded; the lens must never enumerate it
   eagerly. Pages materialize on demand, a rewind discards a window in O(1), and a replay
   re-materializes it — live, by key, or under a snapshot — without losing the identity of a row the
   user has already seen.
1. **Promotion on demand.** Arena cells are ephemeral and cannot carry durable annotations
   ([`manifold.hpp:159`](apps/common/xanadu/zigzag/manifold.hpp), `ephemeralBit`). A row or a whole
   page crosses into a `xanadu::Store` through the single `promote()` crossing only when the user
   asks to annotate, after which annotations are ordinary structure operations — links, notes,
   transclusions — with no SQL involvement.
1. **Read-only, round one.** The trait's unproject half already refuses by default
   ([`projection_lens.hpp:122`](apps/common/xanadu/zigzag/projection_lens.hpp)); round one overrides
   nothing. Writing and updating rows is a separate phase that needs the trait-v2 additions of
   [`anything-to-zigzag-lens.md`](anything-to-zigzag-lens.md) §8A — which is, not coincidentally,
   exactly what a paper SQLite lens already stress-tested.

Nothing here is wired into the build. Every claim about current behaviour cites a `file:line` read
in the tree. This document is a design deliverable only — no `SqlLens` is implemented.

______________________________________________________________________

## 1. Problem Statement

A relational database is structure: a schema contains tables, a table contains columns, a query
produces rows, a row contains values, and foreign keys relate rows across tables. That is a
five-level tree plus a reference graph — more regular than a filesystem and more constrained than a
xanadoc. Yet today nothing projects it: the lens corpus covers a directory tree
([`filesystem_lens.hpp:30`](apps/common/xanadu/zigzag/filesystem_lens.hpp)), live processes
([`proc_lens.hpp`](apps/common/xanadu/zigzag/proc_lens.hpp)), and network connections
([`net_lens.hpp`](apps/common/xanadu/zigzag/net_lens.hpp)), and the worked example that already
exists — the `--lens` raster in [`xuzz_app.cpp:506`](apps/xuzz/xuzz_app.cpp) — projects files only.

Two properties make SQL a different lens problem than any of those:

- **The source is a function, not a tree.** What you project is the result of a query, and a result
  set has no natural bound. The filesystem walk terminates; `SELECT * FROM events` may not terminate
  in any useful sense. Enumeration is therefore *windowed by construction* (§5), not by an
  optimization bolted on later.
- **Identity is conditional.** A filesystem always has an inode or a path; a result row has a stable
  identity only when the projection can prove one (a primary key in the select list, a rowid in a
  single-table browse). An aggregate row has no source identity at all. The `ExternalKey` scheme is
  therefore a *ladder* of stability classes (§4), and the class travels with the row as an attribute
  so downstream consumers — especially annotations — know what a rekey can and cannot promise.

The §8A stress test in [`anything-to-zigzag-lens.md:865`](design/anything-to-zigzag-lens.md) already
ran a live SQLite database through the trait on paper and found the *read* spine generalizes cleanly
while the *write* half needs seven additions (edge properties, schema-vs-instance node kinds,
NULL/blob content, transaction brackets, multi-key rebind, `rekey()`, the reparent dimension). Round
one lives entirely on the read spine; §8 scopes the write phase against that list.

______________________________________________________________________

## 2. Evidence: What Already Exists

Read first: `projection_lens.hpp`, `projection_session.{hpp,cpp}`, `filesystem_lens.hpp`,
`arena_manifold.hpp` (`promote`, `mark`/`release`, `attach(Space)`, `insertIntoRank`/`linkChain`),
`manifold.hpp` (`DimLink`, `ephemeralBit`), `multi_store.cpp` `resolveDimension`, and
[`anything-to-zigzag-lens.md`](anything-to-zigzag-lens.md) §4–§6, §8A.

### 2.1 The trait and the session are real and landed

- `ProjectionLens` ([`projection_lens.hpp:96`](apps/common/xanadu/zigzag/projection_lens.hpp)) is
  the abstract trait: `dimensions()`, `forEachNode`, `forEachEdge` are pure virtual; the unproject
  half — `applyContentChange`, `applyAttributeChange` — has **default implementations that return
  `std::unexpected("this lens is read-only")`**
  ([`projection_lens.hpp:122-133`](apps/common/xanadu/zigzag/projection_lens.hpp)). A read-only
  `SqlLens` therefore overrides nothing in the write half; "read-only round one" is the trait's
  default state, not a limitation to add.
- `NodeContent` is `std::variant<std::monostate, std::string, double, bool, std::int64_t>`
  ([`projection_lens.hpp:70-71`](apps/common/xanadu/zigzag/projection_lens.hpp)). `std::monostate`
  mints a cell with no content span
  ([`projection_session.cpp:54-55`](apps/common/xanadu/zigzag/projection_session.cpp)) — the slot
  SQL `NULL` maps onto (§3.4). There is no blob alternative; §3.5 defines the by-budget treatment
  that avoids importing blob bytes for a browse.
- `ProjectionSession` projects a lens into a `VortexCore`'s arena — **the same arena the query/logic
  runtimes enumerate** (the project-into-bound-arena contract, lens doc §9A.2,
  [`projection_session.hpp:35-36`](apps/common/xanadu/zigzag/projection_session.hpp),
  [`vortex_core.hpp:100`](apps/common/xanadu/vortex/vortex_core.hpp)). It mints declared dimensions
  first, then a cell per node recording `keyToCell_`/`cellToKey_` both ways, attaches each node's
  attribute ranks as posward cells, then resolves edges through the map
  ([`projection_session.cpp:35-112`](apps/common/xanadu/zigzag/projection_session.cpp)).
- Dimension names bind by literal string: `resolveDimension` privileges the system set and otherwise
  calls `findOrMintDimension(name)`
  ([`multi_store.cpp:329-353`](apps/common/xanadu/multi_store.cpp)), whose own comment notes the
  canonical generics (`d.contains`/`d.seq`/`d.ref`/`d.mime`) resolve through the same mint-on-demand
  path a query uses. A lens that mints the canonical names is traversable by an unmodified VQL — the
  gate tests prove a projected directory and a hand-built slice of the same shape answer the same
  query strings ([`lens_vql_gate_test.cpp`](tests/xuzz/lens_vql_gate_test.cpp), header comment).

### 2.2 The rank model the projection must respect

Three facts about the manifold constrain the whole §3 shape, and the first draft of this document
got the first two wrong:

- **One pos, one neg per dimension.** A `CellSlot` carries `DimLink{dim,pos,neg}` entries
  ([`manifold.hpp:250-264`](apps/common/xanadu/zigzag/manifold.hpp)); `arena.link(from,dim,dir,to)`
  writes both ends and **evicts whatever either end held** — eviction is silent success, not a
  refusal ([`arena_manifold.hpp`](apps/common/xanadu/zigzag/arena_manifold.hpp), the `link` doc
  comment). deg±(c,d) ≤ 1 is the invariant (plan A2,
  [`zigzag-lenses-implementation-plan.md:47`](design/zigzag-lenses-implementation-plan.md)). The
  canonical many-children encoding is therefore **parent → firstborn on `d.contains`, siblings
  chained on `d.seq`**, exactly as `FilesystemLens::forEachEdge` documents ("parent -> FIRST child
  only … siblings chain on d.seq", `filesystem_lens.cpp`) and `ProjectionSession::project()` assumes
  for root detection ("only the first child is a containment target; the rest are sequence targets",
  [`projection_session.cpp:83-85`](apps/common/xanadu/zigzag/projection_session.cpp)). Many-to-one
  relations are encoded as a **membership rank**: the "one" side terminates the rank posward and
  each member chains posward *toward* it (§3.6) — "posward = toward the one," the direction
  `FilesystemLens` already gives `d.ref` ("points to").
- **`promote()` walks an undirected graph.** The frontier expands *both* `edge.pos` and `edge.neg`
  on every dimension (`arena_manifold.cpp`, the `discover(edge.dim)`/`discover(edge.pos)`/
  `discover(edge.neg)` loop), skipping only `d.dims`, proxies, provenance cells, and read-through
  base cells. Reachability from any cell inside a connected component is the *whole component* — the
  spec's promotion shape must be a separate component, not a clever direction choice (§6.1).
- **A promoted dimension ref must already exist in the target.** `promote` maps a *non-ephemeral*
  dim ref to itself and mints an ephemeral one as a plain content cell (`arena_manifold.cpp`, the
  `real.emplace(arena,arena)` identity branch for non-ephemeral refs): an envelope built in a
  standalone arena promotes its `d.extkey`/`d.contains` links keyed to fresh anonymous dim cells
  that are not on the target store's `d.dims` rank — internally traversable, name-unresolvable
  (§6.1's staging requirement exists to prevent exactly this).

### 2.3 SQLite is already in the tree — as an API, not a sandbox

`sqlite3` is a pkg-config dependency of the xanalogical engine ([`Makefile:277`](Makefile)), and
`swarm_catalog_index.cpp` already opens a database, applies a schema, and reads rows through the C
API ([`swarm_catalog_index.cpp:185-246`](apps/common/xanadu/swarm_catalog_index.cpp)) — behind an
impl pointer that keeps `<sqlite3.h>` out of the header, the pattern §7.2 follows. That file opens
its database `SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE` with no authorizer, so the read-only
sandbox of §7.2 is *new* posture on a familiar API — the honest claim is "the tree already links and
drives sqlite3," nothing stronger. A `SqlLens` adds **no new dependency** for the first backend.

### 2.4 The persistence crossing and the memory budget exist

- `promote(store, parent, arena, root, budget)`
  ([`arena_manifold.hpp:834-836`](apps/common/xanadu/zigzag/arena_manifold.hpp)) writes the
  reachable subgraph of one root cell into a `Store`: scratch spans acquire permascroll addresses,
  deferred scalar renderings are written, links mint after cells. It is the single legal crossing —
  a persistent op refusing an ephemeral target is enforced at `applyStructure`
  ([`manifold.hpp:159`](apps/common/xanadu/zigzag/manifold.hpp), `ephemeralBit`).
- The arena's choice-point machinery — `mark()`, `release(m)`
  ([`arena_manifold.hpp:638-642`](apps/common/xanadu/zigzag/arena_manifold.hpp)) — is the O(1)-ish
  window-toss the implementation plan's invariant A9 prescribes ("windowed materialization + epoch
  tosses, using `ExternalKey` for cross-epoch identity",
  [`zigzag-lenses-implementation-plan.md:54`](design/zigzag-lenses-implementation-plan.md)). Three
  properties bound how it can be used (§5.5): marks are a **stack** — "assumed well nested:
  `release()` and `discard()` pop" (`arena_manifold.hpp`, the marks doc comment); `release`
  truncates the contiguous mint suffix *and* replays the trail in reverse, so writes to pre-mark
  cells are undone too; and `compact()` is refused while any mark is outstanding
  ([`arena_manifold.hpp:648-669`](apps/common/xanadu/zigzag/arena_manifold.hpp)) — and compacts dead
  *link runs*, never cells, so orphaned cells are reclaimed only by a release below their mint
  point. CellRefs are dense indices — `ephemeralBit | dense` (`arena_manifold.hpp`, `refOf`) —
  **reused** after a release truncates `slots_`: a stale CellRef does not dangle, it *aliases*
  whatever now occupies the slot (§5.5's aliasing rule).
- `attach(Space)` ([`arena_manifold.hpp:297`](apps/common/xanadu/zigzag/arena_manifold.hpp)) lets
  one arena host several spaces; combined with the string-keyed map, two `SqlLens` projections (or a
  lens plus a xanadoc) share one query surface without key collisions (§4.1).
- `insertIntoRank`/`linkChain`
  ([`arena_manifold.hpp:617`](apps/common/xanadu/zigzag/arena_manifold.hpp),
  [`:632`](apps/common/xanadu/zigzag/arena_manifold.hpp)) are the shared "turn a vector into a rank"
  primitives a window materialization uses to chain `d.seq`.
- `StoreActivityLog` (`system://activity`,
  [`store_activity_log.hpp:29-30`](apps/common/xanadu/store_activity_log.hpp)) is the append-only
  record of reader walks; §5.6 reuses it to make a page's materialization durable.
- The enfilade layer is explicit that derived, replayable index products live in memory and never
  mint disk ops ([`arrayfilade.hpp`](apps/common/xanadu/enfilade/arrayfilade.hpp), the "Ruling R8"
  bullet) — a *different* R8 from the convergence's "view movement is not an edit"
  ([`store-slice-convergence.md`](store-slice-convergence.md)); this doc always says which it means.
  Both apply to windows: **paging mints cells in the arena, not operations in a store.**

### 2.5 What is missing

- **Incremental projection.** A `ProjectionSession` projects once, at construction
  ([`projection_session.cpp:15-19`](apps/common/xanadu/zigzag/projection_session.cpp)). There is no
  `refresh()` — `ProcLens::refreshFromHost`
  ([`proc_lens.hpp:67`](apps/common/xanadu/zigzag/proc_lens.hpp)) mutates lens state but nothing
  re-enumerates. A windowed lens needs exactly this: §5.2 specifies `ProjectionSession::refresh()`,
  with union-identity semantics, as the one new framework verb.
- **A session-level rewind.** `mark()`/`release()` exist on the arena, but the session's identity
  map and root list would dangle or alias past a release — keys for tossed cells must be forgotten
  too. §5.5 specifies `checkpoint()`/`rewind()` on the session, with the map-repair the raw
  `release()` cannot do.
- **Bounded promotion.** `promote()` knows nothing about `ExternalKey`s and walks the whole
  connected component (§2.2); nothing records "this row is already promoted, annotate the durable
  twin." §6 specifies a staging `promoteSubject` and the reconcile map.
- **A write path.** Deferred to phase two (§8); the §8A v2 list is its prerequisite.

______________________________________________________________________

## 3. The Source Model: A Result Set as Cells and Ranks

### 3.1 The shape — firstborn + sibling + membership ranks, subjects vs sightings

Every "one-to-many" below is the canonical encoding — parent → **firstborn** on `d.contains`,
siblings chained on `d.seq` — because a cell physically holds one pos and one neg neighbour per
dimension (§2.2). Every "many-to-one" is a **membership rank** terminating posward at the "one" side
(§3.6). The distinguishing SQL decision: **a row's identity (subject) is separated from its position
in a window (sighting)** — because a `pk:` subject can appear in two windows at once and a cell can
hold only one rank position (§3.7).

```
 [conn]
   │ +d.contains → first child; queries AND tables share one +d.seq
   ▼              sibling rank (mixed roles — d.role distinguishes them)
 [query]   d.role=sql.query      content: the SQL text
   │ +d.contains → first page     attrs: d.qhash, d.params
   ▼         (pages chain +d.seq)
 [page₀]   d.role=sql.page       attrs: d.state, d.winlo, d.winhi, d.wincount,
   │ +d.contains → first sighting     d.snapscope
   ▼         (sightings chain +d.seq)
 [sight₀]  d.role=sql.sight      attrs: d.ordinal (position in this window)
   │ +d.subject chains posward → … → [subject]   (subject terminates the rank)
   ▼
 [row₀]    d.role=sql.row        content: display key (§3.3); FLOATS — it is in
   │ +d.contains → first value        no page chain; pages reach it through
   ▼         (values chain +d.seq)    their sightings
 [v₀]      d.role=sql.val        attrs: d.column, d.type, d.null (iff NULL)

   membership ranks, each terminating posward at its "one" side (§3.6):
     sightings  → subject         on  d.subject
     rows       → their table     on  d.conforms    (schema cells head the rank)
     values     → their column    on  d.ofcol
     FK values  → target row      on  d.ref
     result cols→ their query     on  d.colof      (a query's own column list)
     subjects   → the conn cell   on  d.subjects   (the subject index, §3.7)
```

Structural decisions, each load-bearing:

1. **Firstborn `d.contains`, sibling `d.seq`, everywhere — one posward child per parent.** `conn`
   contains its first child and all children chain `d.seq` — one shared sibling rank carrying
   *queries and schema tables* (roles differ; `d.role` distinguishes). A query contains its first
   page — its result columns are *not* a second firstborn (a cell holds one pos neighbour per
   dimension; a second `+d.contains` would evict the first — the rev-1 bug in miniature) but a
   `d.colof` membership rank terminating at the query (§3.6). A page contains the first sighting; a
   subject contains its first value; a table contains the first column; siblings chain `d.seq`. This
   is the only encoding `arena.link()` realizes — anything else self-evicts (§2.2) — and it is what
   `project()`'s root detection and the VQL idiom `##/d.contains/d.seq` already assume. (The first
   draft's "widened containment to all members" was unimplementable for the same reason.)
1. **A row's values are cells, not attributes.** Attribute-rank cells are the framework's metadata
   mechanism ([`projection_session.cpp:71-78`](apps/common/xanadu/zigzag/projection_session.cpp)),
   and a column value is *content*, not metadata: the user annotates "the `amount` field of row
   1042", which must be a first-class addressable cell. Values hang on the subject's
   `d.contains`/`d.seq` chain in **first-seen union order** — two queries projecting different
   columns of one subject accumulate the union; the per-window column *order* comes from the query
   node's own `sql.col` chain, not the value chain (§3.6). Each value's ExternalKey is derived from
   `(subject key, column name)` — §4.1 — so "the `amount` of invoice 1042" is one cell across every
   query that surfaces it.
1. **Sightings carry position; subjects carry identity.** A page's row chain is a chain of
   `sql.sight` cells — occurrences, carrying only `d.ordinal` and membership in the subject's
   `d.subject` rank. The `sql.row` subject cell floats outside all page structure and holds the
   durable attributes (`d.extkey`, `d.keystab`) and the values. This is what makes §4.4's
   cross-query unification real: two windows returning the same `pk:` row mint *two sightings of one
   subject* — each window keeps its own `d.seq` chain intact (a cell can't hold two rank positions),
   and the subject's `d.subject` rank records both occurrences. Two `h:`-identical rows in one
   result are likewise two sightings of one subject — which is why there is no `d.dupcount`:
   occurrence count is the rank's length. (Sighting cells are the answer to the §8A note about
   needing annotatable *occurrences* alongside shared structure.)
1. **A query is a node.** Its `d.qhash` scopes the keyspace of every *unkeyed* row it produces
   (§4.3) — keyed rows take table-scoped `pk:`/`rid:` keys that deliberately omit `qhash` so browse
   and query modes unify on `(schema, table, key)` (§4.4).

### 3.2 Roles and the canonical/generic split

Roles ride `d.role` as attribute cells, as every lens does: `sql.conn`, `sql.query`, `sql.page`,
`sql.sight`, `sql.row`, `sql.val`, `sql.table`, `sql.col`, `sql.count` (a `COUNT(*)` page, §5.4),
`sql.annot` (the promotion envelope, §6.1), `sql.promoted` (the store-side anchor headcell, §6.3).
The generic structural ranks take their canonical meanings; SQL-specific ranks are declared via
`dimensions()` under their own names. A page is the materialized form of a window — the terms are
used interchangeably from here.

| dimension                        | kind           | meaning                                                 |
| -------------------------------- | -------------- | ------------------------------------------------------- |
| `d.contains`                     | generic (enum) | firstborn: conn→query→page→sight; row→value; table→col  |
| `d.seq`                          | generic (enum) | sibling order: queries, pages, sightings, values, cols  |
| `d.ref`                          | generic (enum) | FK referrer rank terminating at the target row          |
| `d.role`                         | generic (attr) | node kind (`sql.row`, `sql.page`, …)                    |
| `d.extkey`                       | generic (attr) | the subject's `ExternalKey`, for promotion (§6.3)       |
| `d.subject`                      | sql custom     | sighting-membership rank terminating at the subject     |
| `d.conforms`                     | sql custom     | row-membership rank terminating at a `sql.table` cell   |
| `d.ofcol`                        | sql custom     | value-membership rank terminating at a `sql.col` cell   |
| `d.colof`                        | sql custom     | result-column rank terminating at its query             |
| `d.subjects`                     | sql custom     | subject-index rank terminating at the conn cell         |
| `d.column`                       | sql custom     | a value's column name (display; `d.ofcol` is truth)     |
| `d.type`                         | sql custom     | declared type / affinity (`TEXT`, `INTEGER`, …)         |
| `d.pk`                           | sql custom     | present on a `sql.col` in the primary key, in order     |
| `d.notnull`                      | sql custom     | present on a `sql.col` declared `NOT NULL`              |
| `d.null`                         | sql custom     | present iff the value is SQL NULL (§3.4)                |
| `d.ordinal`                      | sql custom     | position: on a sighting in its window; on a `sql.col`   |
| `d.keystab`                      | sql custom     | key class on a subject: `pk`/`rid`/`hash`/`ord`         |
| `d.qhash`                        | sql custom     | hash of raw query text + typed bound params             |
| `d.params`                       | sql custom     | bound-parameter rendering (display)                     |
| `d.winlo`/`d.winhi`/`d.wincount` | sql custom     | page window bounds, typed-encoded (§5.3)                |
| `d.snapscope`                    | sql custom     | snapshot scope: `none`, `live`, or a snapshot id        |
| `d.state`                        | sql custom     | `deferred`/`live`/`stale` on pages; `stale` on subjects |

An unknown rank name mints an empty dimension rather than erroring
([`multi_store.cpp:353`](apps/common/xanadu/multi_store.cpp), the fallthrough), so every name above
is declared in `dimensions()` up front — a missing declaration is a silent-empty-rank bug, the lens
doc's §4.3.1 failure mode. All dimension minting goes through `VortexCore::findOrMintDimension`,
never `ArenaManifold::ensureDimension`: the two consult different name maps (the `d.dims` rank vs
`arenaDims_`), and mixing them mints two same-named dimensions — the same silent-empty-rank bug one
level down (`vortex_core.cpp`, `findOrMintDimension`; `arena_manifold.cpp`, `ensureDimension`). A
harder consequence for windowing: **all dimensions a lens will ever mint are declared in
`dimensions()` and minted at initial `project()`, below every checkpoint** — a rank name first seen
during `refresh()` mints a dim cell *above* the current checkpoint, which a rewind then truncates
while `findOrMintDimension` keeps the stale ref. This is a framework contract, not a SqlLens
convenience (§5.5).

Two honesty notes on this table: (a) attribute cells are minted as **text**
(`arena.makeCell(std::string_view)` in `projection_session.cpp`), so `d.ordinal`/`d.wincount` lose
R6 scalar fidelity until the framework grows scalar attribute cells — a display and comparison
limitation, not an identity one; (b) ~15 bespoke `d.*` names sit in unresolved tension with plan
invariant A3 ("struct fields bastardize dimensions"), which the landed `FilesystemLens`
(`d.size`/`d.mtime`/`d.perm`/`d.name`) and the parent lens doc's R13 ("metadata is cells on a
dimension") already endorse — this spec follows the landed convention and records the tension rather
than resolving it.

### 3.3 A subject's content: the display key

A `sql.row` cell's content is a **display key**: the human-readable form of its key subject — the pk
rendering for `pk:`/`rid:` rows, a short hash prefix for `h:` rows. It exists so a row cell has
visible text; the machine identity is always `d.extkey`. A sighting's content is the same display
key plus its `d.ordinal`, so a page reads like the result it came from.

### 3.4 NULL, empty string, and monostate

SQL `NULL` maps to `NodeContent{std::monostate}` → a cell with no content span *plus* a `d.null`
attribute. The attribute is load-bearing, not decoration: an empty-string value mints a cell whose
content is a zero-length span, and `arena.textOf` cannot tell the two apart (the arena *can* — a
NULL cell has `spanCount=0`, an `""` cell has one zero-length span — but the distinction **collapses
at promotion**: both cross as `makeCell(version, "")`, so on the durable side `d.null` is the sole
NULL marker). The rule a query can rely on, both sides of the crossing: **a NULL value cell carries
`d.null`; an empty-string cell does not.**

### 3.5 Blobs: a byte budget, not laziness-by-pretense

The `SqlValue` variant carries owned bytes (`std::vector<std::byte>`), so a blob is copied into the
backend's row buffer at fetch time — true laziness after the cursor steps on is impossible without a
re-query, and re-querying needs a rowid only `pk:`/`rid:` rows have. Honest treatment: **blobs are
fetched up to a per-value byte budget** (`SqlBackend` declares it, e.g. 64 KiB). Within budget, the
value cell's content is a size-only rendering (`blob(4096 B)` — no digest, since a digest requires
the eager read anyway and the bytes live in the backend buffer until the cell is opened or
promoted); the bytes ride the buffer for the annotate/promote path. Over budget, the cell renders
`blob(… B, truncated)` and bytes are re-fetchable **only** for `pk:`/`rid:` rows, by point-select or
`sqlite3_blob_open` — for `h:` rows the over-budget blob is simply not viewable, and the cell says
so. Two corollaries the key grammar respects (§4.1): the `b:` key digest is computed over the
*complete* column value via the fetch-time pointer — **budget-independent**, or rekeys change with
the budget; and a sort-key component that is a blob degrades the window to ordinal (a bound that
can't be cheaply retained can't rebind a keyset replay). Bytes enter the permascroll only at promote
time — as scratch content on the envelope's value cells that `promote()` spools into the permascroll
itself (§6.2) — the first moment they legitimately may (convergence R8).

### 3.6 Membership ranks for schema and references

In **browse mode** (§5.1) the lens also projects the schema, because schema is what makes rows
explainable: `conn` → `d.contains`/`d.seq` → a `sql.table` cell per table/view →
`d.contains`/`d.seq` → a `sql.col` cell per column (content: name; attributes: `d.type`, `d.pk`,
`d.notnull`, `d.ordinal`). The many-to-one relations become ranks whose members chain **posward
toward the "one" side** — a uniform reading ("posward = toward the one") that matches
`FilesystemLens`'s `d.ref` (symlink `+d.ref→` target). In the listings below, `→` is posward:

- **`d.subject`**: each sighting chains posward toward its subject —
  `sight₃ +d.subject→ sight₂ +d.subject→ sight₁ +d.subject→ subject`; a sighting reaches its row by
  walking posward, the subject enumerates its sightings by walking negward.
- **`d.conforms`**: `row₃ → row₂ → row₁ → table` — a row finds its table by walking posward
  `d.conforms` to the `sql.table` headcell; the table enumerates conforming rows negward. This is
  the §8A "schema vs instance" node-kind gap expressed as structure rather than a field: conformance
  is traversable, which is the point.
- **`d.ofcol`**: `v₃ → v₂ → v₁ → column` — the same pattern, and its headcell is the **schema
  column** (`t:<stable>:c:<name>`) whenever the value is attributable — including in query mode,
  which mints the schema column cell for attributable result columns even though it projects no full
  schema. Heading at the schema column is what lets one `v:`-keyed value join the ranks of several
  queries: each query's own `sql.col` cells serve column order and display, while membership is
  anchored at the shared, stable column. For an unattributable value the rank terminates at the
  query's own result-column cell — and when two unattributable columns claim one value, the most
  recent enumeration wins, mirroring the value-union rule (§3.1). `d.column` remains as the cheap
  display attribute (and the only column identity when attribution fails).
- **`d.ref`** (foreign keys): `fkval₃ → fkval₂ → fkval₁ → target row` — the referenced row
  terminates the rank of its referrers, where a referrer member is the FK *value* cell (annotating
  "this reference" is then annotating the member cell — an annotatable occurrence, the thing §8A
  wanted edge properties for). A referrer resolves its target by posward walk; a row enumerates
  referrers negward. Emitted **only when both ends are already materialized** — an edge naming an
  unprojected node is skipped silently
  ([`projection_session.cpp:88-90`](apps/common/xanadu/zigzag/projection_session.cpp)); chasing a
  reference across pages is a navigation gesture that *requests* a window (§5.4), never a
  projection-time obligation. Many members on one rank is not a deg± violation: each member holds
  its own pos/neg pair.
- **`d.colof`**: `col₃ → col₂ → col₁ → query` — a query's own result-column list terminates at the
  query node, emitted in select order, so the rank *is* the per-window column order the value
  chain's union order defers to (§3.1). Pages and columns live on different ranks precisely because
  a query can hold only one `d.contains` firstborn (§3.1).
- **`d.subjects`**: every materialized subject chains posward toward the `conn` cell — the subject
  index. Subjects float outside all page structure (§3.7), so this non-structural rank is what makes
  them enumerable without turning every one into a structural root (root election is lens-declared,
  §3.7). The index keeps **stale** members too — it enumerates "every subject ever seen," and
  consumers filter `d.state` (a vanished subject's membership is never retracted because nothing
  re-enumerates it; the seen-history reading is the useful one anyway).

In **query mode** the query node mints `sql.col` cells for the result's own column list (from cursor
metadata, §7.1) on its `d.colof` rank in select order. A *row's* `d.conforms` membership is absent
where the backend cannot attribute it to exactly one base table — an honest omission, not a `NULL`
rank.

### 3.7 Why sightings, not "the row in the page"

The `pk:` unification of §4.4 promises a row reached by two queries is *one subject*. A naive
encoding puts the subject cell in the page's `d.seq` chain — but a cell holds one rank position, so
the second materialization would either silently evict it from the first page's chain (splicing page
A's `d.seq` into page B's) or skip re-chaining (page B missing a row it returned). Occurrence cells
dissolve the conflict: each window owns its sightings outright, subjects float, and the `d.subject`
rank is the join — an encoding that is *more* traversable, not less (a subject answers "where has
this row been seen"). It is the same subject/occurrence split the lens doc's §8A asked for in edge
form, applied to rows.

Floating has one side effect the design handles rather than ignores: root detection counts only
`d.contains`/`d.seq` posward targets, so every subject is structurally a root — which would flood
`roots()` ("natural entry points") with every row ever seen. Two fixes compose: subjects join the
`d.subjects` index rank terminating at `conn` (§3.6 — a membership rank, not containment, so it
doesn't re-enter page structure), and **root election becomes lens-declared**: the trait grows
`forEachRoot()`/`roots()` with the structural rule as default, and `SqlLens` declares the `conn`
cell. Consumers that want rows ask the index rank, not `roots()`.

______________________________________________________________________

## 4. The ExternalKey Ladder: Identity When the Source Only Conditionally Has One

### 4.1 The encoding

Two grammars share the `sql:` prefix and must not be confused: the **lens URI** a user types
(`sql:sqlite:///abs/path.db` — browse, §7.4) and the **ExternalKey** a node carries:

```
ExternalKey (sql) ::= "sql:" <backend> ":" <dbid> ":" <subject>
  <backend> ::= "sqlite" | "pg"                           -- §7
  <dbid>    ::= "f:" <sha256(realpath of db file)>        -- sqlite: exactly one file;
                                                        --   ATTACH is denied (§7.2)
              | "s:" <conninfo-hash>                      -- pg: hash of normalized
                                                        --   host:port/db + user (§4.5)
  <subject> ::= "t:" <stable> ":" "pk:" <pk-rendering>    -- declared-PK row (subject)
              | "t:" <stable> ":" "rid:" <sql-value>      -- implicit-rowid row ("i:<n>")
              | "q:" <qhash> ":" "h:" <rowhash>           -- hashed row (subject)
              | "q:" <qhash> ":" "ord:" <ordinal>         -- positional row (reserved)
              | <row-subject> ":" "v:" <colname>          -- a value cell
              | "t:" <stable>                             -- table/view
              | "t:" <stable> ":" "c:" <column>           -- schema column
              | "q:" <qhash>                              -- query
              | "q:" <qhash> ":" "c:" <n>                 -- result column
              | "q:" <qhash> ":" "p:" <window-id>         -- page
              | "q:" <qhash> ":" "p:" <window-id> ":" "@:o:" <ordinal>   -- sighting
  <row-subject> ::= the pk:/rid:/h:/ord: alternatives above
  <colname>   ::= the BASE column name when attributable (origin_name /
                  PQftablecol, §4.3), else the result-column name --
                  alias-insensitive so one field is one key; on a v:
                  collision WITHIN one enumeration (an attributable
                  'amount' and a computed '... AS amount' in the same
                  result), the colliding unattributable cell falls back
                  to positional keying 'v:@<n>'
  <stable>  ::= <schema-qualified, canonically-cased table name>   -- §4.4
  <qhash>   ::= sha256 over (raw statement text || typed bound params)
              -- raw text, NOT sqlite3_normalized_sql: that API folds literals
              -- into '?', so WHERE x=5 and WHERE x=7 would collide on one
              -- keyspace; literal-vs-param spellings MUST differ since they
              -- can name different result sets (§4.4)
```

The typed renderings, all normative (they feed key *equality*):

```
<sql-value> ::= <tag> ":" <payload>
  tag "n" NULL | "i" int64 decimal | "f" IEEE-754 bits as 16 hex
    | "t" UTF-8 text, percent-encoded | "b" sha256 hex of the COMPLETE
      column value (fetch-time pointer — never the budget-truncated
      buffer, §3.5)
<pk-rendering> ::= <sql-value> ("," <sql-value>)*   -- declared PK order:
                                                   -- table_xinfo pk positions /
                                                   -- conkey attnums (§7)
rowhash input  ::= <sql-value>* in select order, joined by U+0000
                 (column names excluded: qhash already scopes the keyspace)
<window-id>  ::= "k:" <sql-value> ("," <sql-value>)*  -- the bound the window
                 was materialized against (the continuation sort key) --
                 known pre-fetch, so a stub's key IS its page's key
              |  "o:" <ordinal>      -- ordinal window: start offset
```

Two encoding rules the nested grammar makes normative:

- **Percent-encoding reserves**: `t:` payloads encode at minimum `%`, `:`, `,`, `@`, and NUL — those
  bytes are productions' delimiters, so payload bytes must never carry them or `q:…:p:k:t:…:@:o:…`
  mis-parses.
- **The sighting key is positional** — `(query, window, window-ordinal)`, not `(window, subject)`:
  two sightings of one subject in one window (a join fan-out, two `h:`-identical rows) get distinct
  cells, which is what makes the `d.subject` rank's length the true occurrence count (§3.7). A
  sighting's cross-epoch identity is not load-bearing — annotations target subjects and `v:`-cells,
  never sightings — so positional keying costs nothing.

Keys stay bounded: a blob contributes its digest, not its bytes. Composite keys are first-class —
`<pk-rendering>` is a tuple — because the common case is multi-column. A value cell's key derives
from `(subject, column)` — `v:` — which is what makes a field annotatable independent of the window
that surfaced it (§6.4).

### 4.2 The ladder, strongest to weakest

1. **`pk:` — declared-key.** The result is attributable to exactly one base table and includes every
   declared PK column (an `INTEGER PRIMARY KEY` alias counts — an IPK is stored data and survives
   `VACUUM`). Survives refresh, reorder, update of other columns, page moves.
1. **`rid:` — implicit rowid.** Single-table browse of a table with no declared PK but a rowid — the
   generated SELECT asks for the first of `rowid`/`oid`/`_rowid_` not shadowed by a declared column
   (all three shadowed → `h:`). The rowid is volatile twice over — `VACUUM` renumbers it, and
   deletion of the `max(rowid)` high-water lets a later insert *reuse* it without any VACUUM — so it
   gets its own class rather than masquerading as `pk` (this resolves rev 1's U-S2).
1. **`h:` — hashed.** No provable key: joins, grouped selects, views, a `WITHOUT ROWID` table whose
   PK columns aren't all in the select list (a `WITHOUT ROWID` table always *has* a declared PK —
   the gap is the projection, not the table), keyless rowid tables, missing column metadata. The key
   is the row's content hash over the typed encoding above. Survives reorder and
   refresh-that-didn't-touch-this-row; an update mints a new key.
1. **`ord:` — positional, reserved.** Not emitted in round one; reserved for a backend that cannot
   buffer a row long enough to hash it (a future streaming cursor). If it is ever emitted, only
   `d.keystab=ord` saves it from masquerading as stable.

`d.keystab` carries the class on every subject cell (`pk`|`rid`|`hash`|`ord` — the attribute
spelling; the key grammar abbreviates `hash` as `h:`); it is the attribute a rekey or refresh
reconciliation consults first, and the attribute the annotation contract reads (§6.4).

### 4.3 Eligibility is per query, and cells model subjects (sightings model occurrences)

The backend reports, per result column, its base-table attribution and key membership (§7.1
`SqlColumn`): SQLite uses `PRAGMA table_xinfo` (whose `pk` field carries composite-PK *position* —
`table_info` lacks generated/hidden columns and positions) plus, where the build provides them,
`sqlite3_column_table_name`/`origin_name` (`SQLITE_ENABLE_COLUMN_METADATA` — a **link probe**, not
an `#ifdef`: defining it against a lib that lacks the symbols fails at link); PostgreSQL uses
`PQftable`/`PQftablecol` attnums joined to `pg_constraint` `conkey` (`contype='p'`). A result gets
`pk:` keys when **exactly one** base table qualifies — an equijoin on two tables' PKs satisfies the
test twice and falls to `h:` — and duplicate keys detected at emission (a join fan-out, or two
`h:`-identical rows) collapse to one subject with *two sightings* (§3.7): the sighting count is the
occurrence count, so the projection still "shows what the query returned" while identity stays
subject-level. Injectivity is not claimed statically — it can't be proven — the rule is "exactly one
qualifying table → `pk:`; detected duplicates share the subject." When column metadata is
unavailable the degradation is per-mode, not global: **browse keeps `pk:`** (the lens generated the
query and knows its table — `table_xinfo` alone suffices), **query falls to `h:`**.

### 4.4 What unifies and what breaks each rung

- **Cross-query unification.** `pk:`/`rid:` keys name `(stable, key)` — never `<qhash>` — so a row
  reached by browse and by two different queries is *one subject*, reached through each window's own
  sightings, with one annotation target. This is the deliberate payoff of the key grammar.
- **Implicit-rowid reuse (`rid:`).** `max(rowid)+1` assignment means a deleted `max(rowid)`
  high-water is re-issued to a later insert; `VACUUM` renumbers wholesale. A `rid:` key asserts
  position, not history — `d.keystab=rid` is the honest label and the annotation contract treats it
  accordingly.
- **Key/hashing column updates.** An `UPDATE` to a PK or any hashed column changes the key — by
  design; the promoted twin keeps its `d.extkey` as provenance and refresh reconciliation is what
  notices "same row, new key" (policy, not mechanism; U-S3).
- **Schema-qualified names (`<stable>`).** PG tables resolve to `<schema>.<table>` via
  `PQftable`→`pg_class.relnamespace`/`relname` (also fixing `search_path` aliasing — `users` and
  `public.users` must produce one key); SQLite uses the canonical case from `sqlite_schema`, so
  `FROM Invoice` and `FROM invoice` key alike.
- **`qhash`.** `sha256(raw statement text || typed bound params)` — *raw* text, deliberately:
  `sqlite3_normalized_sql()` folds literals into `?`, which would give `WHERE x=5` and `WHERE x=7`
  the same `qhash` and silently merge two different result sets into one keyspace. Normalized SQL
  may be kept for *display* dedup; the hash needs only determinism — "identical text + identical
  params share a keyspace," nothing stronger.

### 4.5 What breaks the whole ladder

- **`ATTACH`ed databases.** The `<dbid>` grammar names one file; a connection attaching several
  breaks it. Resolution: **`ATTACH` is denied by the authorizer** (§7.2) — the single-file keyspace
  is enforced, not assumed. Temp tables live in the `temp` database and never mint `pk:`/`rid:` keys
  (they are `h:` — nothing about them is durable).
- **Path aliasing.** `realpath(3)` (not `weakly_canonical` alone) resolves symlinks so two spellings
  of one file hash identically; PG endpoints alias similarly (unix sockets vs TCP to one cluster) —
  `<conninfo-hash>` normalizes what libpq reports, not what the user typed. Including the **user**
  in the hash is a deliberate choice: `same database, different credentials → different dbid`,
  because the privilege realm differs — two roles may see different rows under the same name, and
  their annotations must not unify silently.
- **Text rendering is not a key input.** Floats (`-0.0` vs `0.0` — SQL-equal but bit-distinct, and
  they hash differently: accepted, and stated), dates, `money` — text renderings vary by
  platform/GUC. The typed encoding is canonical; on PG the backend pins session GUCs (`DateStyle`,
  `IntervalStyle`, `extra_float_digits`, `bytea_output`) at connect, and `float4`/`float8` text is
  parsed to `double` then bit-encoded (`NaN` uses its canonical bit pattern — PG can return one;
  SQLite stores NaN as NULL so it can't).

______________________________________________________________________

## 5. Windowing and Replay

### 5.1 Two modes, two paging mechanisms

| mode     | query source                         | window mechanism                                     |
| -------- | ------------------------------------ | ---------------------------------------------------- |
| `browse` | lens-generated `SELECT … ORDER BY …` | **keyset**: tuple predicate `WHERE (cols) > (:sk)`   |
| `query`  | arbitrary user SQL                   | **ordinal**: held cursor forward; replay by `OFFSET` |

Key class is orthogonal — decided per §4.3 for *both* modes (`pk:`/`rid:` wherever eligible, `h:`
otherwise). In browse mode the lens generates `SELECT <cols>, <sort key>` over one table, appending
the sort columns **plus the PK** to `ORDER BY` so the sort key is unique, then paginates by keyset —
row-value tuple comparison `(c1,…,ck) > (?,?,…)`, legal in SQLite ≥ 3.15 and in PG. Five details the
generated SQL owns:

- **`NULLS FIRST|LAST` is always emitted explicitly** (SQLite defaults NULLs first, PG NULLs last —
  `d.winlo`/`d.winhi` must mean the same thing in both; the syntax needs SQLite ≥ 3.30, so the
  stated floor bumps to it or the backend capability-probes and falls back).
- **A nullable or mixed-direction sort** uses the OR-of-prefix-equalities expansion the backend
  generates — `OR_{i=1..k} [ (∧_{j<i} c_j IS [NOT DISTINCT FROM] :v_j) ∧ after_i(c_i, :v_i) ]` with
  `after_i` chosen per column's direction and null-placement — rather than a bare tuple compare,
  which yields NULL (≈excluded) on any NULL bound and can't express `DESC`. Browse `ORDER BY` is
  pinned `ASC` in round one so the expansion stays linear-in-k; ordinal windows remain the fallback
  when a backend can't express it. The same expansion serves both bounds of the keyset `BETWEEN`
  replay (§5.3).
- **Re-selected sort columns get reserved aliases** (`<col> AS __sql_sort_<i>`,
  `rowid AS __sql_rid`) — a sort column already in `<cols>` would otherwise appear twice in the
  result and mint a phantom second `v:` cell; alias-prefixed columns are excluded from `v:`
  derivation and serve only bound extraction.
- **An implicit rowid** is asked for by alias — the first of `rowid`/`oid`/`_rowid_` not shadowed by
  a declared column (`SELECT *` omits implicit rowids; `table_xinfo` detects the shadow, not the
  name).

Query mode wraps nothing into the user's text: live windows read the held cursor forward (which
preserves the query's own `ORDER BY` perfectly); a *replay* of an ordinal window re-prepares
`SELECT * FROM (<sql>) LIMIT :n OFFSET :m` — with the `;`-stripping, single-statement tail check,
and the caveat that `ORDER BY` inside a FROM-subquery is not contractually guaranteed to order the
outer result (in practice preserved when the outer query has no reordering operator; stated, not
hidden).

### 5.2 `ProjectionSession::refresh()` — the one new framework verb

A windowed lens is stateful: `SqlLens` owns the backend and a buffer of materialized pages;
`forEachNode` enumerates *the current materialized state* — structural cells plus every sighting,
subject, and value of every `live` page. To open page k, the app asks the lens to fetch page k into
its buffer (`materializePage`, §5.4 — the app holds a non-owning `SqlLens*` captured before the
session takes ownership, since `session.lens()` is const), then calls a new session method:

```cpp
/// Re-enumerate the lens and reconcile the arena with it:
///  - mint cells for keys not yet in the map (same key -> same CellRef);
///  - restate NodeContent on surviving keys when the re-emitted content
///    differs (setContent, trailed like any write) -- a pk:/rid: subject
///    whose non-key column changed must display the new value, or the
///    refresh-faithful claim lies;
///  - re-assert edges (link() overwrites in place), AND remove stale ones:
///    for each re-enumerated cell, unlink() every link in its CURRENT run on
///    the structural dims not re-emitted (snapshot the run first; link() may
///    relocate it) -- the tombstone pass that empties a page's chain and
///    severs a vanished tail row. Diffing the live run, not a recorded edge
///    set, is deliberate: a recorded set mutates post-checkpoint and cannot
///    be restored by mint-order truncation, so a trail-resurrected link
///    would ghost forever;
///  - rewrite attribute cells whose re-emitted value changed (the old attr
///    cell is orphaned until the next rewind -- cells are reclaimed only by
///    release(), never by compact());
///  - for keys in the map absent from this enumeration, write the attributes
///    the lens's staleAttributes(key) returns (default {}; SqlLens answers
///    {{"d.state","stale"}}) -- the framework never hardcodes a rank name;
///  - recompute roots() over the keys present in THIS enumeration only --
///    stale cells persist in the map but are not roots;
///  - atomicity: refresh() wraps its body in an internal mark() -- the
///    innermost, legal even under outstanding scroll checkpoints -- so a
///    mid-enumeration fault releases cleanly rather than half-applying.
/// Returns the count of newly minted node cells. Lens-level failures are the
/// caller's (materializePage returns expected); a fault inside the mint loop
/// is an engine bug surfaced via expectWritten.
[[nodiscard]] std::size_t refresh();
```

`refresh()` is framework code, not lens code: the same loop serves `ProcLens::refreshFromHost` and
any future live lens — the current one-shot `project()` becomes the first `refresh()`. The
`staleAttributes` hook lands on the trait as an optional virtual with a `{}` default, so
`FilesystemLens`/`ProcLens`/`NetLens` need no change:

```cpp
/// What attributes a projected cell should carry when its key is absent from
/// a refresh() enumeration. Framework-invoked; lens vocabulary.
[[nodiscard]] virtual std::vector<std::pair<std::string, std::string>>
staleAttributes(const ExternalKey & /*key*/) const { return {}; }
```

Union-on-identity is what makes replay safe: a subject seen in page 1 and re-fetched in page 5 keeps
its cell — `keyToCell_` hits, no re-mint — so anything the session maps stays valid (and the
`sightings` model means the second page's chain doesn't fight the first's — §3.7).

### 5.3 A page is a cell that knows how to re-fetch itself

Each page cell carries its replay parameters — `d.winlo`/`d.winhi` (typed `<sql-value>` encodings,
not display strings — rebinding needs the type: `'1042'` ≠ `1042` under SQLite affinity),
`d.wincount`, `d.qhash`, `d.snapscope`, `d.state` — so a page is self-describing. The bounds have
one meaning on stubs and pages alike: **`d.winlo` is always the exclusive bound the window starts
after** (on a stub, the continuation sort key; on a materialized page, the bound it was fetched with
— the same value, which is what makes the `<window-id>` key `k:`-bound stable across the
stub→materialize transition); **`d.winhi` is the last row's sort key**, known only after the fetch
and absent on stubs. The lens also keeps the typed `SqlValue`s and each window's row-key list in its
page buffer; the attributes are the durable/display form of the same data. The three replay
semantics, all against the recorded parameters:

1. **Live replay.** Re-execute the window fetch against current data. Cheapest; sees inserts and
   deletes; the honest default.
1. **Keyset replay** (browse mode only). Re-prepare the recorded window SQL with `winlo`/`winhi`
   bound — `WHERE (sort cols) > (:lo) AND (sort cols) <= (:hi)` in row-value expansion (legal in
   both engines; the exclusive lo mirrors how the window was fetched, and the appended PK makes the
   tuple unique so boundary rows re-match exactly). Inserts whose sort key lands *inside* the range
   appear — honestly, not hidden: the key list in the page buffer tells replay-originals from
   inserts, and `d.wincount` stays the original count. A shortfall means rows were deleted and shows
   as fewer live rows, not as wrong ones.
1. **Snapshot replay.** Inside one consistent-read scope every window reads one version — backward
   paging re-reads bytes already read, deterministically. The mechanics differ honestly by backend
   (§7.2, §7.3): SQLite gets it only in **WAL mode** (a held read transaction pins a snapshot at
   first read — `BEGIN DEFERRED` itself pins nothing; `journal_mode` is probed at open, before the
   authorizer, and cached; the read form is also allowed post-authorizer under the arg2-NULL policy,
   so a fresh check costs nothing); rollback-journal databases get no snapshot replay
   (`d.snapscope=none`) — and the writer-blocking caveat applies to **any held read position**, not
   just snapshots: a `fetchNext` cursor idling mid-window holds a SHARED lock that denies every
   writer's commit on a rollback journal, so live cursors there are stepped to completion or dropped
   between gestures (the idle policy is U-S1). `SQLITE_SCHEMA` from concurrent DDL maps to
   `d.state=stale` + a `refresh()`; `PRAGMA data_version` is the cheap "did another connection
   commit" probe; `sqlite3_snapshot_*` (build probe) is the round-two transfer-capable option.

### 5.4 Deferred pages and the stub horizon

`forEachNode` cannot yield a stub per page — without an eager `COUNT(*)` the page count is unknown,
and computing it is a scan the user didn't ask for. The enumeration is therefore a **bounded stub
horizon**: the lens emits one `sql.page` cell per materialized window *plus one trailing `deferred`
stub* carrying the next window's `d.winlo` (the last seen sort key, or the next `OFFSET`).
Materializing a stub is `materializePage(pageKey)` on the lens followed by `session.refresh()`; the
trailing stub is re-emitted further along each refresh — under a *new* `<window-id>` key — until a
fetch comes up short, at which point it is absent: the end of the result is represented by *no
stub*, not a marker. A `count(*)` request is itself a replayable page of role `sql.count`, minted on
demand.

Stub lifetimes split honestly by when they were minted: the **initial** trailing stub is minted at
`project()`, below every checkpoint, so a rewind restores its `d.state` to `deferred` via trail
replay; a stub emitted by a later `refresh()` is minted *above* existing checkpoints and is **tossed
wholesale** by any rewind that pops past its mint point (the horizon re-emits it). The ordering rule
that keeps this coherent: materialization is *fetch → refresh → then checkpoint*, so each new
trailing stub sits below the checkpoint its window's rows will be tossed by. Page-cell state is only
ever `deferred`/`live`/`stale` — there is no `tossed` value, because a tossed page's cells are gone
and the stub either reverted or is re-emitted fresh.

### 5.5 Rewind: `checkpoint()`/`rewind()`/`pin()` — a stack, with map repair

`mark()`/`release()` are strictly LIFO and `release` is contiguous truncation plus trail replay
(§2.4): the session's rewind must be a stack too, and it must repair what the arena doesn't own.

```cpp
/// One rewind boundary. High-waters let rewind() forget exactly what was
/// minted after the mark: keyToCell_/cellToKey_ entries (by mint order),
/// roots_ entries, and the checkpoint stack itself. Dimensions are NOT
/// checkpointed -- they must only ever be minted via findOrMintDimension at
/// project() time (§3.2).
struct Checkpoint {
  ArenaManifold::Mark mark;
  std::size_t keyHighWater;
  std::size_t rootsHighWater;
};

[[nodiscard]] Checkpoint checkpoint();

/// Register/withdraw a pin. A pinned key's cell (or any cell minted with it —
/// pin granularity is the checkpoint that contains it) blocks any rewind that
/// would toss it.
void pin(const ExternalKey &key);
void unpin(const ExternalKey &key);

/// Pop every checkpoint above @p cp (discard(), no undo), then release(cp.mark):
/// tosses all cells minted after it, truncates the identity maps and roots_ to
/// their high-waters, and reverses trail entries (a stub's d.state attr reverts
/// to deferred; a link re-asserted since is restored; a stale-marked row
/// un-stales). Then invokes the lens's windowed-side discard
/// (WindowedLens::discardMaterializedAfter, with a no-op default): the lens's
/// page buffer is off-arena state release() cannot touch, and without this the
/// next refresh would re-emit the buffered rows and resurrect the tossed
/// window -- a discard is the only thing that makes a toss a toss.
/// Refuses -- naming the pinned key -- if anything pinned was minted after
/// cp: there is no selective skip, because release() is contiguous
/// truncation. Refuses naming the foreign mint if the mint delta above cp
/// exceeds the session's logged mints -- i.e. something else minted into this
/// arena (a solver's answer cells, another lens's attach(Space) mints): a
/// checkpoint must not straddle writes it can't repair, so the caller
/// quiesces such activity or accepts the refusal.
std::expected<void, std::string> rewind(const Checkpoint &cp);
```

The rules the design lives by, each forced by the machinery:

- **Stack discipline.** `rewind(cp_k)` first discards `cp_{k+1..N}` then releases `cp_k` — passing a
  stale mark to `release()` while newer marks are outstanding is vector-growth corruption (a mark
  recorded with larger sizes than the truncated vectors makes `resize` *fabricate* slots). Scroll
  checkpoints are also **outermost**: taken and released only while no solver choice point (`mark()`
  from vlog/vprolog evaluation) is outstanding — marks are one shared stack.
- **CellRef aliasing.** Dense indices are reused after a release, so a CellRef held across a rewind
  does not dangle — it *aliases* a different cell. Cross-epoch identity is `ExternalKey` only: every
  consumer re-resolves `cellFor(key)` after `refresh()`/`rewind()` rather than caching a `CellRef`,
  and the session may expose an epoch counter for cheap staleness checks.
- **Pin = refuse, never skip.** A pinned key minted after `cp` makes `rewind(cp)` return
  `std::unexpected` naming it; the caller may rewind to a deeper checkpoint or promote the pinned
  subject first (§6).
- **Cost model, honestly.** With K scroll checkpoints outstanding — the designed steady state, not
  an anomaly — `trail()` fires for every write to any cell older than the innermost floor, copying
  the slot *and* its link/content runs to the arena tail, and `compact()` stays refused for the
  deque's whole life. The bound is "touched-old-cells × run size per refresh," which is small here
  precisely because projected cells are written only during materialization and stale-marking — each
  `d.state` flip mints one orphaned attr cell plus one trailed run, reclaimable only by the next
  rewind past its mint (never by `compact()`, which reclaims runs, not cells). Ad-hoc checkpoints
  taken for other purposes should be short-lived for the same reason.
- **Overlay boundary.** A checkpoint must not straddle writes to non-projected cells — base-overlay
  shadows, attached-space proxies, quote occurrences, or foreign mints (the refusal above) — because
  `release()` un-shadows/un-mints those along with the window. The mint-order log suffices for the
  two identity maps (attribute cells were never mapped — `cellToKey_` records node cells only), but
  any other `CellRef`-keyed state the session grows must get the same high-water repair.

**Scrollback policy (honest bound and cost).** Forward scrolling accumulates mints until a rewind —
a deque of K checkpoints gives K pages of backward reach; scrolling past the horizon is a *slide*:
note the current page key, `rewind` to the horizon floor, re-materialize from there — an O(K) slide
costing ≤ K page fetches, amortized over K pages of forward travel, and the deque is rebuilt as
pages re-materialize. Backward history is sacrificed, never skipped around; re-fetching is a replay
(§5.3). Pinned pages stop a slide at their checkpoint (refuse-and-name).

### 5.6 Recording the walk

Each materialization gesture — not each fetch — can append the page's replay parameters (query hash,
window bounds, snapshot scope, key class) to `system://activity`
([`store_activity_log.hpp:29`](apps/common/xanadu/store_activity_log.hpp)) as a visit, coalesced per
navigation gesture so a scroll doesn't spam the walk history. Optional in round one; the hook is
cheap because a page is already self-describing (§5.3).

______________________________________________________________________

## 6. Promotion on Demand: The Annotation Crossing

The arena projection is ephemeral — `ephemeralBit`-tagged refs that a persistent `SetLink` refuses
([`manifold.hpp:159`](apps/common/xanadu/zigzag/manifold.hpp)). A row a user wants to annotate must
cross into a `Store`, and the only legal crossing is `promote()`
([`arena_manifold.hpp:834-836`](apps/common/xanadu/zigzag/arena_manifold.hpp)). Nothing in the lens
promotes by itself: **promotion is a user gesture**, which is what keeps convergence R8 intact —
browsing and replaying mint zero ops anywhere.

### 6.1 The staging envelope: two crossings, both required

`promote()` walks *every* link of every reached cell, both directions (§2.2): from a `sql.row` cell
the walk climbs negward `d.contains` to sightings→page→query→conn, runs `d.seq` to every sibling,
descends `d.conforms`/`d.ofcol`/`d.ref`/`d.subject` to the schema and every sighting — the whole
projection is one connected component, so `promote(rowCell)` would write *the whole materialized
database* to the user's store (or exceed `PromotionBudget` and return `std::nullopt`). Envelope
cells linked *into* the projection don't help: the walk follows them right back out.

So `promoteSubject(key)` builds a **staging envelope**: a small, *disconnected* subgraph — a
`sql.annot` envelope cell holding copies of the subject (its display-key content, its value cells
with `d.column`/`d.type`/`d.null`/`d.extkey`/`d.keystab`, or for `promotePage` the window's subject
copies in window order) — and promotes that. But the envelope has a second, subtler requirement
(§2.2, third fact): its **dimensions must already exist in the target store**. `promote()` maps a
non-ephemeral dim ref to itself and mints an ephemeral one as a nameless content cell — an envelope
built in a standalone arena lands its `d.extkey`/`d.contains` links keyed to anonymous dim cells off
the store's `d.dims` rank: internally traversable, but `resolveDimension("d.extkey")` in the
annotation store names a different cell, so the §6.3 rebuild would find nothing. The envelope is
therefore minted in a **dedicated staging arena overlaid on the annotation store's manifold**, with
every rank it uses declared in that store *at first promote* — minted lazily in the same gesture's
op batch, not at session open (a session that never promotes must mint zero ops anywhere) —
`findOrMintDimension` then resolves through the base (`baseDimensionNamed`) to non-ephemeral refs
`promote()` maps identically. (The trait-v2 alternative — a `promote()` option resolving dims by
name in the target store — is noted; the overlay needs no signature change.)

Three rules make the two-arena story safe:

- **Copies are content-level, never refs.** An ephemeral `CellRef` is `ephemeralBit | dense` with no
  arena tag — a projection-arena ref handed to `staging.link()` silently *aliases* whatever staging
  cell holds that dense index, and a scratch `PrimediaSpan` copied across reads the wrong arena's
  scratch buffer. The copy is `staging.makeCell(projArena.textOf(…))` /
  `staging.makeScalarCell(bits)` — text and scalar payloads only. (§5.5's aliasing rule, one arena
  removed.)
- **The staging arena marks before each envelope and releases after `promote()`** — its own mark
  stack is independent of the projection arena's, so per-gesture rollback is O(1) and envelope
  debris never accumulates.
- **The base is re-read after each promote.** The overlay's contract forbids mutating the base while
  it's read through, and `promote()` writes ops into exactly the manifold the staging arena overlays
  — the staging arena's base `Manifold` is advanced (or re-folded) after each gesture so the next
  promote and the §6.3 rebuild walk current state, not a stale read.

The durable twin is a **frozen snapshot**: a new hypertime authority whose content does not track
later source updates — its tie to the live subject is `d.extkey` provenance and the reconcile map
(§6.3), and §6.4's per-class staleness story covers the three-way split (twin, source, live cell).
`d.clone` is deliberately *not* used: the arena cell isn't the authority — the database row is — and
an ephemeral master for a persistent member is exactly the `ephemeralBit` refusal.

### 6.2 `promoteRow` / `promotePage`

Both gestures run the same `promoteSubject(key)` path with a different copy set: a subject's value
chain, or a page's subjects in window order. The envelope root is minted first, so
`Promoted.cells[0]` is the durable envelope; the session then walks the durable envelope —
`envelope →d.contains→ first subject →d.seq→` — to learn each subject twin's real `CellRef` (the
`cells` vector is promote's BFS order, not the session's). Blobs under a promoted subject ride the
envelope as **scratch content** on the copied value cells — `promote()` itself spools scratch spans
into permascroll addresses (the mechanism §2.4 already names), so no byte-import API is needed and
the bytes land atomically with the rest of the crossing (§3.5). `d.null` travels on promoted value
cells and is the *sole* NULL marker on the durable side (§3.4) — annotation code must read it, not
the cell content.

### 6.3 Idempotent promotion: the reconcile map and the `d.promoted` anchor

`promote()` knows nothing about `ExternalKey`s, so the session keeps a third map:
`ExternalKey → (Store*, opIndex)` — a *local* op reference, not `GlobalOpRef`, because a private
never-published annotation store has no seal to globalize under (`GlobalOpRef` needs `sealedAs`);
globalization happens lazily if the store is ever published. The gesture consults it first:
re-annotating an already-promoted subject resolves to the *existing* durable cell — the difference
between "annotate the invoice" and "annotate a copy of the invoice."

The map must be rebuildable on open, and "walk every cell looking for `d.extkey`" is O(store) — a
generic attr rank other lenses mint too, so the walk can't even early-exit. Resolution: the
annotation store holds a **`d.promoted` rank** (a dimension minted there, not in the projection —
its absence from §3.2 is deliberate), headed by one `sql.promoted` cell per projected `<dbid>` —
itself a `d.contains` child of the annotation store's home root, so the headcell is findable on open
by name from a known anchor. Promoted envelopes join that rank as members — the membership link
mints **in the same microversion as the promote** (a torn write would leave a promoted twin no
rebuild could find); rebuild walks the rank — O(promoted subjects) — reading each twin's `d.extkey`.

### 6.4 What annotation may assume

The contract annotation code gets, stated once:

- **`pk:` subjects**: re-fetches find the same row; an annotation pinned to the durable cell stays
  meaningful across refresh, reorder, and window toss.
- **`rid:` subjects**: identity is positional-in-table; reuse and `VACUUM` can rebind it —
  annotations carry the caveat on `d.keystab` and reconciliation is user-confirmed (U-S3).
- **`h:` subjects**: the annotation is pinned to *content*; an update mints a new key and the
  annotation visibly stops matching live data — honest staleness, shown as such.
- **`ord:` subjects**: not emitted round one (§4.2); if ever emitted, `d.keystab=ord` warns that the
  identity is position only.
- **A field, not just a row**: a value cell's `v:`-derived key means "the `amount` of invoice 1042"
  is annotatable at field granularity, stable across windows *and* queries (§4.1).

Annotations never reach back into SQL — the read-only boundary is real: annotating a value cell does
not `UPDATE` the column, and the lens's `apply*` stay at their refusing defaults
([`projection_lens.hpp:122-133`](apps/common/xanadu/zigzag/projection_lens.hpp)). For completeness:
the A10 alternative — a Persistent Proxy Cell (`ValueKind::ForeignOpHandle`) dynamically bound to
the live ephemeral ref — is considered and **rejected for round one**: promotion-first gives the
annotation a real ops-index name (R7) with no dangling binding, which is both more durable and more
Nelsonian than proxying.

______________________________________________________________________

## 7. The Backend Port: SQLite First, PostgreSQL Behind It

### 7.1 `SqlBackend` — the pluggable half

```cpp
// apps/common/xanadu/zigzag/sql_backend.hpp  (proposed)
namespace zigzag {

/// One column of a result or a schema table, as the backend reports it.
struct SqlColumn {
  std::string name;
  std::string declType;                    ///< declared type, "" if none
  std::optional<std::string> baseTable;    ///< set iff attributable (§4.3)
  bool keyColumn{false};                   ///< declared PK member
  std::uint32_t keyPosition{0};            ///< its position in the PK (§4.3)
};

struct SqlTable {                          ///< for the schema projection (§3.6)
  std::string schemaName;                  ///< "" for sqlite "main"; pg schema
  std::string name;                        ///< canonical case (§4.4)
  bool isView{false};
  std::vector<SqlColumn> columns;
};

/// A backend-typed value. monostate is SQL NULL (§3.4); bytes is a blob
/// fetched up to the backend's byte budget (§3.5).
using SqlValue = std::variant<std::monostate, std::int64_t, double,
                              std::string, std::vector<std::byte>>;

struct SqlCapabilities {
  bool keysetPaging{false};       ///< row-value tuple predicates
  bool columnProvenance{false};   ///< base-table attribution per column
  bool snapshots{false};          ///< consistent-read scope (WAL / txn cursor)
  bool blobReopen{false};         ///< rowid point-select / sqlite3_blob_open
};

/// One active window cursor: RAII over a prepared read statement. Move-only.
/// fetchNext reads the cursor forward -- the live-window path that preserves
/// the query's own ORDER BY. A cursor prepared while a SqlSnapshot is live
/// runs inside that snapshot's scope.
class SqlCursor {
public:
  virtual ~SqlCursor() = default;
  [[nodiscard]] virtual const std::vector<SqlColumn> &columns() const = 0;
  [[nodiscard]] virtual std::expected<std::vector<std::vector<SqlValue>>,
                                      std::string>
  fetchNext(std::size_t limit) = 0;
};

/// A consistent-read scope (§5.3 replay-3): RAII transaction guard. Outlives
/// sequential cursors; any cursor prepared while it is live joins its scope.
class SqlSnapshot {
public:
  virtual ~SqlSnapshot() = default;
};

class SqlBackend {
public:
  virtual ~SqlBackend() = default;

  [[nodiscard]] virtual std::expected<std::vector<SqlTable>, std::string>
  schema() = 0;

  /// Prepare a read-only statement for WINDOW use. Refuses anything that
  /// writes, anything that isn't a data statement (BEGIN/ROLLBACK/&c. --
  /// §7.2), and multi-statement input (tail must be empty). Refuses with
  /// unexpected while another WINDOW cursor is live -- one connection
  /// serializes its windows. Internal reads (schema(), pragmas, blob
  /// point-selects) take a separate internal path and don't count.
  [[nodiscard]] virtual std::expected<std::unique_ptr<SqlCursor>, std::string>
  prepare(std::string_view sql, std::span<const SqlValue> params) = 0;

  /// Re-prepare a bounded window for replay: the backend owns the wrapping
  /// (subquery+LIMIT/OFFSET for ordinal; tuple predicate for keyset) so the
  /// user's SQL text is never text-surgered.
  [[nodiscard]] virtual std::expected<std::unique_ptr<SqlCursor>, std::string>
  replayWindow(std::string_view sql, std::span<const SqlValue> params,
               std::span<const SqlValue> lo, std::span<const SqlValue> hi,
               std::size_t offset, std::size_t limit) = 0;

  /// Consistent-read scope for snapshot replay; refused where unsupported
  /// (rollback-journal SQLite). Scopes any cursor prepared while it lives.
  [[nodiscard]] virtual std::expected<std::unique_ptr<SqlSnapshot>,
                                      std::string>
  snapshot() = 0;

  [[nodiscard]] virtual SqlCapabilities capabilities() const noexcept = 0;
};

} // namespace zigzag
```

`SqlLens` is *backend-agnostic*; everything SQLite- or PostgreSQL-shaped lives behind this port,
with `<sqlite3.h>`/`<libpq-fe.h>` confined to backend translation units (the
`SwarmCatalogIndex::Impl` pattern, §2.3). The `ExternalKey`'s `<backend>` prefix (§4.1) is what
`capabilities()` names, so a mixed arena has disjoint keyspaces by construction.

Windowed lenses — `SqlLens` is the first — also implement a small `WindowedLens` sub-interface the
session can drive without knowing the concrete type: `materializePage(windowKey)` (the fetch behind
§5.4) and `discardMaterializedAfter(boundaryKey)` (the lens-side half of `rewind`, §5.5 — drops
buffered windows whose mint epoch the arena toss ended, so a replay — never a buffer hit — is the
only way a window comes back). The app reaches `materializePage` through the non-owning `SqlLens*`
it kept (§5.2); the session reaches `discardMaterializedAfter` through the sub-interface.

### 7.2 `SqliteBackend` (round one)

- **Open.** `sqlite3_open_v2(path, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX)` —
  read-only, URI-parsing (enables `file:…?immutable=1` for truly-static files — and the path must be
  percent-encoded when converted to a `file:` URI, since a literal `?`/`#` in a filename silently
  parses as URI query and could smuggle flags), NOMUTEX because the backend is single-threaded by
  contract (§7.1). Minimum SQLite 3.30: a read-only open over a hot `-wal` needs the 3.22
  heap-memory wal-index fallback, and `NULLS FIRST|LAST` syntax needs 3.30 — the floor covers both.
  `journal_mode` is probed at open (before the authorizer is installed) and cached — the
  WAL-vs-rollback answer `snapshot()` needs; the pragma's read form is also allowed post-authorizer
  under the arg2-NULL policy, so a fresh check costs nothing if wanted.
- **Prepare-time gates, in order:** prepare the raw SQL; refuse a `NULL` stmt (comment-only/empty
  input); check `sqlite3_stmt_readonly()` — it gates *"doesn't write"*, which is the right contract
  (it accepts `VALUES`, `EXPLAIN`, transaction controls, and correctly refuses `WITH…UPDATE`; SQLite
  has no DML-in-CTE syntax, so no hole there); require the `pzTail` remainder empty (single
  statement); and **gate statement kind** — accept only `SELECT`/`VALUES`/`WITH…SELECT`/`EXPLAIN`/
  allowlisted `PRAGMA`, because `BEGIN`/`ROLLBACK`/`SAVEPOINT` all pass `stmt_readonly` and a user's
  `ROLLBACK` would silently destroy the backend's own snapshot scope. The authorizer can't separate
  user-from-internal `TRANSACTION` either — the backend flips a flag in the authorizer's `pArg`
  during its own `BEGIN`/`ROLLBACK` so internal transaction control is permitted and user-issued is
  denied.
- **Authorizer** (`sqlite3_set_authorizer`), installed *after* the connection's own pragmas run:
  allow `SELECT`, `READ`, `RECURSIVE`, `TRANSACTION`, `SAVEPOINT`, `FUNCTION` with a per-name
  denylist (`load_extension`, `writefile`, `readfile`, `fts3_tokenizer`), and `SQLITE_PRAGMA` under
  a per-name-and-argument policy — intrinsically read-only pragmas (`table_xinfo`,
  `foreign_key_list`, `data_version`, `database_list`) allowed with their table-name arg; dual-mode
  pragmas (`journal_mode`, `query_only`) allowed only with arg2 `NULL` (a `PRAGMA query_only = 0`
  write attempt is denied — `query_only` is a belt, the authorizer is the boundary). Explicitly deny
  `ATTACH`/`DETACH` — load-bearing for the one-file `<dbid>` grammar (§4.5). All DDL/DML codes fall
  through unlisted → denied.
- **Connection pragmas:** `PRAGMA query_only = ON`.
- **Cancellation:** `sqlite3_busy_timeout` (it *is* an internally installed busy handler — pick it,
  not a custom one) plus a progress handler, so a runaway query or a WAL-locked database returns
  `std::unexpected` instead of hanging the UI thread.
- **Provenance (§4.3):** `table_xinfo` for PK positions and generated columns;
  `sqlite3_column_table_name`/`origin_name` behind the link probe; their absence degrades *query*
  mode to `h:` while browse keeps `pk:` (the lens knows its table). Column case comes from
  `sqlite_schema`.
- **Snapshots:** WAL-only per §5.3 — a held `BEGIN DEFERRED` read txn pins a snapshot at first read
  and never blocks writers (but pins the WAL file — a long-lived snapshot grows `-wal`, so snapshot
  scope is bounded like a checkpoint); rollback-journal databases refuse `snapshot()`, and there a
  held cursor of *any* kind denies writers — the §5.3 idle rule, not a new one.
- **Blobs:** fetched into the `SqlValue::bytes` budget at fetch time (§3.5); over-budget re-fetch
  via `sqlite3_blob_open`/`SELECT … WHERE rowid` only for `pk:`/`rid:` rows.
- **Concurrency:** one `sqlite3*`, one thread — the app owns threading; multiple prepared statements
  interleave fine, so `schema()`, `snapshot()`, and blob point-selects share the connection through
  the internal-read path, distinct from the one-active-window-cursor rule.

### 7.3 `PgBackend` (the later port)

- **Dependency.** `libpq` behind a Makefile probe in the `GLEDITOR_ENABLE_A11Y` style —
  `GLEDITOR_SQL_PG=1` requires, `=0` disables, unset probes `pkg-config libpq`; a `GLEDITOR_HAVE_PG`
  compile flag builds a stub backend refusing `sql:pg:` URIs so the binary always parses the scheme
  (toggling flips a compile flag → whole-tree rebuild, same cost as `GLEDITOR_DISABLE_VULKAN`).
- **Read-only, layered honestly.** `default_transaction_read_only = on` at session level,
  `SET TRANSACTION READ ONLY` inside every `BEGIN` the backend opens (the GUC alone is overridable —
  defense in depth, not a boundary), and a *hard* guarantee is stated to need a read-only role
  (`pg_read_all_data` or no write grants) — because client-side SELECT-only checking cannot see PG's
  data-modifying CTEs (`WITH d AS (DELETE … RETURNING *) SELECT …`), which the read-only transaction
  correctly errors on at execution.
- **Windows are server-side.** The uniform parameterized path is extended-protocol **portals**
  (Bind/Execute with a row limit; a suspended portal *is* a parameterized server cursor, bound to
  its transaction like everything else — it needs the same `BEGIN` scope). `DECLARE CURSOR` is the
  convenience form for the lens's own parameter-free literal browse SQL (it errors in autocommit and
  cannot bind `$n` — quoting literals by hand is a quoting-risk the portal path avoids entirely); a
  `NO SCROLL` cursor can't go backward — rewind/replay is re-`DECLARE`, or `SCROLL` where needed.
- **Provenance:** `PQftable`→`pg_class` OID, `PQftablecol` attnums joined to `pg_constraint.conkey`
  (`contype='p'`; `contype='u'` UNIQUE+NOT NULL is a legitimate second identity rung — noted, not
  required); `PQftype` for `d.type` (not `PQfmod`, which is the type *modifier*); `relkind='v'`
  marks views → `h:` keys; no rowid analogue exists — `ctid` changes on every update including HOT,
  so `rid:` never applies to PG.
- **Values:** `PQgetvalue` text is GUC-dependent — pin `DateStyle`, `IntervalStyle`,
  `extra_float_digits`, `bytea_output` at connect (§4.5) or run a binary cursor; `float4`/`float8`
  text → parse → IEEE-754 bits (`NaN` uses its canonical bit pattern); `numeric`/`timestamp`/
  `interval` and other text types key as `t`; `bytea` arrives as `\x` hex — decode into
  `SqlValue::bytes`; large objects (`lo_*`) are OID references, not `bytea` — round one renders them
  as integer OIDs and says so.
- **Liveness:** `statement_timeout` + `PQcancel` is the runaway-query analog of the progress
  handler; `PQsendQuery`/`PQconsumeInput` keep a slow remote query off the UI thread inside the
  synchronous `fetchNext` shape; `LISTEN`/`NOTIFY` is an optional refresh-invalidation hint.
- **Alternative kept open:** a remote manifold over 9Z
  ([`zz9p.hpp`](apps/common/xanadu/zigzag/zz9p.hpp) — `Zhop`/`Zread`/`Zwatch` pairs) could serve a
  `SqlLens` projection itself — the port boundary is identical either way (U-S6).

### 7.4 Entry points

The lens URI and the `ExternalKey` grammar are different things (§4.1): a user types
`xuzz --lens sql:sqlite:///abs/path.db` (browse mode — the URI dispatches *before* the existing
`fs::exists` check in [`xuzz_app.cpp:509`](apps/xuzz/xuzz_app.cpp)) and adds `--sql 'SELECT …'` or
`--sql @file.sql` for query mode — arbitrary SQL does **not** go in the URI (percent-encoding soup,
and query text in a URI leaks via `ps`). The pg form is `sql:pg://host:port/dbname?schema=public`
(normalized into `<conninfo-hash>` per §4.1 — the URI names the service, the key names the hash).
The interactive surface the gestures live in is the xuzz slice view (browse/page/rewind/promote) and
`vquery` (the §9 traversals); the `--lens` raster path exists for smoke output only.

______________________________________________________________________

## 8. Round One Is Read-Only — and What the Write Phase Looks Like

### 8.1 The read-only contract

Round one overrides **nothing** in the unproject half: `applyContentChange`/`applyAttributeChange`
stay at their `std::unexpected("this lens is read-only")` defaults
([`projection_lens.hpp:122-133`](apps/common/xanadu/zigzag/projection_lens.hpp)). Enforcement is
three-layered: the trait defaults refuse; the backend refuses writes at prepare + statement-kind
gate + authorizer + read-only open (§7.2/§7.3); and the UI offers no edit gesture on `sql.*` cells.
The dirty-cell/`commit()` path of the lens doc §6 is **not part of round one** — `ProjectionSession`
has no `commit()` yet anyway (§2.5).

### 8.2 Phase two, scoped against §8A

When writing arrives, the §8A additions map onto SQL mechanically:

| §8A gap                             | SQL lowering                                                                                  |
| ----------------------------------- | --------------------------------------------------------------------------------------------- |
| `beginBatch`/`endBatch`             | one SQL transaction per `commit()` — rollback, not partial failure                            |
| batched `applyCreate`               | one `INSERT` per created row, all value cells in one statement                                |
| edge properties + `applyEdgeChange` | FK constraint names/actions on `d.ref`/`d.conforms` ranks                                     |
| schema-vs-instance `NodeKind`       | expressed as `d.conforms`/`d.ofcol` membership ranks (§3.6) — done in round one               |
| incidence/occurrence cells          | the `sql.sight` sighting model (§3.7) — done in round one                                     |
| NULL/BlobRef content                | already the round-one model (§3.4, §3.5) — adopted, not deferred                              |
| multi-key rebind                    | `ALTER TABLE RENAME` rekeys every `t:<stable>:` subject                                       |
| `rekey()`                           | `VACUUM`/reindex invalidates `rid:` keys wholesale (§4.4)                                     |
| reparent dimension                  | `d.ref` reparent = `UPDATE … SET fk=?`; `d.contains` refuses (rows don't move between tables) |

Writes also need guards the read path doesn't: an optimistic precondition (`SELECT` the expected
value before `UPDATE`, compare-and-set semantics), a permission story (read-only role for pg; file
mode for sqlite), and the authorizer flipped to admit `INSERT`/`UPDATE`/`DELETE` only — never DDL.
These are phase-two design inputs, listed so round one's shape doesn't foreclose them.

______________________________________________________________________

## 9. The Query and Logic Surface

Everything §9A of the lens doc promises applies unchanged, because the mechanism is the canonical
names, not the source:

```text
##/d.contains/d.seq                 -- every child rank, in order
#query/d.contains/d.seq            -- a query's pages, in order
#page/d.contains/d.seq             -- a page's sightings, in window order
#sight/d.subject[/d.role='sql.row'] -- posward walk to the subject (rank terminates there)
#subj/-d.subject                   -- a subject's sightings (negward members)
#subj/d.contains/d.seq             -- a subject's value cells (union order)
[/d.role = 'sql.sight']            -- restrict to window positions
[/d.keystab = 'pk']                -- subjects whose identity survives refresh
```

run identically over a projected database, a projected directory, and a hand-built slice — the
`lens_vql_gate_test.cpp` equivalence is the gate to extend (`tests/xuzz/sql_lens_test.cpp` reusing
the same assert-both-shapes pattern). vprolog's `cell_link/4` walks the same ranks:
`cell_link(Sight,'d.subject',pos,Next)` steps toward the subject (the rank terminates at it, so a
recursive walk reaches it), `cell_link(Subj,'d.subject',neg,S)` enumerates a subject's sightings,
and `cell_link(V,'d.ofcol',pos,C)` steps toward a value's column; `extkey/2` (the lens doc §9A.3
predicate — currently a doc-comment contract on `ProjectionSession::keyFor`,
[`projection_session.hpp:42-44`](apps/common/xanadu/zigzag/projection_session.hpp)) recovers
`"sql:sqlite:…:pk:i:1042"` from a bound cell — *more* load-bearing for SQL than for any prior lens,
because the ExternalKey is how a logic fact names a database row without an ops index.

Two SQL-specific notes:

- **VQL is not SQL.** The lens projects *results*; the user still writes SQL to name the result
  (§5.1 query mode), or browses to avoid writing it. A future "VQL→SQL" translation is a compiler
  problem over the same schema cells, explicitly out of scope — the lens's job is that both surfaces
  see the same cells.
- **`d.keystab` is the annotation-aware predicate.** "Show me the rows whose annotations can't rot"
  is `[/d.keystab='pk']` — a query the ladder (§4) exists to make expressible.

______________________________________________________________________

## 10. Phased Implementation Plan

Each phase is one commit; after each, `make -j$(nproc)` builds and `make -j$(nproc) test` passes.

1. **`SqlBackend` + `SqliteBackend`, no lens.** The port (§7.1), the read sandbox (§7.2: statement-
   kind gate, authorizer, `pArg` internal-txn flag, pragma policy), `schema()`, cursor `fetchNext`,
   `replayWindow`, the provenance probe, WAL-snapshot support. Unit tests over a temp `.db`:
   non-SELECT refused, `ATTACH` refused, user `BEGIN`/`ROLLBACK` refused, internal `BEGIN` allowed,
   ordinal and keyset windows deterministic, blobs budgeted.
1. **`ProjectionSession::refresh()` + `checkpoint()`/`rewind()`/`pin()` + `staleAttributes`.** The
   union-identity re-projection with the live-run-diff tombstone pass, content restate,
   attr-rewrite, present-keys roots recompute, and internal-mark atomicity (§5.2); the
   stack-disciplined rewind with high-water map/roots repair, pin-refusal, and foreign-mint refusal
   (§5.5); the trait hook with `{}` default. Tested with an in-memory lens whose node set mutates:
   same key → same CellRef, emptied rank severed, dropped key → stale attrs, rewind → keys forgotten
   \+ roots trimmed + trail reverted, pinned key → refused. **Highest-risk item, deliberately
   early.**
1. **`SqlLens` browse mode.** Schema projection (tables/columns + membership ranks), the stub
   horizon, `materializePage`, sightings vs subjects (§3.7), `pk:`/`rid:`/`h:` keys with typed
   renderings, `d.extkey`/`d.keystab`/`d.null`/`d.ordinal` attributes. Gate: a known fixture `.db`
   projects a known cell layout deterministically; `lens_vql_gate`-style tests run the same VQL
   strings over the projection and a hand-built slice.
1. **Query mode + replay.** The `sql:` URI + `--sql` flag (§7.4), key class per §4.3, ordinal
   windows, the three replay semantics (§5.3) with fixtures asserting keyset replay returns
   identical rows under a concurrent inserter (and flags inserts inside the range honestly), and
   WAL-vs-rollback snapshot honesty.
1. **Promotion + the reconcile map.** The overlay staging arena + `promoteSubject` envelope (§6.1),
   the `d.promoted`/`sql.promoted` anchor and `ExternalKey → (Store*, opIndex)` map rebuilt on open
   (§6.3), blob import at promote time. Gate: annotate a `pk:` subject, close, re-project, re-derive
   the same durable cell; `rid:`/`h:` subjects show their `d.keystab` caveat.
1. **The interactive surface + query gates.** xuzz slice-view gestures (browse → page → rewind →
   promote → annotate → resume) and the same VQL/Prolog gates over `SqlLens` (lens doc §9A.4). UX
   journey per the repo's validation workflow.
1. **`PgBackend`.** `libpq` behind `GLEDITOR_SQL_PG` (§7.3); the SQLite tests rerun against a
   postgres fixture where the environment provides one (skipped, not faked, otherwise); portals for
   parameterized windows.
1. **Phase two (separate design): the write path.** Trait v2 (§8.2), guarded UPDATE/INSERT/DELETE in
   one transaction per `commit()`, `rekey()` on `VACUUM`, permission model. Out of scope here by
   construction.

______________________________________________________________________

## 11. Fidelity, Limits, and Honest Edges

| aspect                            | behaviour                                                         | faithful?                           |
| --------------------------------- | ----------------------------------------------------------------- | ----------------------------------- |
| keyed row identity across refresh | `pk:`/`rid:` key → same subject after `refresh()`                 | faithful (class-dependent, §6.4)    |
| keyed row, updated value          | `v:` key survives; `refresh()` restates the content               | faithful — display tracks source    |
| unkeyed row identity              | `h:` hash — survives reorder, dies on update                      | faithful, marked (`d.keystab`)      |
| a row in two different queries    | one subject + one sighting per window (`d.subject` rank)          | faithful by construction (§3.7)     |
| identical rows in one result      | one subject + N sightings                                         | faithful — occurrences kept         |
| NULL vs `''` vs `0`               | `d.null` flag + `d.type`; survives promote only via the flag      | faithful                            |
| `-0.0` vs `0.0`                   | SQL-equal, bit-distinct → different `h:`/`pk:` keys               | honest over-distinction (§4.5)      |
| BLOB                              | budgeted fetch at read; `pk:`/`rid:`-only reopen                  | faithful, bounded (§3.5)            |
| window under concurrent insert    | keyset replay: originals + inserts-inside-range flagged           | faithful in browse; honest in query |
| deleted row after replay          | stale-marked via `staleAttributes`, cell persists                 | honest (append-only arena)          |
| total row count                   | never computed eagerly; stub horizon + on-demand `sql.count`      | honest bound                        |
| implicit rowid reuse / `VACUUM`   | own class `rid:`, caveat on `d.keystab`                           | **lossy on identity**, marked       |
| `WITHOUT ROWID` / temp / view     | declared PK only (and only if fully selected), else `h:`          | faithful                            |
| aggregate/computed column         | `d.ofcol` to a result-column cell; `h:` keys                      | faithful — no base identity exists  |
| FK to an unmaterialized row       | rank membership skipped until both ends exist                     | honest omission, §3.6               |
| concurrent writer                 | WAL: snapshot clean; rollback journal: all held reads deny writes | bounded, honestly scoped (§5.3)     |
| schema change mid-session         | `SQLITE_SCHEMA` → `d.state=stale` + `refresh()`                   | honest re-projection                |
| CellRef held across a rewind      | aliases a different cell — consumers re-resolve by key            | honest hazard, stated (§5.5)        |
| value order on a shared subject   | first-seen union; per-window order is the query's `sql.col` seq   | faithful — order is per-query       |

"Faithful" means: the projection shows what the query returned (occurrences preserved as sightings),
identity means what `d.keystab` says it means, and a refresh never silently transfers an annotation.
The genuinely lossy row — implicit-rowid identity — is a source limitation, not a projector
limitation, and the ladder's marking keeps it honest rather than hidden.

______________________________________________________________________

## 12. Open Questions

- **U-S1. Held-read lifetime.** One `SqlCursor` per window scope is cheap to re-prepare for replay
  but pins nothing; a held cursor preserves `ORDER BY` naturally and — on a rollback journal —
  blocks every writer while it idles (§5.3). The port makes the cursor RAII and the snapshot
  explicit, which answers the mechanism — what remains open is the *policy*: how long a live cursor
  or snapshot may idle before the lens drops it and pays a re-prepare.
- **U-S3. Refresh reconciliation policy.** A row whose key changed (`UPDATE` to a hashed or key
  column) is a delete+create in identity space. Matching "same row, new key" by remaining-column
  similarity is a heuristic — worth it for annotation continuity, dangerous if wrong. Recorded as
  policy, not mechanism: the reconcile map keeps `d.extkey` history so a *user-confirmed* rekey can
  be recorded rather than guessed.
- **U-S4. Pin escalation.** `rewind` refuses naming the pin (§5.5) — decided. What stays open is
  whether the refusal path should offer "promote the pinned subject now" as the escape (violates
  gesture-purity) or leave it to the user (current lean).
- **U-S5. The annotation store.** Where promoted rows live — the user's private annotation store
  alongside `system://activity`, or a per-database sibling store — is a workflow decision (the same
  question every lens's promote gesture asks); this spec needs only that the store exists, the
  `d.promoted` rank (§6.3) can be walked, and the session arena can overlay its manifold (§6.1).
- **U-S6. Serving a lens over 9Z.** `zz9p` already defines the wire pairs; whether PostgreSQL
  eventually arrives as a remote `SqlLens` server (the database *is* the manifold) rather than a
  linked backend changes where the port sits but not the key scheme — deferred, deliberately.
- **Resolved since rev 1.** U-S2 (implicit-rowid honesty): `rid:` is its own stability class, §4.2.
- **Resolved since rev 2.** The single-sited-subject problem: subjects float, windows chain
  `sql.sight` occurrences on the subject's `d.subject` rank, §3.7.
- **Resolved since rev 3.** The `roots()` flood: subjects join the `d.subjects` index rank and root
  election is lens-declared, §3.7. The buffer-resurrection hole: `rewind` invokes the lens's
  `discardMaterializedAfter`, §5.5.

______________________________________________________________________

## Change History

- **rev 1.** Initial design: the conn→query→page→row→value shape, the `pk:`/`h:`/`ord:` ExternalKey
  ladder with `d.keystab` marking, windowed projection via `ProjectionSession::refresh()` +
  `checkpoint()`/`rewind()`, three replay semantics, `promoteRow`/`promotePage` with a reconcile map
  as the annotation crossing, the `SqlBackend` port, read-only round one.
- **rev 2.** Folded a five-perspective critique (Xanadu/ZigZag purist, technical writer,
  Vortex/runtime expert, system architect, SQL expert). Structural corrections: replaced the
  unimplementable "widened `d.contains`" with strict firstborn `d.contains` + `d.seq` chains and
  headcell membership ranks for `d.conforms`/`d.ofcol`/`d.ref` (deg±≤1 — the first draft's fan-out
  self-evicted silently); replaced `promote(row)` — which walks an undirected graph and would dump
  the whole projection — with the disconnected staging-envelope `promoteSubject`; made
  `checkpoint()`/`rewind()` a LIFO stack with high-water map/roots repair, pin-refusal, the CellRef
  *aliasing* rule, and the trail/compaction cost model; replaced the contradictory stub-per-page
  enumeration with the bounded stub horizon (materialized stubs + one trailing `deferred` stub).
  Identity corrections: defined the typed `<sql-value>`/`<pk-rendering>`/`rowhash`/`<window-id>`
  encodings; split `rid:` (implicit rowid, reuse- and `VACUUM`-volatile) out of `pk:`; demoted
  `ord:` to reserved; made key class orthogonal to mode; subjects-not-occurrences dedup; schema-
  qualified `<stable>` names; ATTACH denied. Backend corrections: full authorizer action list
  (`TRANSACTION`/`SAVEPOINT`/read-`PRAGMA` allowlist — the first draft denied the `BEGIN` its own
  snapshot needed); cursor/snapshot as RAII objects on the port; composite keyset via tuple
  predicates; blob byte-budget instead of impossible laziness; WAL-vs-rollback-journal snapshot
  honesty; PG portal/transaction/`relkind`/`PQftype`/`bytea`/GUC details;
  `default_transaction_read_only` downgraded to defense-in-depth. Documentation: reconciled the
  mode/key-class table, the `d.state` value set, rank-table completeness, the two R8s,
  lens-doc-vs-local § refs, the `swarm_catalog_index` sandbox overclaim, and the reconcile-map
  rebuild path.
- **rev 3.** Folded the same five perspectives' second round. Structural corrections: added the
  `sql.sight` occurrence layer (§3.7) — pages chain *sightings*, subjects float, `d.subject` is the
  join — resolving the one-cell-two-windows rank-position conflict and retiring `d.dupcount`;
  flipped membership-rank direction so members chain posward *toward* the "one" side (uniform with
  `FilesystemLens`' `d.ref`); gave `refresh()` its tombstone pass (live-run diff + `unlink()` —
  re-assertion alone cannot empty a rank or sever a tail), internal-mark atomicity, and present-keys
  roots recompute; made the staging envelope mint in a dedicated arena *overlaid on the annotation
  store's manifold* with dims pre-declared there (an ephemeral dim promotes as an anonymous copy —
  name-unresolvable, silently breaking the `d.extkey` rebuild); added `pin()`/ `unpin()` and the
  foreign-mint rewind refusal. Identity corrections: `qhash` reverted to raw text + typed params
  (`normalized_sql` folds literals and collides distinct queries); `v:`-derived value-cell keys for
  field-level annotation; `-0.0`/NaN honesty; the conninfo-hash user inclusion stated as a
  privilege-realm choice. Backend corrections: `prepare()`'s exclusion rescoped to one active
  *window* cursor (a cursor inside a snapshot joins its scope — the rev-2 wording forbade its own
  replay mode); statement-kind gate so user `BEGIN`/`ROLLBACK` can't hijack snapshot scope;
  `journal_mode` probed pre-authorizer; per-name-and-argument pragma policy; held-read blocking
  generalized to cursors on rollback journals; NULL-aware keyset predicates with explicit
  `NULLS FIRST|LAST`; blob digests made budget-independent over the complete value; portals as the
  uniform parameterized PG path. Plus ~30 wording/consistency repairs.
- **rev 4.** Third critique round. Structural: result columns moved off the query's `d.contains`
  (which can't hold two firstborns) onto a `d.colof` membership rank; sighting keys re-based on
  `(window, window-ordinal)` so N sightings of one subject in one window get distinct cells;
  `d.ofcol` ranks and `v:` keys anchor at *schema* columns when attributable — the same
  single-siting fix as sightings, one level down; the `d.subjects` index rank plus lens-declared
  roots stop subjects flooding `roots()`. Runtime: `rewind` now invokes the lens's
  `discardMaterializedAfter` — the off-arena page buffer would otherwise resurrect every tossed
  window; `refresh()` restates surviving nodes' content (a `pk:` row's updated value) and diffs the
  *live* run for tombstones (a recorded edge set can't survive rewind); the staging arena gets a
  lifecycle (mark before, release after, base advanced post-promote) and the copy is content-level
  only — a `CellRef` carried across arenas silently aliases; envelope dims mint at first promote,
  not session open; blobs ride the envelope as scratch content (`promote()` spools them itself —
  `importFileStore` was the wrong primitive). SQL: the NULL-aware keyset expansion stated in general
  form, `NULLS FIRST|LAST` bumps the floor to 3.30, reserved `__sql_sort_*` aliases keep re-selected
  sort columns out of `v:` keys, `d.winlo` unified as the exclusive bound on stubs and pages alike
  (the `<window-id>` `k:` bound is known pre-fetch, so a stub's key *is* its page's key),
  foreign-mint detection stated as a count delta.
