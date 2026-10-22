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

Two facts about the manifold constrain the whole §3 shape, and the first draft of this document got
both wrong:

- **One pos, one neg per dimension.** A `CellSlot` carries `DimLink{dim,pos,neg}` entries
  ([`manifold.hpp:250-264`](apps/common/xanadu/zigzag/manifold.hpp)); `arena.link(from,dim,dir,to)`
  writes both ends and **evicts whatever either end held** — eviction is silent success, not a
  refusal ([`arena_manifold.hpp`](apps/common/xanadu/zigzag/arena_manifold.hpp), the `link` doc
  comment; `manifold.cpp`'s displaced-end repair). deg±(c,d) ≤ 1 is the invariant (plan A2,
  [`zigzag-lenses-implementation-plan.md:47`](design/zigzag-lenses-implementation-plan.md)). The
  canonical many-children encoding is therefore **parent → firstborn on `d.contains`, siblings
  chained on `d.seq`**, exactly as `FilesystemLens::forEachEdge` documents ("parent -> FIRST child
  only … siblings chain on d.seq", `filesystem_lens.cpp`) and `ProjectionSession::project()` assumes
  for root detection ("only the first child is a containment target; the rest are sequence targets",
  [`projection_session.cpp:83-85`](apps/common/xanadu/zigzag/projection_session.cpp)). Many-to-one
  relations are encoded as a **membership rank**: the "one" side is the rank's headcell and the
  "many" are its members (§3.6) — a column *is* a rank through the table.
- **`promote()` walks an undirected graph.** The frontier expands *both* `edge.pos` and `edge.neg`
  on every dimension (`arena_manifold.cpp`, the `discover(edge.dim)`/`discover(edge.pos)`/
  `discover(edge.neg)` loop), skipping only `d.dims`, proxies, provenance cells, and read-through
  base cells. Reachability from any cell inside a connected component is the *whole component* — the
  spec's promotion shape must be a separate component, not a clever direction choice (§6.1).

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
  properties bound how it can be used (§5.4–§5.6): marks are a **stack** — "assumed well nested:
  `release()` and `discard()` pop" (`arena_manifold.hpp`, the marks doc comment); `release`
  truncates the contiguous mint suffix *and* replays the trail in reverse, so writes to pre-mark
  cells are undone too; and `compact()` is refused while any mark is outstanding
  ([`arena_manifold.hpp:648-669`](apps/common/xanadu/zigzag/arena_manifold.hpp)), so checkpoints
  must be short-lived or garbage accumulates. CellRefs are dense indices — `ephemeralBit | dense`
  (`arena_manifold.hpp`, `refOf`) — **reused** after a release truncates `slots_`: a stale CellRef
  does not dangle, it *aliases* whatever now occupies the slot (§5.6's aliasing rule).
- `attach(Space)` ([`arena_manifold.hpp:297`](apps/common/xanadu/zigzag/arena_manifold.hpp)) lets
  one arena host several spaces; combined with the string-keyed map, two `SqlLens` projections (or a
  lens plus a xanadoc) share one query surface without key collisions (§4.1).
- `insertIntoRank`/`linkChain`
  ([`arena_manifold.hpp:617`](apps/common/xanadu/zigzag/arena_manifold.hpp),
  [`:632`](apps/common/xanadu/zigzag/arena_manifold.hpp)) are the shared "turn a vector into a rank"
  primitives a window materialization uses to chain `d.seq`.
- `StoreActivityLog` (`system://activity`,
  [`store_activity_log.hpp:29-30`](apps/common/xanadu/store_activity_log.hpp)) is the append-only
  record of reader walks; §5.7 reuses it to make a page's materialization durable.
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

### 3.1 The shape — strict firstborn + sibling + membership ranks

Every "one-to-many" below is the canonical encoding — parent → **firstborn** on `d.contains`,
siblings chained on `d.seq` — because a cell physically holds one pos and one neg neighbour per
dimension (§2.2). Every "many-to-one" is a **membership rank**: the "one" side heads the rank and
the "many" are members on it (§3.6).

```
 [conn]
   │ +d.contains → first query; queries chain +d.seq
   ▼
 [query]   d.role=sql.query      content: the SQL text
   │ +d.contains → first page     attrs: d.qhash, d.params
   ▼         (pages chain +d.seq)
 [page₀]   d.role=sql.page       attrs: d.state, d.winlo, d.winhi, d.wincount
   │ +d.contains → first row      (rows chain +d.seq)
   ▼
 [row₀]    d.role=sql.row        content: display key (§3.3)
   │ +d.contains → first value    attrs: d.extkey, d.keystab, d.ordinal, d.dupcount
   ▼         (values chain +d.seq)
 [v₀]      d.role=sql.val        attrs: d.column, d.type, d.null (iff NULL)
   │
   │  membership ranks (§3.6):
   │  [table]  ─d.conforms→ row₁ ─d.conforms→ row₂ ─…   (table is headcell)
   │  [column] ─d.ofcol→   v₁   ─d.ofcol→   v₂   ─…    (column is headcell)
   │  [target] ─d.ref→     fkval₁ ─d.ref→   fkval₂ ─…  (target is headcell)
   │
   └ [row] ─d.conforms? no: the row is a MEMBER of the table's rank;
     it reaches the table by walking -d.conforms to the headcell.
```

Three structural decisions, each load-bearing:

1. **Firstborn `d.contains`, sibling `d.seq`, everywhere.** `conn` contains the first query, a query
   contains the first page, a page contains the first row, a row contains the first value, a table
   contains the first column; siblings chain `d.seq`. This is the only encoding `arena.link()`
   realizes — anything else self-evicts (§2.2) — and it is what `project()`'s root detection and the
   VQL idiom `##/d.contains/d.seq` already assume. (The first draft's "widened containment to all
   members" was unimplementable: N `d.contains` edges leave only the last linked.)
1. **A row's values are cells, not attributes.** Attribute-rank cells are the framework's metadata
   mechanism ([`projection_session.cpp:71-78`](apps/common/xanadu/zigzag/projection_session.cpp)),
   and a column value is *content*, not metadata: the user annotates "the `amount` field of row
   1042", which must be a first-class addressable cell. Values project as `d.contains`/`d.seq`
   children in select order, each carrying `d.column`/`d.type`/`d.null` attributes and membership in
   its column's `d.ofcol` rank (§3.6).
1. **A query is a node.** Its `d.qhash` scopes the keyspace of every *unkeyed* row it produces
   (§4.3) — keyed rows take table-scoped `pk:` keys that deliberately omit `qhash` so browse and
   query modes unify on `(schema, table, pk)` (§4.4).

### 3.2 Roles and the canonical/generic split

Roles ride `d.role` as attribute cells, as every lens does: `sql.conn`, `sql.query`, `sql.page`,
`sql.row`, `sql.val`, `sql.table`, `sql.col`, `sql.count` (a `COUNT(*)` page, §5.4), `sql.annot`
(the promotion envelope, §6.1). The generic structural ranks take their canonical meanings;
SQL-specific ranks are declared via `dimensions()` under their own names. A page is the materialized
form of a window — the terms are used interchangeably from here.

| dimension                        | kind           | meaning                                             |
| -------------------------------- | -------------- | --------------------------------------------------- |
| `d.contains`                     | generic (enum) | firstborn: conn→query→page→row→value; table→column  |
| `d.seq`                          | generic (enum) | sibling order: queries, pages, rows, values, cols   |
| `d.ref`                          | generic (enum) | FK referrer rank headed by the target row           |
| `d.role`                         | generic (attr) | node kind (`sql.row`, `sql.page`, …)                |
| `d.extkey`                       | generic (attr) | the subject's `ExternalKey`, for promotion (§6.3)   |
| `d.conforms`                     | sql custom     | row-membership rank headed by a `sql.table` cell    |
| `d.ofcol`                        | sql custom     | value-membership rank headed by a `sql.col` cell    |
| `d.column`                       | sql custom     | a value's column name (display; `d.ofcol` is truth) |
| `d.type`                         | sql custom     | declared type / affinity (`TEXT`, `INTEGER`, …)     |
| `d.pk`                           | sql custom     | present on a `sql.col` in the primary key, in order |
| `d.notnull`                      | sql custom     | present on a `sql.col` declared `NOT NULL`          |
| `d.null`                         | sql custom     | present iff the value is SQL NULL (§3.4)            |
| `d.ordinal`                      | sql custom     | the row's position in its result                    |
| `d.dupcount`                     | sql custom     | occurrences the cell stands for, if > 1 (§4.3)      |
| `d.keystab`                      | sql custom     | key stability class: `pk`/`rid`/`hash`/`ord` (§4)   |
| `d.qhash`                        | sql custom     | hash of normalized query + bound params             |
| `d.params`                       | sql custom     | bound-parameter rendering (display)                 |
| `d.winlo`/`d.winhi`/`d.wincount` | sql custom     | page window bounds, typed-encoded (§5.3)            |
| `d.state`                        | sql custom     | page state `deferred`/`live`; row state `stale`     |

An unknown rank name mints an empty dimension rather than erroring
([`multi_store.cpp:353`](apps/common/xanadu/multi_store.cpp), the fallthrough), so every name above
is declared in `dimensions()` up front — a missing declaration is a silent-empty-rank bug, the lens
doc's §4.3.1 failure mode. All dimension minting goes through `VortexCore::findOrMintDimension`,
never `ArenaManifold::ensureDimension`: the two consult different name maps (the `d.dims` rank vs
`arenaDims_`), and mixing them mints two same-named dimensions — the same silent-empty-rank bug one
level down (`vortex_core.cpp`, `findOrMintDimension`; `arena_manifold.cpp`, `ensureDimension`).

Two honesty notes on this table: (a) attribute cells are minted as **text**
(`arena.makeCell(std::string_view)` in `projection_session.cpp`), so `d.ordinal`/`d.wincount`/
`d.dupcount` lose R6 scalar fidelity until the framework grows scalar attribute cells — a display
and comparison limitation, not an identity one; (b) ~15 bespoke `d.*` names sit in unresolved
tension with plan invariant A3 ("struct fields bastardize dimensions"), which the landed
`FilesystemLens` (`d.size`/`d.mtime`/`d.perm`/`d.name`) and the parent lens doc's R13 ("metadata is
cells on a dimension") already endorse — this spec follows the landed convention and records the
tension rather than resolving it.

### 3.3 A row's content: the display key

A `sql.row` cell's content is a **display key**: the human-readable form of its key subject — the pk
rendering for `pk:`/`rid:` rows, a short hash prefix for `h:` rows. It exists so a row cell has
visible text; the machine identity is always `d.extkey`.

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
value cell's content is a size-only rendering (`blob(1048576 B)` — no digest, since a hash requires
the eager read anyway and the bytes live in the backend buffer until the cell is opened or
promoted); the bytes ride the buffer for the annotate/promote path. Over budget, the cell renders
`blob(… B, truncated)` and bytes are re-fetchable **only** for `pk:`/`rid:` rows, by point-select or
`sqlite3_blob_open` — for `h:` rows the over-budget blob is simply not viewable, and the cell says
so. Bytes enter the permascroll only at promote time via `importFileStore` (§6.2) — the first moment
they legitimately may (convergence R8).

### 3.6 Membership ranks for schema and references

In **browse mode** (§5.1) the lens also projects the schema, because schema is what makes rows
explainable: `conn` → `d.contains`/`d.seq` → a `sql.table` cell per table/view →
`d.contains`/`d.seq` → a `sql.col` cell per column (content: name; attributes: `d.type`, `d.pk`,
`d.notnull`, `d.ordinal`). The many-to-one relations become ranks headed by the schema/row cell they
name:

- **`d.conforms`**: `[table] → row₁ → row₂ → …` — the table is the headcell; a row finds its table
  by walking *negward* `d.conforms` to the headcell (`d.role = sql.table`), and the table enumerates
  conforming rows *posward*. This is the §8A "schema vs instance" node-kind gap expressed as
  structure rather than a field: conformance is traversable, which is the point.
- **`d.ofcol`**: `[column] → v₁ → v₂ → …` — the same pattern from a `sql.col` headcell. A value's
  column is its negward `d.ofcol` walk to the headcell; `d.column` remains as the cheap display
  attribute (and the only column identity available in query mode when schema attribution fails).
- **`d.ref`** (foreign keys): `[target row] → fkval₁ → fkval₂ → …` — the referenced row heads the
  rank of its referrers, where a referrer member is the FK *value* cell (annotating "this reference"
  is then annotating the member cell — the §8A incidence/occurrence cell for free). A referrer
  resolves its target by negward walk; a row enumerates referrers posward. Emitted **only when both
  ends are already materialized** — an edge naming an unprojected node is skipped silently
  ([`projection_session.cpp:88-90`](apps/common/xanadu/zigzag/projection_session.cpp)); chasing a
  reference across pages is a navigation gesture that *requests* a window (§5.4), never a
  projection-time obligation. Many members on one rank is not a deg± violation: each member holds
  its own pos/neg pair.

In **query mode** the lens still mints `sql.col` cells for the result's own column list (from cursor
metadata, §7.1) so `d.ofcol` resolves; a *row's* `d.conforms` is absent where the backend cannot
attribute it to exactly one base table — an honest omission, not a `NULL` rank.

______________________________________________________________________

## 4. The ExternalKey Ladder: Identity When the Source Only Conditionally Has One

### 4.1 The encoding

Two grammars share the `sql:` prefix and must not be confused: the **lens URI** a user types
(`sql:sqlite:///abs/path.db` — browse, §7.4) and the **ExternalKey** a node carries:

```
ExternalKey (sql) ::= "sql:" <backend> ":" <dbid> ":" <subject>
  <backend> ::= "sqlite" | "pg"                           -- §7
  <dbid>    ::= "f:" <sha256(realpath of db file)>        -- sqlite: exactly one file;
                                                        --   ATTACH is denied (§7.2), so a
                                                        --   connection names one database
              | "s:" <conninfo-hash>                      -- pg: hash of normalized
                                                        --   host:port/db (+ user)
  <subject> ::= "t:" <stable> ":" "pk:" <pk-rendering>    -- declared-PK row
              | "t:" <stable> ":" "rid:" <rowid>          -- implicit-rowid row
              | "q:" <qhash> ":" "h:" <rowhash>           -- hashed row (query or view)
              | "q:" <qhash> ":" "ord:" <ordinal>         -- positional row (reserved)
              | "t:" <stable>                             -- table/view
              | "t:" <stable> ":" "c:" <column>           -- column
              | "q:" <qhash>                              -- query
              | "q:" <qhash> ":" "p:" <window-id>         -- page stub
  <stable>  ::= <schema-qualified, canonically-cased table name>   -- see §4.5
  <qhash>   ::= sha256 over (normalized SQL text || typed params)  -- see §4.5
```

The typed renderings, all normative (they feed key *equality*):

```
<sql-value> ::= <tag> ":" <payload>
  tag "n" NULL | "i" int64 decimal | "f" IEEE-754 bits as 16 hex
    | "t" UTF-8 text, percent-encoded | "b" sha256 hex of the blob bytes
<pk-rendering> ::= <sql-value> ("," <sql-value>)*   -- declared PK order:
                                                   -- table_xinfo pk positions /
                                                   -- conkey attnums (§7)
rowhash input  ::= <sql-value>* in select order, joined by U+0000
                 (column names excluded: qhash already scopes the keyspace)
<window-id>  ::= "k:" <sql-value>*   -- keyset window: first row's full sort key
              |  "o:" <ordinal>      -- ordinal window: start offset
```

Keys stay bounded: a blob contributes its digest, not its bytes. Composite keys are first-class —
`<pk-rendering>` is a tuple — because the common case is multi-column.

### 4.2 The ladder, weakest to strongest

1. **`pk:` — declared-key.** The result maps injectively to exactly one base table and includes
   every declared PK column (any column of an `INTEGER PRIMARY KEY` alias counts — an IPK is stored
   data and survives `VACUUM`). Survives refresh, reorder, update of other columns, page moves.
1. **`rid:` — implicit rowid.** Single-table browse of a table with no declared PK but a rowid. The
   rowid is volatile twice over — `VACUUM` renumbers it, and deletion of the max rowid lets a later
   insert *reuse* it without any VACUUM — so it gets its own class rather than masquerading as `pk`
   (this resolves the first draft's U-S2: the fourth class is adopted, not left open).
1. **`h:` — hashed.** No provable key (joins, grouped selects, views, keyless `WITHOUT ROWID`,
   missing column metadata): the key is the row's content hash over the typed encoding above.
   Survives reorder and refresh-that-didn't-touch-this-row; an update mints a new key.
1. **`ord:` — positional, reserved.** Not emitted in round one; reserved for a backend that cannot
   buffer a row long enough to hash it (a future streaming cursor). If it is ever emitted, only
   `d.keystab=ordinal` saves it from masquerading as stable — the first draft made it a live rung
   with no trigger; there is no honest one.

`d.keystab` carries the class on every row cell; it is the attribute a rekey or refresh
reconciliation consults first, and the attribute the annotation contract reads (§6.4).

### 4.3 Eligibility is per query, and cells model *subjects*

The backend reports, per result column, its base-table attribution and key membership (§7.1
`SqlColumn`): SQLite uses `PRAGMA table_xinfo` (whose `pk` field carries composite-PK *position* —
`table_info` lacks generated/hidden columns and positions) plus, where the build provides them,
`sqlite3_column_table_name`/`origin_name` (`SQLITE_ENABLE_COLUMN_METADATA` — a **link probe**, not
an `#ifdef`: defining it against a lib that lacks the symbols fails at link); PostgreSQL uses
`PQftable`/`PQftablecol` attnums joined to `pg_constraint` `conkey` (`contype='p'`). A result gets
`pk:` keys when **exactly one** base table qualifies *and* rows map injectively — an equijoin on two
tables' PKs satisfies the test twice and is refused to `h:`, and a join fan-out that duplicates one
table's rows collapses them: **a cell models a subject, not an occurrence**, so two result rows with
one key become one cell carrying `d.dupcount = 2`. The same rule governs `h:` duplicates; the lens
dedups at emission (one `SourceNode` per distinct key), and duplicate keys inside one enumeration
are a lens bug, never a silent collapse. When column metadata is unavailable the degradation is
per-mode, not global: **browse keeps `pk:`** (the lens generated the query and knows its table —
`table_xinfo` alone suffices), **query falls to `h:`**.

### 4.4 What unifies and what breaks each rung

- **Cross-query unification.** `pk:`/`rid:` keys name `(stable, key)` — never `<qhash>` — so a row
  reached by browse and by two different queries is *one subject, one cell, one annotation target*.
  This is the deliberate payoff of the key grammar.
- **Implicit-rowid reuse (`rid:`).** `max(rowid)+1` assignment means a deleted high-water rowid is
  re-issued to a later insert; `VACUUM` renumbers wholesale. A `rid:` key asserts position, not
  history — `d.keystab=rid` is the honest label and the annotation contract treats it accordingly.
- **Key/hashing column updates.** An `UPDATE` to a PK or any hashed column changes the key — by
  design; the promoted twin keeps its `d.extkey` as provenance and refresh reconciliation is what
  notices "same row, new key" (policy, not mechanism; U-S3).
- **Schema-qualified names (`<stable>`).** PG tables resolve to `<schema>.<table>` via
  `PQftable`→`pg_class.relnamespace`/`relname` (also fixing `search_path` aliasing — `users` and
  `public.users` must produce one key); SQLite uses the canonical case from `sqlite_schema`, so
  `FROM Invoice` and `FROM invoice` key alike.
- **`qhash`.** `sqlite3_normalized_sql()` (3.27+, where built) or canonical raw text, plus the typed
  parameter encoding — identical queries share a keyspace regardless of literal-vs-bound spelling.

### 4.5 What breaks the whole ladder

- **`ATTACH`ed databases.** The `<dbid>` grammar names one file; a connection attaching several
  breaks it. Resolution: **`ATTACH` is denied by the authorizer** (§7.2) — the single-file keyspace
  is enforced, not assumed. Temp tables live in the `temp` database and never mint `pk:`/`rid:` keys
  (they are `h:` — nothing about them is durable).
- **Path aliasing.** `realpath(3)` (not `weakly_canonical` alone) resolves symlinks so two spellings
  of one file hash identically; PG endpoints alias similarly (unix sockets vs TCP to one cluster) —
  `<conninfo-hash>` normalizes what libpq reports, not what the user typed.
- **Text rendering is not a key input.** Floats (`-0.0`, precision drift), dates, `money` — text
  renderings vary by platform/GUC. The typed encoding above is the canonical form; on PG the backend
  pins session GUCs (`DateStyle`,`IntervalStyle`,`extra_float_digits`,`bytea_output`) at connect so
  renderings are stable *within* a session too.

______________________________________________________________________

## 5. Windowing and Replay

### 5.1 Two modes, two paging mechanisms

| mode     | query source                         | window mechanism                                     |
| -------- | ------------------------------------ | ---------------------------------------------------- |
| `browse` | lens-generated `SELECT … ORDER BY …` | **keyset**: tuple predicate `WHERE (cols) > (:sk)`   |
| `query`  | arbitrary user SQL                   | **ordinal**: held cursor forward; replay by `OFFSET` |

Key class is orthogonal — decided per §4.3 for *both* modes (`pk:`/`rid:` wherever eligible, `h:`
otherwise); the first draft wrongly tied `pk:` to browse mode, which would have broken §4.4's
cross-mode unification. In browse mode the lens generates `SELECT <cols>, <sort key>` over one table
— it appends the sort columns **plus the PK** to `ORDER BY` so the sort key is unique, then
paginates by keyset (row-value tuple comparison `(c1,…,ck) > (?,?,…)`, SQLite ≥ 3.15 and PG both
support row values; a nullable sort column degrades that query to ordinal windows — stated, not
hidden). For an implicit-rowid browse the generated SELECT *explicitly adds* `rowid` — `SELECT *`
does not include it, and a user column literally named `rowid` would shadow it (detected via
`table_xinfo`, not the name). Query mode wraps nothing into the user's text: live windows read the
held cursor forward (which preserves the query's own `ORDER BY` perfectly); a *replay* of an ordinal
window re-prepares `SELECT * FROM (<sql>) LIMIT :n OFFSET :m` — with the `;`-stripping,
single-statement tail check, and "ORDER BY inside a FROM-subquery is not contractually guaranteed"
caveat documented at §7.2.

### 5.2 `ProjectionSession::refresh()` — the one new framework verb

A windowed lens is stateful: `SqlLens` owns the backend and a buffer of materialized pages;
`forEachNode` enumerates *the current materialized state* — structural cells plus every row of every
`live` page. To open page k, the app asks the lens to fetch page k into its buffer
(`materializePage`, §5.4), then calls a new session method:

```cpp
/// Re-enumerate the lens and reconcile the arena with it:
///  - mint cells for keys not yet in the map (same key -> same CellRef);
///  - re-assert edges (link() overwrites in place, so a page's d.seq chain and
///    firstborn d.contains reflect THIS enumeration);
///  - rewrite attribute cells whose re-emitted value changed (the old attr
///    cell is orphaned garbage reclaimed at the next rewind/compaction);
///  - for keys in the map absent from this enumeration, ask the lens for
///    staleAttributes(key) and write those attributes (SqlLens answers
///    {{"d.state","stale"}}) — the framework never hardcodes a rank name;
///  - recompute roots().
/// Returns the count of newly minted node cells. Failures inside the mint
/// path are engine bugs (expectWritten); a failure to reach the lens is
/// caller-visible and leaves the arena unchanged.
[[nodiscard]] std::size_t refresh();
```

Union-on-identity is what makes replay safe: a row seen in page 1 and re-fetched in page 5 keeps its
cell — `keyToCell_` hits, no re-mint — so anything the session maps stays valid. Structure is
replace-on-refresh: a row that vanished is simply not re-asserted; the page's `d.seq`/`d.contains`
reflect *this* fetch, and the missing key gets the lens's stale attributes — the cell persists (the
arena is append-only; nothing un-mints outside a release) but no live page reaches it. `refresh()`
is framework code, not lens code: the same loop serves `ProcLens::refreshFromHost` and any future
live lens — the current one-shot `project()` becomes the first `refresh()`.

### 5.3 A page is a cell that knows how to re-fetch itself

Each page cell carries its replay parameters — `d.winlo`/`d.winhi` (the window's sort-key bounds as
typed `<sql-value>` encodings, not display strings — rebinding needs the type: `'1042'` ≠ `1042`
under SQLite affinity), `d.wincount`, `d.qhash`, `d.state` — so a page is self-describing. The lens
also keeps the typed `SqlValue`s in its page buffer; the attributes are the durable/display form of
the same data. The three replay semantics, all against the recorded parameters:

1. **Live replay.** Re-execute the window fetch against current data. Cheapest; sees inserts and
   deletes; the honest default.
1. **Keyset replay** (browse mode only). Re-prepare the recorded window SQL with `winlo`/`winhi`
   bound — `WHERE (sort cols) BETWEEN (:lo) AND (:hi)` as row-value predicates — re-fetching *the
   same rows by key*, stable under concurrent insertions elsewhere. A shortfall vs `d.wincount`
   means rows were deleted and shows as fewer live rows, not as wrong ones.
1. **Snapshot replay.** Inside one consistent-read scope every window reads one version — backward
   paging re-reads bytes already read, deterministically. The mechanics differ honestly by backend
   (§7.2, §7.3): SQLite gets it only in **WAL mode** (a held read transaction pins a snapshot at
   first read — `BEGIN DEFERRED` itself pins nothing; a `PRAGMA journal_mode` conversion is a
   persistent header write a read-only connection cannot make, so rollback-journal databases get no
   snapshot replay and the page's `d.state` says so — a held SHARED lock there would deny every
   writer); PostgreSQL gets it from the transaction wrapping its cursor/portal. `SQLITE_SCHEMA` from
   concurrent DDL maps to `d.state=stale` + a `refresh()`. `PRAGMA data_version` is the cheap "did
   another connection commit" probe; `sqlite3_snapshot_*` (build probe) is the round-two
   transfer-capable option.

### 5.4 Deferred pages and the stub horizon

`forEachNode` cannot yield a stub per page — without an eager `COUNT(*)` the page count is unknown,
and computing it is a scan the user didn't ask for. The enumeration is therefore a **bounded stub
horizon**: the lens emits one `sql.page` stub per materialized window *plus one trailing stub*
carrying `d.state=deferred` and the next window's `d.winlo` (the last seen sort key, or the next
`OFFSET`). Materializing a stub is `materializePage(pageKey)` on the lens followed by
`session.refresh()`; the trailing stub is re-emitted further along each refresh until a fetch comes
up short, at which point it is simply absent — the end of the result is represented by *no stub*,
not a marker. A page-cell state is only ever `deferred`/`live` — there is no `tossed` value, because
a tossed page's row cells are gone (§5.5) and the stub, minted *below* all checkpoints at projection
time, reverts to `deferred` when the trail replay restores its pre-checkpoint `d.state` attribute. A
`count(*)` request is itself a replayable page of role `sql.count`, minted on demand.

Stub cells are minted during initial `project()`, *below* every checkpoint — that is what lets them
survive a rewind and revert cleanly (§5.5).

### 5.5 Rewind: `checkpoint()`/`rewind()` — a stack, with map repair

`mark()`/`release()` are strictly LIFO and `release` is contiguous truncation plus trail replay
(§2.4): the session's rewind must be a stack too, and it must repair what the arena doesn't own.

```cpp
/// One rewind boundary. High-waters let rewind() forget exactly what was
/// minted after the mark: keyToCell_/cellToKey_ entries (by mint order),
/// roots_ entries, and the checkpoint stack itself. Dimensions are NOT
/// checkpointed — they must only ever be minted via findOrMintDimension
/// (§3.2), whose d.dims rank survives a release.
struct Checkpoint {
  ArenaManifold::Mark mark;
  std::size_t keyHighWater;
  std::size_t rootsHighWater;
};

[[nodiscard]] Checkpoint checkpoint();

/// Pop every checkpoint above @p cp, then release(cp.mark): tosses all cells
/// minted after it, truncates the identity maps and roots_ to their
/// high-waters, and reverses trail entries (a stub's d.state attr reverts to
/// deferred; a link re-asserted since is restored). Refuses — naming the
/// pinned key — if any pinned cell was minted after cp (a selection, a draft
/// annotation anchor): there is no selective skip, because release() is
/// contiguous truncation.
std::expected<void, std::string> rewind(const Checkpoint &cp);
```

The rules the design lives by, each forced by the machinery:

- **Stack discipline.** `rewind(cp_k)` first discards `cp_{k+1..N}` then releases `cp_k` — passing a
  stale mark to `release()` while newer marks are outstanding is not a wrong-answer bug, it is
  vector-growth corruption (a mark recorded with larger sizes than the truncated vectors makes
  `resize` *fabricate* slots). Scroll checkpoints are also **outermost**: they may be taken and
  released only while no solver choice point (`mark()` from vlog/vprolog evaluation) is outstanding,
  since marks are one shared stack.
- **CellRef aliasing.** Dense indices are reused after a release, so a CellRef held across a rewind
  does not dangle — it *aliases* a different cell. Cross-epoch identity is `ExternalKey` only: every
  consumer re-resolves `cellFor(key)` after `refresh()`/`rewind()` rather than caching a `CellRef`,
  and the session may expose an epoch counter for cheap staleness checks. (The first draft claimed
  replay produces "a new set of CellRefs"; the truth — same indices, new occupants — is worse and is
  why the rule exists.)
- **Pin = refuse, never skip.** A pinned key minted after `cp` makes `rewind(cp)` return
  `std::unexpected` naming it; the caller may rewind to a deeper checkpoint or promote the pinned
  subject first (§6). "Skipped by rewind" is deleted — contiguous truncation cannot skip.
- **Cost model.** While any mark is outstanding, writes to pre-mark cells trail — the slot *and* its
  link/content runs copy to the arena tail — and `compact()` is refused. Two consequences: attribute
  rewrites during `refresh()` (a `d.state` flip) mint an orphaned attr cell each, reclaimed only by
  the next rewind or compaction; and checkpoints must be **short-lived** — a long-lived checkpoint
  under an actively refreshing lens pays trail+relocation per touched cell with no reclamation.
- **Overlay boundary.** A checkpoint must not straddle writes to non-projected cells — base-overlay
  shadows, attached-space proxies, quote occurrences — because `release()` un-shadows/un-mints those
  along with the window. The mint-order log suffices for the two identity maps (attribute cells were
  never mapped — `cellToKey_` records node cells only), but any other `CellRef`-keyed state the
  session grows must get the same high-water repair.

**Scrollback policy (honest bound).** Forward scrolling accumulates mints until a rewind — a deque
of K checkpoints gives K pages of backward reach; scrolling past the horizon is a *slide*: note the
current page key, `rewind` to the horizon floor, re-materialize from there. Backward history is
sacrificed, never skipped around; re-fetching is a replay (§5.3). Pinned pages stop a slide at their
checkpoint (refuse-and-name).

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
([`arena_manifold.hpp:834`](apps/common/xanadu/zigzag/arena_manifold.hpp)). Nothing in the lens
promotes by itself: **promotion is a user gesture**, which is what keeps convergence R8 intact —
browsing and replaying mint zero ops anywhere.

### 6.1 The staging envelope: why `promote(row)` alone cannot work

`promote()` walks *every* link of every reached cell, both directions (§2.2): from a `sql.row` cell
the walk climbs negward `d.contains` to page→query→conn, runs `d.seq` to every sibling, descends
`d.conforms`/`d.ofcol` to the entire schema — the whole projection is one connected component, so
`promote(rowCell)` would write *the whole materialized database* to the user's store (or exceed
`PromotionBudget` and return `std::nullopt` — silently nothing). Envelope cells linked *into* the
projection don't help: the walk follows them right back out.

The mechanism that works without changing `promote()` is a **staging envelope**: for each promote
gesture the session mints a small, *disconnected* subgraph in the arena — a `sql.annot` envelope
cell holding copies of the subject: the row's display-key content, its value cells (copied with
their `d.column`/`d.type`/`d.null`/`d.extkey`/`d.keystab` attributes), and for `promotePage` one
envelope containing the window's rows on `d.contains`/`d.seq`. The envelope links to **nothing** in
the projection, so `promote(store, parent, arena, envelope, budget)` persists exactly the intended
set — "promote the answer, not the search" restored by construction. (The alternative — a
`promote()` variant taking an expand-predicate or dimension stoplist — is noted as a cleaner
framework primitive for trait v2; the envelope needs no signature change.)

### 6.2 `promoteRow` / `promotePage`

Both gestures run the same `promoteSubject(key)` path with a different copy set: a row's subtree, or
a page's rows. Blobs under a promoted row are imported via `importFileStore` at this point — the
first moment their bytes legitimately enter the permascroll (§3.5). `d.null` travels on promoted
value cells and is the *sole* NULL marker on the durable side (§3.4) — annotation code must read it,
not the cell content.

### 6.3 Idempotent promotion: the reconcile map and the `sql.promoted` anchor

`promote()` knows nothing about `ExternalKey`s, so the session keeps a third map:
`ExternalKey → (Store*, opIndex)` — a *local* op reference, not `GlobalOpRef`, because a private
never-published annotation store has no seal to globalize under (`GlobalOpRef` needs `sealedAs`);
globalization happens lazily if the store is ever published. The gesture consults it first:
re-annotating an already-promoted row resolves to the *existing* durable cell — the difference
between "annotate the invoice" and "annotate a copy of the invoice."

The map must be rebuildable on open, and "walk every cell looking for `d.extkey`" is O(store) — a
generic attr rank other lenses mint too, so the walk can't even early-exit. Resolution: promoted
envelopes link under a **`sql.promoted` headcell rank in the annotation store**, one rank per
projected database (keyed by `<dbid>`) — rebuild walks that one rank, O(promoted subjects), reading
each cell's `d.extkey`. This is the lens doc's §5.3 option (b) applied with the promote-skip caveat
answered: `d.extkey` is minted on the *durable* copy (by the envelope, at staging — it need not
survive `promote()`'s traversal of the projection, because the envelope *is* the durable shape), so
the skip debate doesn't apply to it.

### 6.4 What annotation may assume

The contract annotation code gets, stated once:

- **`pk:` rows**: re-fetches find the same row; an annotation pinned to the durable cell stays
  meaningful across refresh, reorder, and window toss.
- **`rid:` rows**: identity is positional-in-table; reuse and `VACUUM` can rebind it — annotations
  carry the caveat on `d.keystab` and reconciliation is user-confirmed (U-S3).
- **`h:` rows**: the annotation is pinned to *content*; an update mints a new key and the annotation
  visibly stops matching live data — honest staleness, shown as such.
- **`ord:` rows**: not emitted round one (§4.2); if ever emitted, `d.keystab=ordinal` warns that the
  identity is position only.

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

/// One active read cursor: RAII over the prepared statement. Move-only;
/// destroyed at end of window scope. fetchNext reads the cursor forward —
/// the live-window path that preserves the query's own ORDER BY.
class SqlCursor {
public:
  virtual ~SqlCursor() = default;
  [[nodiscard]] virtual const std::vector<SqlColumn> &columns() const = 0;
  [[nodiscard]] virtual std::expected<std::vector<std::vector<SqlValue>>,
                                      std::string>
  fetchNext(std::size_t limit) = 0;
};

/// A consistent-read scope (§5.3 replay-3): RAII transaction/portal guard.
class SqlSnapshot {
public:
  virtual ~SqlSnapshot() = default;
};

class SqlBackend {
public:
  virtual ~SqlBackend() = default;

  [[nodiscard]] virtual std::expected<std::vector<SqlTable>, std::string>
  schema() = 0;

  /// Prepare a read-only statement. Refuses anything that writes (§7.2) and
  /// refuses multi-statement input (tail must be empty). Refuses with
  /// unexpected while another cursor or a snapshot is live — one connection
  /// serializes its reads; coexisting prepared stmts are allowed, a second
  /// ACTIVE cursor is not, because window state must be unambiguous.
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
  /// (rollback-journal SQLite). Scopes a cursor's reads to one version.
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

### 7.2 `SqliteBackend` (round one)

- **Open.** `sqlite3_open_v2(path, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX)` —
  read-only, URI-parsing (enables `file:…?immutable=1` for truly-static files), NOMUTEX because the
  backend is single-threaded by contract (§7.1). Minimum SQLite 3.22: a read-only open over a hot
  `-wal` needs the heap-memory wal-index fallback or it fails on a read-only directory — state the
  floor and return the error cleanly.
- **Prepare-time gates, in order:** prepare the raw SQL; refuse a `NULL` stmt (comment-only/empty
  input); check `sqlite3_stmt_readonly()` — it gates *"doesn't write"*, which is the right contract
  (it accepts `VALUES`, `EXPLAIN`, transaction controls, and correctly refuses `WITH…UPDATE`; SQLite
  has no DML-in-CTE syntax, so no hole there); require the `pzTail` remainder empty (single
  statement — a `;`-terminated second statement never reaches the authorizer); *then* wrap/replay.
- **Authorizer** (`sqlite3_set_authorizer`), installed *after* the connection's own pragmas run (a
  blanket `SQLITE_PRAGMA` deny would silently neuter `PRAGMA query_only` in the open sequence):
  allow `SELECT`, `READ`, `RECURSIVE`, `TRANSACTION`, `SAVEPOINT` (snapshot/`BEGIN` would otherwise
  die on the deny — the first draft's allowlist broke its own `snapshot()`), `FUNCTION` with a
  per-name denylist (`load_extension`, `writefile`, `readfile`, `fts3_tokenizer`), and
  `SQLITE_PRAGMA` against a read-only name allowlist (`table_xinfo`, `foreign_key_list`,
  `data_version`, `query_only`, `database_list`; never `writable_schema`, `journal_mode`,
  `trusted_schema`). Explicitly deny `ATTACH`/`DETACH` — load-bearing for the one-file `<dbid>`
  grammar (§4.5). All DDL/DML codes fall through unlisted → denied.
- **Connection pragmas:** `PRAGMA query_only = ON` (belt-and-braces over the authorizer).
- **Cancellation:** `sqlite3_busy_timeout` (it *is* an internally installed busy handler — pick it,
  not a custom one) plus a progress handler, so a runaway query or a WAL-locked database returns
  `std::unexpected` instead of hanging the UI thread.
- **Provenance (§4.3):** `table_xinfo` for PK positions and generated columns;
  `sqlite3_column_table_name`/`origin_name` behind the link probe; their absence degrades *query*
  mode to `h:` while browse keeps `pk:` (the lens knows its table). Column case comes from
  `sqlite_schema`.
- **Snapshots:** WAL-only per §5.3 — a held `BEGIN DEFERRED` read txn pins a snapshot at first read
  and never blocks writers (but pins the WAL file — a long-lived snapshot grows `-wal`, so snapshot
  scope is bounded like a checkpoint); rollback-journal databases refuse `snapshot()`.
- **Blobs:** fetched into the `SqlValue::bytes` budget at fetch time (the variant forces it — §3.5);
  over-budget re-fetch via `sqlite3_blob_open`/`SELECT … WHERE rowid` only for `pk:`/`rid:` rows.
- **Concurrency:** one `sqlite3*`, one thread — the app owns threading; coexisting prepared
  statements interleave fine, so `schema()`, `snapshot()` and blob point-selects share the
  connection.

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
- **Windows are server-side.** `DECLARE CURSOR` must run inside a transaction block (in autocommit
  it errors), so a snapshot scope is `BEGIN` + `SET TRANSACTION READ ONLY` + `DECLARE`, and a
  `NO SCROLL` cursor can't go backward — rewind/replay is re-`DECLARE`, or `SCROLL` where needed.
  `DECLARE` cannot bind `$n` parameters — the parameterized path is extended-protocol **portals**
  (Bind/Execute with a row limit; a suspended portal *is* a parameterized server cursor), used for
  user queries; literal browse SQL may use `DECLARE` directly.
- **Provenance:** `PQftable`→`pg_class` OID, `PQftablecol` attnums joined to `pg_constraint.conkey`
  (`contype='p'`; `contype='u'` UNIQUE+NOT NULL is a legitimate second identity rung — noted, not
  required); `PQftype` for `d.type` (not `PQfmod`, which is the type *modifier*); `relkind='v'`
  marks views → `h:` keys; no rowid analogue exists — `ctid` changes on every update including HOT,
  so `rid:` never applies to PG.
- **Values:** `PQgetvalue` text is GUC-dependent — pin `DateStyle`, `IntervalStyle`,
  `extra_float_digits`, `bytea_output` at connect (§4.5) or run a binary cursor; `bytea` arrives as
  `\x` hex — decode into `SqlValue::bytes`; large objects (`lo_*`) are OID references, not `bytea` —
  round one renders them as integer OIDs and says so.
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
and query text in a URI leaks via `ps`). The interactive surface the gestures live in is the xuzz
slice view (browse/page/rewind/promote) and `vquery` (the §9 traversals); the `--lens` raster path
exists for smoke output only.

______________________________________________________________________

## 8. Round One Is Read-Only — and What the Write Phase Looks Like

### 8.1 The read-only contract

Round one overrides **nothing** in the unproject half: `applyContentChange`/`applyAttributeChange`
stay at their `std::unexpected("this lens is read-only")` defaults
([`projection_lens.hpp:122-133`](apps/common/xanadu/zigzag/projection_lens.hpp)). Enforcement is
three-layered: the trait defaults refuse; the backend refuses writes at prepare + authorizer +
read-only open (§7.2/§7.3); and the UI offers no edit gesture on `sql.*` cells. The dirty-cell/
`commit()` path of the lens doc §6 is **not part of round one** — `ProjectionSession` has no
`commit()` yet anyway (§2.5).

### 8.2 Phase two, scoped against §8A

When writing arrives, the §8A additions map onto SQL mechanically:

| §8A gap                             | SQL lowering                                                                                  |
| ----------------------------------- | --------------------------------------------------------------------------------------------- |
| `beginBatch`/`endBatch`             | one SQL transaction per `commit()` — rollback, not partial failure                            |
| batched `applyCreate`               | one `INSERT` per created row, all value cells in one statement                                |
| edge properties + `applyEdgeChange` | FK constraint names/actions on `d.ref`/`d.conforms` ranks                                     |
| schema-vs-instance `NodeKind`       | expressed as `d.conforms`/`d.ofcol` headcell ranks (§3.6) — adopted in r1                     |
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
#row/d.contains/d.seq              -- a row's value cells, in select order
[/d.role = 'sql.row']              -- restrict to row cells
[/d.column = 'amount']             -- one field across the materialized set
[/d.keystab = 'pk']                -- rows whose identity survives refresh
```

run identically over a projected database, a projected directory, and a hand-built slice — the
`lens_vql_gate_test.cpp` equivalence is the gate to extend (`tests/xuzz/sql_lens_test.cpp` reusing
the same assert-both-shapes pattern). vprolog's `cell_link(Row,'d.contains',pos,V)` enumerates a
row's fields as facts with no SQL awareness; `extkey/2` (the lens doc §9A.3 predicate — currently a
doc-comment contract on `ProjectionSession::keyFor`,
[`projection_session.hpp:42-44`](apps/common/xanadu/zigzag/projection_session.hpp)) is what lets a
goal recover `"sql:sqlite:…:pk:i:1042"` from a bound cell — *more* load-bearing for SQL than for any
prior lens, because the ExternalKey is how a logic fact names a database row without an ops index.
Membership ranks mean conformance is a goal too: `cell_link(Col,'d.ofcol',pos,V)` enumerates a
column's values; `cell_link(Tab,'d.conforms',pos,R)` its rows.

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
   `schema()`, cursor `fetchNext`, `replayWindow`, the provenance probe, WAL-snapshot support. Unit
   tests over a temp `.db`: non-SELECT refused, `ATTACH` refused, `BEGIN` under the authorizer
   allowed, ordinal and keyset windows deterministic, blobs budgeted.
1. **`ProjectionSession::refresh()` + `checkpoint()`/`rewind()`.** The union-identity re-projection
   with the `staleAttributes` hook, and the stack-disciplined rewind with high-water map/roots
   repair and pin-refusal (§5.2, §5.5) — tested with an in-memory lens whose node set mutates: same
   key → same CellRef, dropped key → stale attrs, rewind → keys forgotten + roots trimmed, pinned
   key → refused. **Highest-risk item, deliberately early.**
1. **`SqlLens` browse mode.** Schema projection (tables/columns + membership ranks), the stub
   horizon, `materializePage`, `pk:`/`rid:`/`h:` keys with typed renderings, `d.extkey`/`d.keystab`/
   `d.dupcount`/`d.null` attributes. Gate: a known fixture `.db` projects a known cell layout
   deterministically; `lens_vql_gate`-style tests run the same VQL strings over the projection and a
   hand-built slice.
1. **Query mode + replay.** The `sql:` URI + `--sql` flag (§7.4), `h:` keys, ordinal windows, the
   three replay semantics (§5.3) with fixtures asserting keyset replay returns identical rows under
   a concurrent inserter, and WAL-vs-rollback snapshot honesty.
1. **Promotion + the reconcile map.** The staging `promoteSubject` envelope (§6.1), the
   `sql.promoted` anchor rank and `ExternalKey → (Store*, opIndex)` map rebuilt on open (§6.3), blob
   import at promote time. Gate: annotate a `pk:` row, close, re-project, re-derive the same durable
   cell; `rid:`/`h:` rows show their `d.keystab` caveat.
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

| aspect                            | behaviour                                                        | faithful?                           |
| --------------------------------- | ---------------------------------------------------------------- | ----------------------------------- |
| keyed row identity across refresh | `pk:`/`rid:` key → same cell after `refresh()`                   | faithful (class-dependent, §6.4)    |
| unkeyed row identity              | `h:` hash — survives reorder, dies on update                     | faithful, marked (`d.keystab`)      |
| a row in two different queries    | same `(stable,pk)` → same `pk:` key → one cell                   | faithful by construction (§4.4)     |
| duplicate subjects in one result  | one cell + `d.dupcount`, any class                               | faithful to subject, not rowcount   |
| NULL vs `''` vs `0`               | `d.null` flag + `d.type`; survives promote only via the flag     | faithful                            |
| BLOB                              | budgeted fetch at read; bytes at open/promote; `pk:`-only reopen | faithful, bounded (§3.5)            |
| window under concurrent insert    | keyset replay: same rows; ordinal: shifts honestly               | faithful in browse; honest in query |
| deleted row after replay          | stale-marked via `staleAttributes`, cell persists                | honest (append-only arena)          |
| total row count                   | never computed eagerly; stub horizon + on-demand `sql.count`     | honest bound                        |
| implicit rowid reuse / `VACUUM`   | own class `rid:`, caveat on `d.keystab`                          | **lossy on identity**, marked       |
| `WITHOUT ROWID` / temp / view     | declared PK only, else `h:`                                      | faithful                            |
| aggregate/computed column         | `d.ofcol` to a result-column cell; `h:` keys                     | faithful — no base identity exists  |
| FK to an unmaterialized row       | rank membership skipped until both ends exist                    | honest omission, §3.6               |
| concurrent writer                 | WAL: snapshot clean; rollback journal: snapshot refused          | bounded, no hang                    |
| schema change mid-session         | `SQLITE_SCHEMA` → `d.state=stale` + `refresh()`                  | honest re-projection                |
| CellRef held across a rewind      | aliases a different cell — consumers re-resolve by key           | honest hazard, stated (§5.5)        |

"Faithful" means: the projection shows what the query returned (up to subject-level dedup, which is
marked by `d.dupcount`), identity means what `d.keystab` says it means, and a refresh never silently
transfers an annotation. The genuinely lossy row — implicit-rowid identity — is a source limitation,
not a projector limitation, and the ladder's marking keeps it honest rather than hidden.

______________________________________________________________________

## 12. Open Questions

- **U-S1. Cursor lifetime.** One `SqlCursor` per window scope is cheap to re-prepare for replay but
  pins nothing; holding one open preserves `ORDER BY` naturally for live paging. The port (§7.1)
  makes the cursor RAII and the snapshot explicit, which answers the mechanism — what remains open
  is the *policy*: how long a live cursor may idle before the lens drops it and pays a re-prepare.

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
  question every lens's promote gesture asks); this spec needs only that the store exists and the
  `sql.promoted` rank (§6.3) can be walked.

- **U-S6. Serving a lens over 9Z.** `zz9p` already defines the wire pairs; whether PostgreSQL
  eventually arrives as a remote `SqlLens` server (the database *is* the manifold) rather than a
  linked backend changes where the port sits but not the key scheme — deferred, deliberately.

- **Resolved since rev 1.** U-S2 (implicit-rowid honesty) is answered: `rid:` is its own stability
  class, §4.2.

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
  `ord:` to reserved; made key class orthogonal to mode; added subjects-not-occurrences dedup with
  `d.dupcount`; schema-qualified `<stable>` names; ATTACH denied to keep `<dbid>` single-file.
  Backend corrections: full authorizer action list (`TRANSACTION`/`SAVEPOINT`/read-`PRAGMA`
  allowlist — the first draft denied the `BEGIN` its own snapshot needed); cursor/snapshot as RAII
  objects on the port; composite keyset via tuple predicates; blob byte-budget instead of impossible
  laziness; WAL-vs-rollback-journal snapshot honesty; PG portal/transaction/`relkind`/
  `PQftype`/`bytea`/GUC details; `default_transaction_read_only` downgraded to defense-in-depth.
  Documentation: reconciled the mode/key-class table, the `d.state` value set, rank-table
  completeness (`d.pk`/`d.notnull`/`sql.count`/`sql.annot`), the two R8s, lens-doc-vs-local § refs,
  the `swarm_catalog_index` sandbox overclaim, and the reconcile-map rebuild path (`sql.promoted`
  anchor rank, local `(Store*, opIndex)` rather than `GlobalOpRef`).
