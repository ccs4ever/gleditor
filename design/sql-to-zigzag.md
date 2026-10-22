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
`arena_manifold.hpp` (`promote`, `mark`/`release`, `attach(Space)`), `multi_store.cpp`
`resolveDimension`, and [`anything-to-zigzag-lens.md`](anything-to-zigzag-lens.md) §4–§6, §8A.

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
  mints an empty cell
  ([`projection_session.cpp:54-55`](apps/common/xanadu/zigzag/projection_session.cpp)) — the slot
  SQL `NULL` maps onto (§3.3). There is no blob alternative; §3.4 defines the by-reference treatment
  that avoids importing blob bytes for a browse.
- `ProjectionSession` projects a lens into a `VortexCore`'s arena — **the same arena the query/logic
  runtimes enumerate** (the §9A.2 project-into-bound-arena contract,
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

### 2.2 SQLite is already in the tree

`sqlite3` is a pkg-config dependency of the xanalogical engine ([`Makefile:277`](Makefile)), and
`swarm_catalog_index.cpp` already opens a database, applies a schema, and reads rows through the C
API ([`swarm_catalog_index.cpp:185-246`](apps/common/xanadu/swarm_catalog_index.cpp)). A `SqlLens`
adds **no new dependency** for the first backend; the include lives in the one backend translation
unit (§7.2), keeping the trait header free of `<sqlite3.h>` exactly as `FilesystemLens` keeps
`<sys/stat.h>` inside one file (the §5.4 boundary rule).

### 2.3 The persistence crossing and the memory budget exist

- `promote(store, parent, arena, root, budget)`
  ([`arena_manifold.hpp:834-836`](apps/common/xanadu/zigzag/arena_manifold.hpp)) writes the
  reachable subgraph of one root cell into a `Store`: scratch spans acquire permascroll addresses,
  deferred scalar renderings are written, links mint after cells. It is the single legal crossing —
  a persistent op refusing an ephemeral target is enforced at `applyStructure`
  ([`manifold.hpp:159`](apps/common/xanadu/zigzag/manifold.hpp), `ephemeralBit`).
- The arena's choice-point machinery — `mark()`, `release(m)`
  ([`arena_manifold.hpp:638-642`](apps/common/xanadu/zigzag/arena_manifold.hpp)) — is the O(1)
  window-toss the implementation plan's invariant A9 prescribes for long-running lenses ("windowed
  materialization + epoch tosses, using `ExternalKey` for cross-epoch identity",
  [`zigzag-lenses-implementation-plan.md:54`](design/zigzag-lenses-implementation-plan.md)); spike
  S5 measured a toss at ~0.6 µs. §5.4 turns this into the rewind/replay mechanism.
- `attach(Space)` ([`arena_manifold.hpp:297`](apps/common/xanadu/zigzag/arena_manifold.hpp)) lets
  one arena host several spaces; combined with the string-keyed map, two `SqlLens` projections (or a
  lens plus a xanadoc) share one query surface without key collisions (§4.1).
- `insertIntoRank`/`linkChain`
  ([`arena_manifold.hpp:617`](apps/common/xanadu/zigzag/arena_manifold.hpp),
  [`:632`](apps/common/xanadu/zigzag/arena_manifold.hpp)) are the shared "turn a vector into a rank"
  primitives a window materialization uses to re-chain `d.seq`.
- `StoreActivityLog` (`system://activity`,
  [`store_activity_log.hpp:29-30`](apps/common/xanadu/store_activity_log.hpp)) is the append-only
  record of reader walks; §5.6 reuses it to make a page's materialization parameters durable, so a
  replay survives a session restart.
- The enfilade layer is explicit that derived, replayable index products live in memory and never
  mint disk ops ([`arrayfilade.hpp`](apps/common/xanadu/enfilade/arrayfilade.hpp), the "Ruling R8"
  bullet) — the same discipline §5 applies to windows: **paging mints cells in the arena, not
  operations in a store.**

### 2.4 What is missing

- **Incremental projection.** A `ProjectionSession` projects once, at construction
  ([`projection_session.cpp:15-19`](apps/common/xanadu/zigzag/projection_session.cpp)). There is no
  `refresh()` — `ProcLens::refreshFromHost`
  ([`proc_lens.hpp:67`](apps/common/xanadu/zigzag/proc_lens.hpp)) mutates lens state but nothing
  re-runs enumeration. A windowed lens needs exactly this: §5.2 specifies
  `ProjectionSession::refresh()` as the one new framework method, with union-identity semantics.
- **A session-level rewind.** `mark()`/`release()` exist on the arena, but the session's identity
  map would dangle past a release — keys for tossed cells must be forgotten too. §5.4 specifies
  `checkpoint()`/`rewind()` on the session.
- **Promotion bookkeeping.** `promote()` knows nothing about `ExternalKey`s; nothing records "this
  row is already promoted, annotate the durable twin." §6.3 specifies the reconciliation map.
- **A write path.** Deferred to phase two (§8); the §8A v2 list is its prerequisite.

______________________________________________________________________

## 3. The Source Model: A Result Set as Cells and Ranks

### 3.1 The shape

```
 [conn]                                   d.role = sql.conn
   │  +d.contains
   ▼
 [query]   d.role=sql.query               content: the SQL text
   │  +d.contains                          attrs: d.qhash, d.params, d.winstate
   ▼
 [page₀] ─d.seq→ [page₁] ─d.seq→ …        d.role=sql.page
   │  +d.contains                          attrs: d.winlo, d.winhi, d.wincount,
   ▼            d.contains to EVERY row    d.state (live|deferred|tossed)
 [row₀] ─d.seq→ [row₁] ─d.seq→ …          d.role=sql.row
   │  +d.contains                          content: display key (pk or ordinal)
   ▼            attrs: d.extkey, d.keystab, d.ordinal
 [v₀] ─d.seq→ [v₁] ─d.seq→ …              d.role=sql.val
                attrs: d.column (name), d.type (decl type/affinity),
                       d.null (present iff NULL)
                +d.ofcol→ [column cell]   (schema edge, §3.5)
 [row₀] ─d.conforms→ [table cell]         (schema edge, §3.5)
 [fk val] ─d.ref→ [target row]            (FK edges, §3.5, deferred)
```

Three structural decisions, each load-bearing:

1. **A page contains *every* row, rows chain `d.seq`.** `promote()` promotes the reachable subgraph
   of a root ([`arena_manifold.hpp:823-826`](apps/common/xanadu/zigzag/arena_manifold.hpp)): a page
   cell with `d.contains` to each row makes `promotePage` a single call (§6.2), while `d.seq` keeps
   result order for traversal. This is the same firstborn/sibling split as `d.contains`/`d.seq`
   elsewhere, with containment widened to all members so reachability does not depend on walking a
   chain.
1. **A row's values are cells, not attributes.** Attribute-rank cells are the framework's metadata
   mechanism ([`projection_session.cpp:71-78`](apps/common/xanadu/zigzag/projection_session.cpp)),
   and a column value is *content*, not metadata: the user annotates "the `amount` field of row
   1042", which must be a first-class addressable cell. Values therefore project as `d.contains`
   children chained `d.seq` in select order, each carrying `d.column`/`d.type`/`d.null` attributes
   and a `d.ofcol` edge to its schema column (§3.5).
1. **A query is a node.** `forEachEdge` resolves ends through the identity map, and a query node's
   `d.qhash` is part of every row key it produces (§4.2), so "the same SELECT re-run" lands in the
   same keyspace and two different queries never collide — while two result sets that happen to
   contain the same *source row* still share identity when the key is a primary key (§4.3).

### 3.2 Roles and the canonical/generic split

Roles ride `d.role` as attribute cells, as every lens does: `sql.conn`, `sql.query`, `sql.page`,
`sql.row`, `sql.val`, `sql.table`, `sql.col`. The generic structural ranks take their canonical
meanings: `d.contains` (conn→query→page→row→value, query→page, table→column), `d.seq` (pages, rows,
values, columns), `d.ref` (foreign-key and user cross-references), `d.clone` (only for genuine
shared-authority content — a duplicate row value is *not* a clone; two equal strings are two
occurrences). SQL-specific ranks are declared via `dimensions()` under their own names:

| dimension                        | kind           | meaning                                         |
| -------------------------------- | -------------- | ----------------------------------------------- |
| `d.contains`                     | generic (enum) | conn→query→page→row→value; table→column         |
| `d.seq`                          | generic (enum) | page order, row order, column order             |
| `d.ref`                          | generic (enum) | foreign-key edge row→row                        |
| `d.role`                         | generic (attr) | node kind (`sql.row`, `sql.page`, …)            |
| `d.extkey`                       | generic (attr) | the row's `ExternalKey`, for promotion (§6.3)   |
| `d.conforms`                     | sql custom     | row→table, value→column conformance (schema)    |
| `d.ofcol`                        | sql custom     | value→schema-column edge                        |
| `d.column`                       | sql custom     | a value's column name                           |
| `d.type`                         | sql custom     | declared type / affinity (`TEXT`, `INTEGER`, …) |
| `d.null`                         | sql custom     | present iff the value is SQL NULL (§3.3)        |
| `d.ordinal`                      | sql custom     | the row's position in its result (scalar)       |
| `d.keystab`                      | sql custom     | key stability class: `pk`/`hash`/`ordinal` (§4) |
| `d.qhash`                        | sql custom     | hash of normalized query + bound params         |
| `d.params`                       | sql custom     | bound parameter rendering                       |
| `d.winlo`/`d.winhi`/`d.wincount` | sql custom     | page window bounds (§5.3)                       |
| `d.state`                        | sql custom     | `live`/`deferred`/`tossed`/`stale` page state   |

An unknown rank name mints an empty dimension rather than erroring
([`multi_store.cpp:353`](apps/common/xanadu/multi_store.cpp), the fallthrough), so every name above
is declared in `dimensions()` up front — a missing declaration is a silent-empty-rank bug, the
§4.3.1 failure mode.

### 3.3 NULL, empty string, and monostate

SQL `NULL` maps to `NodeContent{std::monostate}` → an empty cell *plus* a `d.null` attribute. The
attribute is load-bearing, not decoration: an empty-string value also mints a cell whose text is
`""`, and `arena.textOf` cannot tell the two apart. The rule a query can rely on: **a NULL value
cell carries `d.null`; an empty-string cell does not.** This closes the §8A "NULL ≠ '' ≠ 0"
complaint without a parallel shadow rank — `d.null` is the flag, `d.type` carries the column's
declared type, and the cell itself stays honestly empty.

### 3.4 Blobs by reference, never by import

A BLOB does not become a content span on browse. Projecting a 2 MB blob into the author's
permascroll to *view* it is exactly the R8 violation §8A warned about — a read-only browse must not
mutate the store. Round one projects a blob value cell whose content is a rendering —
`blob(1048576 B, sha256:ab12…)` — with `d.type= BLOB` and the digest as the cache key. The bytes are
fetched *through the backend on demand*: only when the user opens the cell for viewing, or when the
row is promoted (§6.2, where `importFileStore` is the correct tool — promotion is the moment bytes
legitimately enter the permascroll). The framework needs no `BlobRef` variant for round one; the
rendering-plus-backend-fetch split keeps it honest.

### 3.5 The schema projection (browse mode)

In **browse mode** (§5.1) the lens also projects the schema itself, because the schema is what makes
rows explainable: `conn` → `d.contains` → a `sql.table` cell per table/view → `d.contains` → a
`sql.col` cell per column (content: name; attributes: `d.type`, `d.pk`, `d.notnull`, `d.ordinal`).
Rows get a `d.conforms` edge to their table cell; values get a `d.ofcol` edge to their column cell —
the §8A "schema vs instance" node kinds expressed as edges rather than a new enum field, since the
framework's `SourceNode` has no kind slot and doesn't need one: conformance is itself traversable
structure. In **query mode** the lens still mints column cells for the result's own column list
(from cursor metadata, §7.1) so `d.ofcol` resolves even where no base table is visible; a value's
`d.conforms` is simply absent when the backend cannot attribute it to a base column — an honest
omission, not a `NULL` rank.

Foreign keys mint `d.ref` edges row→row **only when both ends are already materialized** (both in
the key map — an edge naming an unprojected node is skipped silently,
[`projection_session.cpp:88-90`](apps/common/xanadu/zigzag/projection_session.cpp)). FK edges are
therefore opportunistic in round one: emitted by `forEachEdge` where resolvable, never forced by
materializing the target. Chasing references across pages is a navigation gesture that *requests* a
window (§5.5), not a projection-time obligation.

______________________________________________________________________

## 4. The ExternalKey Ladder: Identity When the Source Only Conditionally Has One

### 4.1 The encoding

```
ExternalKey (sql) ::= "sql:" <backend> ":" <dbid> ":" <subject>
  <backend> ::= "sqlite" | "pg"                    -- §7, more later
  <dbid>    ::= "f:" <sha256(canonical path)>      -- sqlite: the file
              | "s:" <host> ":" <port> "/" <db>    -- pg: the service
  <subject> ::= "t:" <table> ":" "pk:" <pk-rendering>          -- keyed row
              | "q:" <qhash> ":" "h:" <sha256(row rendering)>  -- hashed row
              | "q:" <qhash> ":" "ord:" <ordinal>              -- positional row
              | "t:" <table>                                   -- table
              | "t:" <table> ":" "c:" <column>                 -- column
              | "q:" <qhash>                                   -- query
              | "q:" <qhash> ":" "p:" <window-id>              -- page
```

`<qhash>` is SHA-256 over normalized statement text plus the bound-parameter rendering — identical
queries share a keyspace, and the hash is recorded on `d.qhash` so a stored reference can re-derive
the query it names. The `backend`/`dbid` prefix is what lets one arena host two databases (or a
database and a xanadoc) with zero key collisions — the string-key design already assumes disjoint
prefixes (§5.2 of the lens doc).

### 4.2 The ladder, weakest to strongest

1. **`pk:` — keyed.** The lens can prove the row's source identity: a single-table browse including
   the primary key (or SQLite `rowid`) in the projection. Survives refresh, reorder, update of
   *other* columns, and page moves. This is the only class whose annotation can safely follow a
   replay by name (§6.4).
1. **`h:` — hashed.** No provable key (a join, a grouped select, a keyless table): the key is the
   row's content hash. Survives reorder and refresh-that-didn't-touch-this-row; two identical rows
   collapse to one key — which is *correct* for deduplicated viewing but means an annotation on one
   applies to all copies. Honest, and marked so.
1. **`ord:` — positional.** Nothing better exists: the key is the row's ordinal in the result.
   Survives nothing — a refresh that inserts a row above renumbers it. Legal only because
   `d.keystab=ordinal` tells every consumer, including the annotation path, that this identity is
   positional. The rule is the same as §5.2's no-inode fallback: say what the key can promise, never
   pretend it is stable.

`d.keystab` carries the class (`pk`|`hash`|`ordinal`) on every row cell; it is the attribute a
"rekey after VACUUM" or a refresh reconciliation consults first.

### 4.3 Which class applies is decided per query, not per row

The backend reports, per result, whether each column is a base-table key column (§7.1
`keyColumns()`): SQLite exposes `sqlite3_column_table_name`/`origin_name` when compiled in (a
build-time probe, not an assumption — §7.2), PostgreSQL exposes `PQftable`/`PQftablecol`. A result
containing *all* key columns of *one* base table gets `pk:` keys; anything else falls to `h:`, with
`ord:` reserved for results where even hashing is meaningless (a streaming `ord:` fallback the
backend can disable in favour of `h:` always — the default is `h:`).

### 4.4 What breaks each rung

- `VACUUM` renumbers rowids of `INTEGER PRIMARY KEY`-less tables — the §8A `rekey()` case. For `pk:`
  rows the *user-declared* PK is preferred over rowid exactly because rowid is volatile; where rowid
  is all there is, the key embeds it and `d.keystab` still reads `pk` — a documented lie the open
  questions (U-S2) own.
- An `UPDATE` to a key column or to any hashed column changes the key — by design; the old key's
  promoted twin keeps its recorded `d.extkey` as provenance (§6.3), and refresh reconciliation is
  what notices "same row, new key" (a `h:`-collision or a `pk:`-match on other columns — policy, not
  mechanism; U-S3).
- A cross-query `pk:` row *is* the same subject — browse and query-mode keys for the same
  `(table, pk)` pair are equal by construction, so annotations unify across the two modes. This is
  deliberate and worth the constraint it imposes: `pk:` keys never include `<qhash>`.

______________________________________________________________________

## 5. Windowing and Replay

### 5.1 Two modes, two paging guarantees

| mode     | query source                         | window mechanism                       | key class      |
| -------- | ------------------------------------ | -------------------------------------- | -------------- |
| `browse` | lens-generated `SELECT … ORDER BY k` | **keyset**: `WHERE k > :last LIMIT :n` | `pk:`          |
| `query`  | arbitrary user SQL                   | **ordinal**: `LIMIT :n OFFSET :m`      | `h:` or `ord:` |

Browse mode is how the user reaches a table without writing SQL (a `sql.table` cell's "open" gesture
generates the query); the lens controls the text, so it appends the key column to `ORDER BY` and
paginates by key — windows are *stable under insertion* and replayable by name. Query mode wraps
arbitrary SQL: the lens cannot rewrite it safely (it appends `LIMIT`/`OFFSET` via a subquery —
`SELECT * FROM (<sql>) LIMIT :n OFFSET :m` — never by text surgery on the original), so windows are
positional and `d.keystab` tells the truth about them.

### 5.2 `ProjectionSession::refresh()` — the one new framework method

A windowed lens is stateful: `SqlLens` holds the backend cursor and a buffer of materialized pages;
`forEachNode` enumerates *the current materialized state* — structural nodes plus every row of every
`live` page. To open page k, the app asks the lens to fetch page k into its buffer, then calls a new
session method:

```cpp
/// Re-enumerate the lens and reconcile the arena with it: mint cells for keys
/// not yet in the map, leave existing cells (and their CellRefs) untouched,
/// re-assert edges (a page's d.seq chain and d.contains set are rebuilt each
/// refresh), and mark projected cells whose key no longer appears as stale
/// via their d.state attribute. Union semantics for identity; replace
/// semantics for structure.
[[nodiscard]] std::size_t refresh();
```

Union-on-identity is what makes replay safe: a row seen in page 1 and re-fetched in page 5 keeps its
cell — `keyToCell_` hits, no re-mint — so anything pointing at that cell (a selection, an ephemeral
note draft, a promoted twin's proxy) is undisturbed. Structure is replace-on-refresh: the page's
`d.contains`/`d.seq` reflect *this* fetch, so a row that vanished is unlinked from the page and its
`d.state` attribute flips to `stale` — the cell persists (the arena is append-only; nothing
un-mints) but no live page reaches it. `refresh()` is framework code, not lens code: the same loop
serves `ProcLens::refreshFromHost` and any future live lens — the current one-shot `project()`
becomes the first `refresh()`.

### 5.3 A page is a cell that knows how to re-fetch itself

Each page cell carries its own replay parameters as attributes — `d.winlo`/`d.winhi` (keyset
bounds), `d.wincount`, `d.qhash`, `d.state` — so a page is self-describing: any session that can
reach the backend can replay it without out-of-band state. The three replay semantics, all against
the *recorded* parameters:

1. **Live replay.** Re-execute the window SQL against current data. Cheapest; sees inserts and
   deletes; the honest default for dashboards.
1. **Keyset replay** (browse mode only). `WHERE k BETWEEN :winlo AND :winhi` — re-fetch *the same
   rows by key*, so the window is stable under concurrent insertions elsewhere. `d.wincount` bounds
   the fetch; a shortfall means rows were deleted and shows as fewer live rows, not as wrong ones.
1. **Snapshot replay.** Inside one `REPEATABLE READ`/`SERIALIZABLE` transaction (or SQLite's
   read-transaction snapshot), every window reads one consistent version — paging backward re-reads
   bytes already read, *deterministically*. The page's `d.state` records which snapshot scope it
   belongs to; crossing a snapshot boundary is defined as a live replay so no cell silently mixes
   versions.

### 5.4 Rewind: `checkpoint()`/`rewind()` — the A9 toss made session-safe

Scrolling forward without bound grows the arena forever — the exact failure A9 exists to prevent.
The session gains:

```cpp
struct Checkpoint { ArenaManifold::Mark mark; std::size_t mapHighWater; };
[[nodiscard]] Checkpoint checkpoint();      ///< mark() + current map size
void rewind(const Checkpoint &);            ///< release(mark); drop keys minted since
```

`ProjectionSession` keeps a mint-order log alongside the two maps, so `rewind` can erase exactly the
keys minted after the checkpoint — the map-repair the raw `release()` cannot do itself. The
scroll-forward story is then free: take a checkpoint per page boundary; to go back to page 2,
`rewind(cp2)` tosses pages 3…N in O(1) (the S5-measured toss), and revisiting page 3 is a *replay* —
re-fetch, re-mint, same keys → same ExternalKeys → a *new* set of CellRefs this epoch, which is why
A9 insists identity cross epochs as `ExternalKey`, not CellRef. Anything the user might attach to —
a selection, a draft annotation — pins its page: pinned pages are skipped by rewind, or the gesture
refuses with the pin named (policy in §11, U-S4). **Promoted rows are immune to all of this**: they
live in the `Store`, not the arena (§6).

### 5.5 Deferred pages: bounded projection by construction

`forEachNode` yields a page-stub node for every page of the result (the count comes from a
`LIMIT`-ed probe or `d.wincount=unknown` — a stub need not know the total), marked
`d.state=deferred` with its `d.winlo`/`d.winhi` recorded but no row children. Materializing a stub
is the `refresh()` of §5.2. Total count is deliberately *not* computed eagerly: `SELECT COUNT(*)` on
a large join is a scan the user didn't ask for; the UI shows "N+ rows" from the stub count, not a
false exact total. A `count(*)` request is itself a replayable page of role `sql.count`.

### 5.6 Recording the walk

Each materialization can append the page's replay parameters (query hash, window bounds, snapshot
scope, key class) to `system://activity`
([`store_activity_log.hpp:29`](apps/common/xanadu/store_activity_log.hpp)) as a visit — the same
append-only record navigation already keeps. A replay is then also *auditable*: "which version of
page 3 did I annotate from" is a lookup, not a guess. This is optional in round one (the activity
log is the user's own store), but the hook is cheap because a page is already self-describing
(§5.3).

______________________________________________________________________

## 6. Promotion on Demand: The Annotation Crossing

The arena projection is ephemeral — `ephemeralBit`-tagged refs that a persistent `SetLink` refuses
([`manifold.hpp:159`](apps/common/xanadu/zigzag/manifold.hpp); the lens doc's invariant 4). A row a
user wants to annotate must cross into a `Store`, and the only legal crossing is `promote()`
([`arena_manifold.hpp:834`](apps/common/xanadu/zigzag/arena_manifold.hpp)). Nothing in the lens
promotes by itself: **promotion is a user gesture**, which is what keeps R8 intact — browsing and
replaying mint zero ops anywhere.

### 6.1 `promoteRow`

The gesture on a `sql.row` cell: `promote(store, parent, arena, rowCell, budget)`. Reachability does
the rest — the row's `d.contains` value cells (with their `d.column`/`d.type`/`d.null` attribute
cells) and its `d.extkey`/`d.keystab`/`d.ordinal` attribute cells all travel, because the page→row
direction is *into* the row. The promoted row lands in the user's annotation store as an ordinary
structure cell — after which annotation is **the generic mechanism, not SQL machinery**: note cells
`d.contains`/`d.ref`-linked to the row or to an individual value cell, a xanalink from a value's
text into a xanadoc, a transclusion of the row's rendering. The SQL lens's job ends at the crossing;
it deliberately does not know what an annotation is.

### 6.2 `promotePage`

The gesture on a `sql.page` cell promotes the whole materialized window — §3.1's decision that a
page `d.contains` *every* row exists for exactly this: one `promote()` call, one durable snapshot of
the window as seen (a "this is what the report looked at" artefact, the workflow use-case for
promoting a page rather than a row). Blobs under a promoted row are imported through
`importFileStore` at this point — the first moment their bytes legitimately enter the permascroll
(§3.4).

### 6.3 Idempotent promotion: the reconcile map

`promote()` knows nothing about `ExternalKey`s, so the session keeps a third map:
`ExternalKey → GlobalOpRef` ("this subject is promoted, at this op"). The gesture consults it first:
re-annotating an already-promoted row resolves to the *existing* durable cell instead of minting a
second twin — the difference between "annotate the invoice" and "annotate a copy of the invoice".
`d.extkey` travels on the promoted cells (the §5.3(b) mechanism, with its recorded promote-skip
caveat), so the map is *rebuildable*: opening a store that already contains promoted rows walks
`d.extkey` and re-derives it, which is also how annotations on `pk:`-keyed rows survive a fresh
session — the durable cell still names `"sql:sqlite:…:t:invoice:pk:1042"` in hypertime, and the live
projection keys the same row identically (§4.3).

### 6.4 What annotation may assume

The contract annotation code gets, stated once:

- **`pk:` rows**: re-fetches find the same row; an annotation pinned to the durable cell stays
  meaningful across refresh, reorder, and window toss.
- **`h:` rows**: the annotation is pinned to *content*; an update changing the row mints a new key
  and the annotation visibly stops matching live data — honest staleness, shown as such.
- **`ord:` rows**: the annotation is pinned to *position*; the promote gesture should warn, and the
  durable cell's `d.keystab=ordinal` is the permanent record of why its identity is fragile.

Annotations never reach back into SQL — the read-only boundary is real: annotating a value cell does
not UPDATE the column, and the lens's `apply*` stay at their refusing defaults
([`projection_lens.hpp:122-133`](apps/common/xanadu/zigzag/projection_lens.hpp)).

______________________________________________________________________

## 7. The Backend Port: SQLite First, PostgreSQL Behind It

### 7.1 `SqlBackend` — the pluggable half

```cpp
// apps/common/xanadu/zigzag/sql_backend.hpp  (proposed)
namespace zigzag {

/// One column of a result, as the backend reports it.
struct SqlColumn {
  std::string name;
  std::string declType;             ///< backend's declared type, "" if none
  std::optional<std::string> baseTable;  ///< set iff attributable (§4.3)
  bool keyColumn{false};            ///< part of that table's primary key
};

/// A backend-typed value. monostate is SQL NULL (§3.3); bytes is a blob the
/// lens renders by reference (§3.4).
using SqlValue = std::variant<std::monostate, std::int64_t, double,
                              std::string, std::vector<std::byte>>;

/// The port the lens speaks. One connection, one active read cursor; all
/// methods are synchronous — the lens runs on the caller's thread and the
/// app owns threading, exactly as sqlite3's serialized mode assumes.
class SqlBackend {
public:
  virtual ~SqlBackend() = default;

  /// Tables and their columns, for the schema projection (§3.5).
  [[nodiscard]] virtual std::expected<std::vector<SqlTable>, std::string>
  schema() = 0;

  /// Prepare a read-only statement. Refuses anything not a SELECT (§8.1).
  [[nodiscard]] virtual std::expected<SqlCursor, std::string>
  prepare(std::string_view sql, std::span<const SqlValue> params) = 0;

  [[nodiscard]] virtual const std::vector<SqlColumn> &columns() const = 0;

  /// A window of rows starting at ordinal @p offset; empty span past the end.
  [[nodiscard]] virtual std::expected<std::vector<std::vector<SqlValue>>,
                                      std::string>
  fetch(std::size_t offset, std::size_t limit) = 0;

  /// Keyset variant for browse mode (§5.1): rows with key > :lastKey.
  [[nodiscard]] virtual std::expected<std::vector<std::vector<SqlValue>>,
                                      std::string>
  fetchAfter(const SqlValue &lastKey, std::size_t limit) = 0;

  /// Snapshot scope for §5.3 replay-3; refused where unsupported.
  [[nodiscard]] virtual std::expected<SqlSnapshot, std::string>
  snapshot() = 0;

  /// What this backend can prove: keyset paging, column provenance, snapshots.
  [[nodiscard]] virtual SqlCapabilities capabilities() const noexcept = 0;
};

} // namespace zigzag
```

`SqlLens` is *backend-agnostic*; everything SQLite- or PostgreSQL-shaped lives behind this port. The
`ExternalKey`'s `<backend>` prefix (§4.1) is the same string `capabilities()` names, so a mixed
arena — a SQLite file and a Postgres service in one session — has disjoint keyspaces by
construction.

### 7.2 `SqliteBackend` (round one)

- `sqlite3_open_v2(path, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI)` — a read-only lens opens
  read-only. `PRAGMA query_only = ON` as belt-and-braces.
- `prepare()` rejects non-SELECT up front with `sqlite3_stmt_readonly()`, then an authorizer
  (`sqlite3_set_authorizer`) allowing only `SELECT`/`READ`/`RECURSIVE`/`FUNCTION` minus a denylist
  (`load_extension`, `readfile`, `writefile`) — the same sandbox posture the codebase already takes
  for embedded SQLite (`swarm_catalog_index.cpp` wraps the same C API,
  [`swarm_catalog_index.cpp:185`](apps/common/xanadu/swarm_catalog_index.cpp)).
- Cancellation: a progress handler plus a busy timeout, so a runaway query or a locked database (WAL
  writer elsewhere) returns `std::unexpected` instead of hanging the UI thread.
- Column provenance (§4.3): `sqlite3_column_table_name`/`origin_name` where the build provides them
  — probed at compile time, since Debian's sqlite enables them and a minimal build may not; their
  absence downgrades results to `h:` keys honestly.
- `fetchAfter` implements keyset paging by binding the recorded `d.winhi`; `snapshot()` is a
  deferred read transaction held across `fetch` calls.
- BLOBs fetch lazily — the cursor returns a `SqlValue::bytes` *handle* the lens renders by reference
  (§3.4); `sqlite3_column_blob` runs only when the row is opened or promoted.

### 7.3 `PgBackend` (the later port)

- `libpq`, an **optional** dependency behind a Makefile probe in the `GLEDITOR_SDL`/
  `GLEDITOR_ENABLE_A11Y` style — `GLEDITOR_SQL_PG=1` to require it, unset probes `pkg-config libpq`
  — so the tree still builds without it. Connection via a URI in the `sql:pg://host:port/db` key
  form (§4.1).
- Read-only is enforced twice: `default_transaction_read_only = on` at session level plus the lens's
  own SELECT-only refusal — a defense in depth a network backend owes (a hostile string reaching a
  shared server is worse than reaching a local file).
- Windows map to server-side cursors: `DECLARE c CURSOR FOR <sql>` + `FETCH :n`, which gives
  snapshot paging (replay-3) natively — `fetch(offset,limit)` is `FETCH`/`MOVE`, and the cursor is
  the snapshot. Keyset `fetchAfter` is the same SQL shape as SQLite's.
- Column provenance via `PQftable`/`PQftablecol` + `PQfmod`; `pk:` keys resolve through a
  `pg_constraint` lookup in `schema()`.
- Async: `PQsendQuery`/`PQconsumeInput`/`PQgetResult` let a slow remote query stay off the UI thread
  — the one backend concern SQLite doesn't have, listed because the port's synchronous shape (§7.1)
  was chosen deliberately: the *backend* hides async inside `fetch` so the lens never sees a thread.
- Wire-level alternative the design keeps open: a remote manifold over 9Z
  ([`zz9p.hpp`](apps/common/xanadu/zigzag/zz9p.hpp) — `Zhop`/`Zread`/`Zwatch` pairs already exist)
  could serve a `SqlLens` projection itself, making the PostgreSQL case a *lens server* rather than
  a linked backend; the port boundary is identical either way.

______________________________________________________________________

## 8. Round One Is Read-Only — and What the Write Phase Looks Like

### 8.1 The read-only contract

Round one overrides **nothing** in the unproject half: `applyContentChange`/`applyAttributeChange`
stay at their `std::unexpected("this lens is read-only")` defaults
([`projection_lens.hpp:122-133`](apps/common/xanadu/zigzag/projection_lens.hpp)). Enforcement is
three-layered: the trait defaults refuse; the backend refuses non-SELECTs at `prepare()` and opens
the database read-only (§7.2); and the UI treats `sql.*` cells as browse-only — there is no edit
gesture to wire to a refusal. The dirty-cell/`commit()` path of the lens doc §6 is **not part of
round one** — `ProjectionSession` has no `commit()` yet anyway (§2.4).

### 8.2 Phase two, scoped against §8A

When writing arrives, the §8A additions map onto SQL mechanically:

| §8A gap                             | SQL lowering                                                                                  |
| ----------------------------------- | --------------------------------------------------------------------------------------------- |
| `beginBatch`/`endBatch`             | one SQL transaction per `commit()` — rollback, not partial failure                            |
| batched `applyCreate`               | one `INSERT` per created row, all value cells in one statement                                |
| edge properties + `applyEdgeChange` | FK constraint names/actions on `d.ref`/`d.conforms` edges                                     |
| NULL/BlobRef content                | already the round-one model (§3.3, §3.4) — adopted, not deferred                              |
| multi-key rebind                    | `ALTER TABLE RENAME` rekeys every `t:<table>:` subject                                        |
| `rekey()`                           | `VACUUM`/reindex invalidates rowid keys wholesale (§4.4)                                      |
| reparent dimension                  | `d.ref` reparent = `UPDATE … SET fk=?`; `d.contains` refuses (rows don't move between tables) |

Writes also need guards the read path doesn't: an optimistic precondition (`SELECT` the expected
value before `UPDATE`, compare-and-set semantics), a permission story (read-only database user for
pg; file mode for sqlite), and the same authorizer posture flipped to admit `INSERT`/`UPDATE`/
`DELETE` only — never DDL. These are phase-two design inputs, listed here so round one's shape
doesn't foreclose them.

______________________________________________________________________

## 9. The Query and Logic Surface

Everything §9A of the lens doc promises applies unchanged, because the mechanism is the canonical
names, not the source:

```text
##/d.contains/d.seq                 -- every row of every page, in order
[/d.role = 'sql.row']              -- restrict to row cells
#row/d.contains                     -- a row's value cells, in select order
#val/d.ofcol                        -- a value's schema column
[/d.column = 'amount']             -- one field across the materialized set
[/d.keystab = 'pk']                -- rows whose identity survives refresh
```

run identically over a projected database, a projected directory, and a hand-built slice — the
`lens_vql_gate_test.cpp` equivalence is the gate to extend (`tests/xuzz/sql_lens_test.cpp` reusing
the same assert-both-shapes pattern). vprolog's `cell_link(Row,'d.contains',pos,V)` enumerates a
row's fields as facts with no SQL awareness; `extkey/2` (the §9A.3 predicate — currently a
doc-comment contract on `ProjectionSession::keyFor`,
[`projection_session.hpp:42-44`](apps/common/xanadu/zigzag/projection_session.hpp)) is what lets a
goal recover `"sql:sqlite:…:pk:1042"` from a bound cell, which is *more* load-bearing for SQL than
for any prior lens: the ExternalKey is how a logic fact names a database row without an ops index.

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

1. **`SqlBackend` + `SqliteBackend`, no lens.** The port (§7.1), the read sandbox (§7.2),
   `schema()`, ordinal `fetch`, keyset `fetchAfter`, the column-provenance probe. Unit tests over a
   temp `.db`: read-only enforcement refuses `UPDATE`, `fetch` windows deterministically, blob
   values come back as bytes-by-reference.
1. **`ProjectionSession::refresh()` + `checkpoint()`/`rewind()`.** The union-identity re-projection
   and the map-repaired toss (§5.2, §5.4) with the mint-order log — framework changes tested with an
   in-memory lens whose node set mutates between refreshes: same key → same CellRef, dropped key →
   `d.state=stale`, rewind → keys forgotten.
1. **`SqlLens` browse mode.** Schema projection (tables/columns), page stubs, row materialization on
   `refresh`, the `pk:` key rung, `d.extkey`/`d.keystab`/`d.null` attributes. Gate: a known fixture
   `.db` projects a known cell layout deterministically; `lens_vql_gate`-style tests run the same
   VQL strings over the projection and a hand-built slice.
1. **Query mode.** Arbitrary `SELECT` through a `sql:` URI (`--lens 'sql:sqlite:///db?q=…'`),
   `h:`/`ord:` keys, ordinal windows, deferred stubs, the three replay semantics (§5.3) with
   fixtures asserting keyset replay returns identical rows under a concurrent inserter.
1. **Promotion + the reconcile map.** `promoteRow`/`promotePage` gestures, `d.extkey` on durable
   cells, `ExternalKey → GlobalOpRef` map rebuilt on open (§6.3); blobs imported at promote time.
   Gate: annotate a `pk:` row, close, re-project, re-derive the same durable cell; annotate an
   `ord:` row → warning path exercised.
1. **`xuzz --lens sql:` entry + the vquery/vprolog surface.** Extend `executeLensRaster`
   ([`xuzz_app.cpp:506`](apps/xuzz/xuzz_app.cpp)) from a path to a URI; the same VQL/Prolog gates
   over `SqlLens` (§9A.4 of the lens doc — the cross-lens equivalence). UX journey per the repo's
   validation workflow: browse a table, page, rewind, promote a row, annotate, close, resume.
1. **`PgBackend`.** `libpq` behind the `GLEDITOR_SQL_PG` probe (§7.3); the SQLite tests rerun
   against a postgres fixture where the environment provides one (skipped, not faked, otherwise);
   server cursors for snapshot replay.
1. **Phase two (separate design): the write path.** Trait v2 (§8.2), guarded UPDATE/INSERT/DELETE in
   one transaction per `commit()`, `rekey()` on `VACUUM`, permission model. Out of scope here by
   construction.

______________________________________________________________________

## 11. Fidelity, Limits, and Honest Edges

| aspect                            | behaviour                                                  | faithful?                             |
| --------------------------------- | ---------------------------------------------------------- | ------------------------------------- |
| keyed row identity across refresh | `pk:` key → same cell after `refresh()`                    | faithful                              |
| unkeyed row identity              | `h:` hash — survives reorder, dies on update               | faithful, marked (`d.keystab`)        |
| positional row identity           | `ord:` — survives nothing                                  | honest by marking                     |
| a row in two different queries    | same `(table,pk)` → same `pk:` key → one cell              | faithful by construction (§4.3)       |
| NULL vs `''` vs `0`               | `d.null` flag + `d.type`                                   | faithful                              |
| BLOB                              | rendered by reference; bytes at open/promote only          | faithful, lazy (§3.4)                 |
| window under concurrent insert    | keyset replay: same rows; ordinal: shifts honestly         | faithful in browse; honest in query   |
| deleted row after replay          | unlinked, `d.state=stale`, cell persists                   | honest (append-only arena)            |
| total row count                   | never computed eagerly; "N+" from stubs                    | honest bound                          |
| `rowid` renumbered by `VACUUM`    | `rekey()`-shaped invalidation; prefer declared PKs (§4.4)  | **lossy** where rowid is all there is |
| `WITHOUT ROWID` table             | declared PK only — the good case                           | faithful                              |
| aggregate/computed column         | no base column → no `d.ofcol`, `h:`/`ord:` keys            | faithful — it has no source identity  |
| FK to an unmaterialized row       | edge skipped (both ends must be minted)                    | honest omission, §3.5                 |
| two identical rows, `h:` keys     | one cell — deduplication by identity                       | faithful to the hash, caveat on notes |
| concurrent writer (WAL)           | busy-timeout + `unexpected`, snapshot for consistency      | bounded, no hang                      |
| schema change mid-session         | `rekey()`-adjacent; column cells re-resolve on `refresh()` | honest re-projection                  |

"Faithful" means: the projection shows what the query returned, identity means what `d.keystab` says
it means, and a refresh never silently transfers an annotation. The genuinely lossy row —
`rowid`-only tables across `VACUUM` — is a source limitation, not a projector limitation, and the
ladder's marking is what keeps it honest rather than hidden.

______________________________________________________________________

## 12. Open Questions

- **U-S1. Where the cursor lives.** A long-lived `sqlite3_stmt` per page is simple but pins a read
  snapshot implicitly; a re-prepare per fetch is snapshot-fresh but needs the window bounds
  re-bound. The §5.3 split (snapshot scope explicit on the page) decides it — but who *closes* a
  snapshot when the user wanders away is a lifetime question the port leaves to the session.
- **U-S2. `rowid` honesty.** `pk:` keys over rowid claim `d.keystab=pk` while `VACUUM` can renumber
  — either the attr distinguishes `pk-declared` from `pk-rowid` (a fourth class), or `rekey()`
  learns to detect it. The doc prefers the fourth class; it is a one-word change to §4.2.
- **U-S3. Refresh reconciliation policy.** A row whose key changed (`UPDATE` to a hashed or key
  column) is a delete+create in identity space. Matching "same row, new key" by remaining-column
  similarity is a heuristic — worth it for annotation continuity, dangerous if wrong. Recorded as
  policy, not mechanism: the reconcile map keeps `d.extkey` history so a *user-confirmed* rekey can
  be recorded rather than guessed.
- **U-S4. Pinning vs rewind.** A selection or draft annotation on an unpromoted cell pins its page
  against `rewind()` — refuse the rewind naming the pin, or auto-promote the pinned rows first?
  Auto-promote is friendlier and violates "promotion is a user gesture"; the doc leans
  refuse-and-name, with the pin visible.
- **U-S5. The annotation store.** Where promoted rows live — the user's private annotation store
  alongside `system://activity`, or a per-database sibling store — is a workflow decision (the same
  question every lens's promote gesture asks); this spec needs only that the store exists and the
  reconcile map (§6.3) can walk it.
- **U-S6. Serving a lens over 9Z.** `zz9p` already defines the wire pairs; whether PostgreSQL
  eventually arrives as a remote `SqlLens` server (the database *is* the manifold) rather than a
  linked backend changes where the port sits but not the key scheme — deferred, deliberately.

______________________________________________________________________

## Change History

- **rev 1.** Initial design: the conn→query→page→row→value shape, the `pk:`/`h:`/`ord:` ExternalKey
  ladder with `d.keystab` marking, windowed projection via `ProjectionSession::refresh()` +
  `checkpoint()`/`rewind()` (union-identity re-projection over A9 epoch tosses), three replay
  semantics (live / keyset / snapshot), `promoteRow`/`promotePage` with the
  `ExternalKey → GlobalOpRef` reconcile map as the annotation crossing, the `SqlBackend` port
  (SQLite first, PostgreSQL behind it), read-only round one with the §8A trait-v2 list scoped as
  phase two.
