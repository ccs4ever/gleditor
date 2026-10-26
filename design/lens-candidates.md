# Anything-to-ZigZag: Candidate Lenses and the Lens-Type Taxonomy

Design note, not a spec. Records a brainstorm over what else the `ProjectionLens` trait can serve,
groups the candidates into a taxonomy by the machinery each needs, and recommends sequencing. Where
`design/sql-to-zigzag.md` is a full design for one lens, this document is the map of the territory
around it — each candidate named here would earn its own spec when scheduled.

______________________________________________________________________

## 1. The inventory, and the shape of the trait

Lenses and lens-adjacent machinery, honestly labeled:

| component         | source                           | status                                              | what it teaches the machinery                                                                                                                                               |
| ----------------- | -------------------------------- | --------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `FilesystemLens`  | a directory tree                 | **landed** `ProjectionLens`                         | `stat`-based `ExternalKey`s, `d.size`/`d.mtime`/`d.perm`/`d.name` attrs, symlinks on `d.ref`                                                                                |
| `ProcLens`        | `/proc` process trees            | **landed** `ProjectionLens`                         | `d.child`/`d.sibling` firstborn encoding, `d.threads`, scalar attrs; `refreshFromHost` is the manual re-read precedent                                                      |
| `NetLens`         | network connections              | **landed** `ProjectionLens`                         | `d.instance` prototypes, `d.endpoint`, connection `d.state`; state is *fed* by the transport owner, pull-style                                                              |
| `MeshLensChannel` | BEP-10 wire cells                | **landed, lens-adjacent**                           | proxy cells + async `pollIncoming`/`requestMaterialization` — the closest thing to a landed push-driven pump                                                                |
| `ReconcileLens`   | declared vs actual service state | **landed, not a `ProjectionLens`**                  | a control-loop engine writing `d.desired` into an `ArenaManifold` directly and diffing it against live state — evidence the reconcile *pattern* works, not a trait instance |
| `XanadocLens`     | the store's own documents        | **planned**                                         | the parent lens doc's first deliverable; today's xanadoc→slice path is the pre-trait `xanadu_projector`                                                                     |
| `SqlLens`         | SQL queries/results              | **spec'd** ([`sql-to-zigzag.md`](sql-to-zigzag.md)) | windowing, replay, promotion, `WindowedLens`                                                                                                                                |

The trait as it stands ([`projection_lens.hpp`](apps/common/xanadu/zigzag/projection_lens.hpp)):
`dimensions()` up front; pull-based `forEachNode`/`forEachEdge` enumeration; the unproject half
(`applyContentChange`/`applyAttributeChange`, :118–133) defaults to refuse — so "read-only" is the
free state for any lens. The `sql-to-zigzag` spec proposes the windowed-session additions:
`refresh()` union-identity re-projection, `checkpoint`/`rewind`/`pin`, `staleAttributes`,
lens-declared `forEachRoot`/`roots`, and the `WindowedLens` sub-interface (`materializePage`,
`discardMaterializedAfter`). Nothing below assumes any of that is landed — each candidate names
which additions it needs.

______________________________________________________________________

## 2. The taxonomy

Two axes — **boundedness** (can `forEachNode` see the whole source cheaply) and **freshness** (does
the source push changes or only answer pulls) — give a 2×2 with all four cells populated (Evented
spans both pushed cells):

| cell                               | trait surface needed              | precedent                                                                       | notes                                |
| ---------------------------------- | --------------------------------- | ------------------------------------------------------------------------------- | ------------------------------------ |
| **Enumeration** (bounded, pulled)  | the trait + `refresh()`           | Filesystem, Proc                                                                | today's model                        |
| **Windowed** (unbounded, pulled)   | + `WindowedLens`                  | SqlLens (spec'd)                                                                | keyset/ordinal windows + page buffer |
| **Evented** (pushed, either bound) | + a change-notification path (§4) | `MeshLensChannel::pollIncoming`, `ProcLens::refreshFromHost` (both hand-rolled) | the uncovered cells                  |

Three orthogonal properties sit outside the matrix — every candidate *also* has a topology, a
provenance, and a subject, and the interesting ones are compound (`SwarmLens` is windowed *and*
evented):

- **Composite** — more than one source in an arena. `ProjectionSession` today takes exactly one
  `unique_ptr<ProjectionLens>`
  ([`projection_session.hpp:35`](apps/common/xanadu/zigzag/projection_session.hpp)). One
  `ArenaManifold` can host several sessions trivially (it has no session ownership), but each
  session keeps its own `keyToCell_` — two lenses get no shared keyspace or arbitration; and
  `attach(Space)` federates a *foreign manifold's* cells, a different thing entirely. The pieces
  exist; the composite pattern is undemonstrated. `ReconcileLens` carries the desired-vs-actual
  *idea*, in non-trait form.
- **Derived** — a lens whose source is itself a projection or query result. Needs no new trait
  surface of its own, but does need **project-by-reference** (§3.3, U-L5): mapping a key onto an
  *existing* cell rather than minting a weak ephemeral twin.
- **Meta** — the store as its own subject. Same machinery, same project-by-reference need, plus a
  self-reference discipline (§3.3).

Two observations:

1. **`SqlLens` was the right first windowed lens.** `WindowedLens`, the stub horizon,
   `boundaryKey`-driven discard — all source-agnostic and the expensive half of every unbounded
   source below. The second windowed lens is far cheaper than the first.
1. **The trait's missing verb is notification, not enumeration.** Evented sources are the largest
   uncovered cells; §4 names what they actually cost — more than one trait line.

______________________________________________________________________

## 3. Candidate lenses

Ranked roughly by value-over-cost. Each entry names what makes it interesting, its key scheme, and
its cell(s) in the §2 matrix.

### 3.1 `GitLens` — commits, branches, trees, blobs

**Why:** git is already an append-only content-addressed DAG — the closest thing to a manifold in
the wild. Commits chain on `d.parent` (firstborn + siblings for merges — the canonical deg±≤1
encoding, same shape as `d.contains`+`d.seq`), trees→blobs on `d.contains`, file contents
budget-fetched like SQL blobs. Commit SHAs give pk-strength identity for free. **Type:** enumeration
for commits/trees/blobs (immutable); **pulled-mutable for refs** — branch tips move under
commit/rebase/fetch, and `refresh()`'s union identity is exactly what makes ref motion safe (ref
cell keyed by name, target edge re-asserted); windowed for `git log` (the backend pins log order —
topo vs date — or replay drifts). **Writability:** read-only first (annotate commits/files via
promote); `git add`/commit is a clean phase-two unproject. **Cost:** libgit2 or plumbing; moderate.

### 3.2 `ConfigLens` — YAML/JSON/TOML trees

**Why:** the *unproject* proving ground. A YAML file is a tiny bounded tree; keys are path-strings
(`/render/backend`); a scalar content edit never changes the path, so `applyContentChange` returns
the same key — the simplest possible exercise of the contract. **Type:** enumeration. **Hard
dependency:** the trait has `apply*`, but **nothing drives them** — `ProjectionSession` has no
`commit()`/dirty-diff yet (parent lens doc §6), so phase 0 for *any* writable lens is the session
growing that verb. Round one covers scalar edits only; adding/removing keys needs trait-v2
`applyCreate`/`applyDelete` (§8A of the lens doc). Caveats: YAML comments/anchors are fidelity
hazards (restrict to a canonical subset or accept reformatting); datetime values arrive as text —
`NodeContent` has no date variant, a display limitation only.

### 3.3 `MetaLens` / `OpSpoolLens` — the store projecting itself

**Why:** introspection in the system's own language — ops, spans, authorities, hypertime heads, the
dimension census as a navigable slice. **Type:** enumeration for store shape; evented for live
stores. **Keys:** op indices / `(doc, span)` triples — stable by construction. **Three constraints
the machinery imposes:**

- Enumerate a **frozen fold** — `store.rebuildManifold(fixedVersion)`, re-folded per `refresh()`; a
  live manifold appends ops mid-enumeration (the overlay contract applies to readers too).
- Sever the cycle by enumerating the store manifold, never the arena holding the projection.
- The payoff needs **project-by-reference** (`key → existing base CellRef`, U-L5): project-by-mint
  produces a disconnected copy of the store's structure — a read-only replica, not a reflection —
  and annotating a twin writes duplicate ops into the store that already holds the original.

### 3.4 `LogLens` — journald / app logs

**Why:** time-ordered ranks are natural slices; "most recent N" is literally a window. The second
windowed lens — the cheapest possible proof that `WindowedLens` generalizes. **Type:** windowed
(ordinal, or keyset on timestamp+sequence); evented variant = tail via the §4 path. **Keys:**
journald's own cursor is pk-strength. **Cost:** small.

> **As implemented (this branch).** Two lenses: `LogLens` over a file (tail-as-window, `mtime+size`
> stamp + a `FileWatch` kernel event), and `JournalLens` over `sd-journal` (`journal:` URI, whole
> journal or `journal:<unit>`) — entries are cursor-keyed (`journal:l:<cursor>`), pages walk back
> from the tail in `pageSize` windows, `pollChanged()` is `sd_journal_process()` and `waitableFd()`
> is `sd_journal_get_fd` — the journal notifies at the kernel surface, no file stat needed.
> `lens.open` on a systemd-unit-named file (`.service`/`.socket`/`.timer`/…) mounts that unit's
> journal one hop down.

### 3.5 `VQLResultLens` — a query's answer set as a slice

**Why:** closes the loop — query the docuverse, get a slice, annotate it, promote the annotation.
**Type:** derived. **The real blocker is shared with MetaLens:** project-by-reference — the session
projects into the same arena the query runtime enumerates, so projecting answer rows by mint creates
a second, weaker ephemeral copy; `key → existing CellRef` is what makes a derived lens better than
the result it wraps. **Keys:** an answer row that resolves to a store cell gets durable identity
(pk-strength); a scratch-constructed row (computed tuples) bottoms out at `h:`/`ord:` — the honesty
ladder inherits the mechanism, not the strength.

### 3.6 `SwarmLens` — peers, torrents, pieces, the ledger

**Why:** the BitTorrent layer is a live distributed data structure with real identity (infohashes,
peer IDs) and real ranks (pieces of a torrent, votes in the Merkle identity ledger).
`MeshLensChannel` is the precedent that wire-visible cells can live in the space. **Type:** windowed
*and* evented — the compound case that makes the §4 epoch question concrete. **Cost:** biggest scope
on the list; the ledger and the peer tables are two separate slices. Schedule late but keep.

### 3.7 `MailLens` / `CalendarLens` — Maildir or IMAP; iCal

**Why:** real identity (Message-ID, UID) and natural dims (`d.references`, `d.attendee`,
`d.inreplyto`). **Type:** windowed (folders are unbounded); enumeration for calendar. **Cost:**
moderate — Maildir parsing is easy, IMAP is the auth/state tax.

### 3.8 `MediaMetaLens` — exif/id3/codec metadata

**Why:** metadata is structure, bytes are permascroll — the separation the model already endorses
(budgeted blobs). Shares `FilesystemLens`'s `stat` key scheme — the concrete case for cross-lens key
unification (U-L1). **Type:** enumeration. **Cost:** small.

### 3.9 `ActivityLens` — `system://activity` walks

**Why:** the reader's own trail as a navigable, annotatable slice. **Type:** enumeration on a
bounded store; evented as visits append. **Cost:** small.

### 3.10 `MicropaymentLens` — the ledger idea

**Why:** Nelson's transcopyright micropayments made navigable. **Type:** evented + windowed.
**Status:** phase-two material — named for the record, not scheduled.

______________________________________________________________________

## 4. The evented cells, stated honestly

`notifyChanged()` as a lone trait hook is the small part. The real design is the path a source event
walks — and the direction is **lens → app → session**, not app → lens:

```text
source event (inotify, PQconsumeInput, swarm pump, op-append)
  → lens-side capture: the lens owns its source watch; the event lands on the
    SOURCE's thread, so the lens records dirty and snapshots one epoch of pending
    deltas — it never touches the arena itself (a trait lens mints nothing, so
    source-thread events can't write the arena; anything push-adjacent that
    DOES mint — mesh proxies, solver cells — is exactly what the foreign-mint
    refusal exists to catch, so pushed mints must be funneled to the session
    thread too)
  → app-side funnel: marshal to the session thread and schedule refresh per
    the lens-supplied debounce policy (U-L2: the lens coalesces source events
    into one pending epoch; the funnel coalesces epoch-pending signals into
    refresh calls — an inotify storm must not schedule a refresh per inode)
  → session.refresh() at a marks-quiesced point: NO foreign mark outstanding —
    a solver mark's release() would truncate the refresh's mints while the
    session's maps keep stale entries (silent aliasing, not a refusal); scroll
    checkpoints the session owns are fine by LIFO
```

The wiring is named, not assumed: `notifyChanged()`/`poll()` on the lens records the event (the
`refreshFromHost` precedent — mutate lens state, session re-enumerates); the app polls or receives
it and drives `session.refresh()`. For evented+windowed lenses (SwarmLens, LogLens-tail) the epoch
question is the `d.snapscope` analog: the lens snapshots pending deltas at event boundaries so a
refresh consumes exactly one source epoch — an event landing mid-window can't mix two epochs into
one page.

______________________________________________________________________

## 5. Sequencing

| order | lens                         | what it proves                                      | needs                                                 |
| ----- | ---------------------------- | --------------------------------------------------- | ----------------------------------------------------- |
| 0     | —                            | session `commit()`/dirty-diff                       | phase 0 for any writable lens (lens doc §6)           |
| 1     | `ConfigLens`                 | the unproject half works — smallest writable source | scalar `apply*` only                                  |
| 2     | `LogLens`                    | `WindowedLens` generalizes — second windowed lens   | the SqlLens session additions                         |
| 3     | `GitLens`                    | DAG-native sources project beautifully; real value  | `refresh()`; windowed log                             |
| 4     | `MetaLens`/`OpSpoolLens`     | introspection; self-reference discipline            | project-by-reference (U-L5); evented variant needs §4 |
| 5     | `VQLResultLens`              | the query→slice→annotate loop closes                | project-by-reference                                  |
| 6     | `SwarmLens`                  | evented + windowed at scale                         | §4 path + `WindowedLens`                              |
| 7     | Mail/Calendar/Media/Activity | standard fodder, each a small win                   | per-source; evented cells need §4                     |

`SqlLens` itself stays ahead of all of it — its framework additions
(`refresh()`/`checkpoint`/`rewind`/`WindowedLens`) are the substrate the pulled candidates reuse;
the §4 evented path is the substrate the pushed ones do.

______________________________________________________________________

## 6. Open questions

- **U-L1. Cross-lens key unification needs machinery, not just shared keys.** Under a naive
  multi-lens host (or one lens re-emitting a key), same-key emission today is a *silent
  half-projection*: `keyToCell_.emplace` keeps the first cell while the second node's cell is
  already minted — orphaned, with its edges cross-wired to the first cell via `cellFor`. Union needs
  (a) multi-lens hosting (`CompositeLens` or session-owned lists); (b) a defined same-key rule —
  "first node wins the cell; later same-key nodes contribute attributes/edges only, mint skipped" —
  implemented deliberately, plus `d.ref` unification edges when keys differ but referents coincide;
  (c) an **attribute-collision policy** — a cell holds one attr cell per dim, so a second lens's
  `d.role` on the shared cell evicts the first's by `link()` (namespace or last-wins, stated); (d)
  **per-lens presence sets** for staleness — a key dropped by lens A but still emitted by B must not
  go stale; and (e) **all-lenses-atomic refresh** — the tombstone diffs runs, not owners, so a
  per-lens refresh would sever the other lens's edges on a shared cell.
- **U-L2. Evented coalescing splits by stage, not owner.** The lens coalesces *source events into
  one pending epoch* (it knows the source's event rate and owns the capture); the app-side funnel
  coalesces *epoch-pending signals into refresh calls* (it knows the session's refresh cost). Two
  stages, two policies — the session just sees "one epoch pending."
- **U-L3. Derived-lens identity.** `VQLResultLens` rows inherit the honesty ladder's mechanism but
  not its strength: `pk:`-class only where a row resolves to a store cell (the
  `resolveSourceCell`-style hook U-L5 proposes); computed rows are `h:`/`ord:` and should say so on
  `d.keystab`.
- **U-L4. Meta-lens recursion.** Enumerating the store manifold (frozen fold), never the
  projection's own arena, severs the cycle; the first implementation should assert it.
- **U-L5. Project-by-reference is the shared missing primitive.** MetaLens and VQLResultLens both
  need `key → existing CellRef` in the identity map — the non-ephemeral identity branch `promote()`
  already takes would then map durable cells to themselves, turning promotion into a no-op where the
  twin already is the source. Two scope guards it needs: reference cells are **read-only in the
  session** — no mint-log entry, excluded from tombstone and attr-rewrite (diffing a base cell's run
  would shadow source structure); and the session arena must **overlay the source manifold** (the
  staging-arena mechanism) — a standalone arena can't name a base cell. One session feature unlocks
  both rows of the taxonomy.

______________________________________________________________________

## Change History

- **rev 1.** Recorded from a brainstorm session: the landed-lens inventory, the six-type taxonomy,
  ten ranked candidates, the `notifyChanged()` gap, and the sequencing table.
- **rev 2.** Folded a three-reviewer critique (technical writer, system architect, vortex/runtime).
  Inventory corrections: `XanadocLens` marked planned (the landed xanadoc path is the pre-trait
  projector); `ReconcileLens` marked non-trait (a control engine over an arena, not a
  `ProjectionLens`); `MeshLensChannel` added as the real push precedent; "mixed-source session"
  corrected to mechanism-landed/pattern-undemonstrated. Taxonomy reframed as a 2×2 (boundedness ×
  freshness, three populated cells) plus orthogonal properties (composite = topology, derived =
  provenance, meta = subject). The evented cells got their real design — lens→app→session direction,
  source-thread capture, marks-quiesced refresh, epoch snapshots for evented+windowed — replacing
  the overclaimed "one-hook" version. ConfigLens scoped to scalar edits with the `commit()`
  dependency named. U-L1 rewritten: same-key emission today is a silent half-projection; unification
  needs multi-lens hosting + a deliberate union rule. U-L5 added: project-by-reference is the shared
  missing primitive for the Meta and Derived rows.
- **rev 3.** Second critique round. Debounce ownership split by stage (lens: events→epochs; funnel:
  epochs→refresh calls). The "lens never mints" claim narrowed — trait lenses can't write the arena,
  but push-adjacent minters (mesh proxies, solver cells) are what the foreign-mint refusal exists to
  catch. U-L1 grew its sub-rules: attribute-collision policy, per-lens presence sets for staleness,
  all-lenses-atomic refresh. U-L5 grew its guards: reference cells are read-only/tombstone-exempt
  and need an overlay arena. `attach(Space)` corrected — it federates foreign manifold cells, not
  lens composition.
