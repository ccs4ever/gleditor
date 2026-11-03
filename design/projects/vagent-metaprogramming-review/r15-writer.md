# r15-writer — final coherence critique of `design/vagent-metaprogramming.md` v1.14

Scope: verification of my four r14 minors (the changelog program-side misgrouping, the
un-narrated §10.2 (v) + cite precision, the grafted-twin "append case" gloss, the un-mirrored
"no session resolver" qualifier), a final whole-document coherence pass (`d.refFor`/three-arm
endpoint terminology, `closed` arms (a)/(b)/(c) across glossary/§6.1/§6.6/frontier,
`cap$N`/`masterOf` vs the deleted `d.captured`-correlation vocabulary, v1.14 changelog
narration including its 1.13-entry corrections, `mdl`, tables/fences/anchors, status line),
and a new-defect sweep of the v1.14 deltas only (`f252c2b8` → `406d079b`, +122/−57).

## Verdicts on r14 items — all four fixed

| Finding | Verdict |
| --- | --- |
| Changelog program-side misgrouping | **Fixed.** The 1.13 entry (:3580-3586) now reads "external/state-side endpoints ride the slot's own `int64` ref position in-session or `*handleTarget` after promotion …, internal referents rebind through the old→new reify map, program-side endpoints ride the `+d.name →` dim-name arm re-resolved through `ensureDimension`" — the correct three-way split, matching §6.1's enumeration (:1157-1168) and genesis (e)'s scoping (:2225-2229, :2246-2249). |
| §10.2 (v) un-narrated + `store.cpp:124` cite | **Fixed.** §10.2's item (v) now cites `store.cpp:123` (:2861) — verified: `Store::Store()`'s no-id signature sits at `store.cpp:123`, `:124` is the `userPermascroll_` initializer as I reported. The (v) narration was backfilled into the 1.13 entry (:3600-3603) and the cite correction confessed in the 1.14 entry ("corrected from the `userPermascroll_` initializer line to the no-id signature (`store.cpp:123`)"). |
| Grafted-twin "reduces to the append case" gloss | **Fixed, exactly as proposed.** :1244-1247 now reads "a session hit on a grafted twin displaced nothing durable (grafts never fold back), so the replay re-matches the folded rank, appending where no same-named clause survives and displacing where one does" — the replay-displace caveat restored. The 1.13-entry gloss corrected to match (:3598-3600) and the 1.14 entry confesses the correction ("the 1.13 entry corrected to match"). |
| "Never a session lookup key" un-mirrored | **Fixed.** Glossary `patch` row (:136): "no session resolver consults it, `learn_plan`'s replay key and the `d.patch` rediscovery join do"; §6.1's replay sentence (:1240-1241): "the alias is replay-keyed, no session resolver consulting it"; and the primary definition site (:1201-1204) carries the full qualifier. Three sites, one claim. |

## Whole-document coherence — final pass

- **`d.refFor` / three-arm endpoint terminology uniform.** `d.refFor` is the record→slot
  correlation dim at :95, :1054, :1144, :1151, :2216, :2227, :2814 (sketch), :3028 region,
  :3533, :3586 — no stale enumeration. The three referent classes are stated identically at
  §6.1 (:1157-1168), the head diagram (:1057-1061), genesis (e) (:2225-2229, :2241-2249):
  internal → slot `int64` + old→new map (+`d.values` conditional on a free `-d.values`);
  external/state-side → slot `int64` in-session / `*handleTarget` at (d); program-side →
  `+d.name →` dim name re-resolved through `ensureDimension`.
- **`closed` arms (a)/(b)/(c) consistent.** §6.6 (:1851-1865) enumerates (a) state-side
  endpoints however carried (`cap$N` link or slot-carried `int64`), (b) `+d.refs`
  name-records, (c) program-side dim referents via the paired ref-capture record's `+d.name`
  — the REF-record/`+d.name` mutual exclusion ("a `+d.name` there would make it a name-record
  and skip the marker ceremony", :1861-1862) composes correctly with the diagram's
  NAME-record/REF-record split (:1046-1051) and the marker rule (:1063-1065). The glossary
  row (:137) states the same partition unlettered; §6.1's frontier note (:1171-1173) and the
  frontier enumeration (:1880-1882) and re-verify walk (:1887-1889) all read endpoints off the
  paired slot's content or the paired record's `+d.name`. The refused class is uniform: an
  unnamed carrier landing program-side.
- **`cap$N`/`masterOf` vs deleted-correlation vocabulary disambiguated.** "Capture
  records" (cap$N machinery) and "ref-capture records" (the `+d.recipe` pairing) are distinct
  terms everywhere (:1053, :1142-1143). Every remaining "capture link/edge" is
  `cap$N`-qualified — the real edge class, decoded `asInt64` fails → `masterOf(slot)`
  (`vortex_core.cpp:356-365`, verified — the operand lambda is :359-364). `d.captured`
  survives only as annotated changelog history (:3536-3539, :3575-3578). The R14-A agent-legs
  restatement (:2134-2143) correctly reaches agent-`d.vars` cells only across the `cap$N`
  clone edge ("through `masterOf`/`d.clone`" — `masterOf` is `cloneMaster(cell, dims_.clone)`,
  `vortex_core.cpp:344-346`) and excludes ref-capture records from the link walk.
- **`documentId_` claims verified.** `store.hpp:133` two-arg ctor decl, `:1435` the member;
  `store.cpp:135` the ctor's `documentId_ = documentId;`; `store.cpp:4450` the store-tables
  load's `documentId_ = tables.documentId;` inside the `readStoreTables` directory-open path;
  `historyFromSeal` mints `std::make_unique<Store>()` at `publication.cpp:1097`. All five
  cites accurate, "ctor-or-load" phrasing honest at :634-638, :1561-1564, :2786-2788.
- **v1.14 changelog narrates correctly.** Every adopted r14 item appears: the R14-A
  restatement and vocabulary sweep, the (a)/(b)/(c) arms, the frontier list, the (d)-time
  `*handleTarget` correction, the recipe-tuple `+d.values` conditional, the declared-victim
  bound, the grafted-twin caveat, the alias qualifier mirroring, the `documentId_` narrowing,
  the (v) cite fix — plus marked confessions of its 1.13-entry retro-edits (see R15-2 for the
  two it missed).
- **Mechanicals.** `mdl -i` exit 0. 22 fence markers = 11 balanced pairs. 7 table blocks
  (:4, :79, :123, :595, :1092, :2867, :2888). All 16 TOC anchors resolve against the 16
  content headings (15 numbered + Appendix); version field 1.14; "Design draft." intact (:6).
- **Declared-victim bound verified sound.** :1234-1237's carve-out is exactly right: a
  verbatim-alias match can only hit a declared clause or a `graft_*`-named clause (a learned
  clause's `patch:<n>` label is unreachable — `patch:` is refused as a `name` argument,
  :1223-1224), and only a `graft_*` victim lacks a `d.patch` record — so "session-grafted
  victim mints no `d.patch` record" covers the whole non-declared class.

## New findings (v1.14 deltas only)

**R15-1 MINOR — "the copy's capture slots carry the same definition-site positions" conflates
the two capture carriers.** :1176-1178 — under the doc's own §5.3 vocabulary, a "capture-slot
cell" is the `cap$N` wing slot (:822-824), which carries *no* `int64` — `asInt64` fails and
`masterOf` decodes it as a clone edge. Position-carriage is the ref-box/ref-capture arm only;
the cap$N arm preserves the referent by re-cloning the same definition-site cell. And "name
the copy's own twins for internal ones" applies only to the content arm — `cap$N` referents
are definition-site bindings, never internal to the fragment. As written an implementer could
write `int64` into a copy's `cap$N` clone slot and silently flip `wingOf`'s decode off
`masterOf`. **Fix:** "the copy's captures are preserved verbatim — its ref slots carry the
*same* definition-site positions the source's did for external referents, its `cap$N` slots
re-clone the same definition-site cells, and internal referents name the copy's own twins
through the old→new map, so the copy is `closed` iff the source was."

**R15-2 MINOR — two of the 1.13-entry retro-edits go unmarked in the 1.14 changelog.** The
entry confesses "the 1.13 entry's own 'capture edges' phrasing is corrected" and "(the 1.13
entry corrected to match)" for grafted-twin, but stays silent on the two other retro-edits in
the same entry: the program-side arm regroup at :3580-3585 — which silently replaces a
*false* mechanism claim v1.13 actually made (program-side under `int64`/`*handleTarget`), a
stronger case for marking than the two confessed ones — and the (v)-narration backfill at
:3600-3603, which adds a sentence the published 1.13 entry never had. **Fix:** append to the
1.14 entry's correction list: "the 1.13 entry's program-side grouping corrected to the
`+d.name`→`ensureDimension` arm, and the (v) narration it omitted backfilled".

**Nit bundle:** :1244 "displaced nothing durable" vs :1234's "displaced-vs-appended staying
auditable" — the two clauses now agree, but the changelog's parallel gloss "appending where no
same-named clause survives and displacing where one does" (1.14 entry) compresses "a
same-named *declared* clause" (grafts can't fold back, so a surviving hit is necessarily
declared) — acceptable as a summary, worth a "declared" if touched again. Residual unadopted
nits carried from r13/r14, all still present and all still nits: `wingOf` cited `:366-369`
for the `linked(opcode, dims_.grab, POS)` call that is `inputsOf`'s at `vortex_core.cpp:373`
(:1149-1150); "replayed grafts" verb vocabulary (:2199); "`learn_where`/`learn_body` install
nothing" (:1795 — installs no *clause*); the `patch:<n>` sketch comment's unpinned serial
(:2686 region); `set_weight`'s table-row gloss still silent on mint-on-miss (:1115).

VERDICT: CONVERGED — all four r14 minors verified landed (cites re-verified against
`store.cpp:123`/`:135`/`:4450`, `publication.cpp:1097`, `vortex_core.cpp:344-346`/`:356-365`);
the R14-A restatement, the (a)/(b)/(c) `closed` partition, and the three-arm endpoint
vocabulary are uniform across glossary/§6.1/diagram/§6.6/genesis with `d.captured` confined
to annotated history; mechanicals clean. The residue is two MINORs — the "capture slots
carry positions" carrier slip at :1176 and the two unmarked 1.13-entry retro-edits in the
1.14 changelog — plus a carried nit bundle; nothing here blocks handing the document to an
implementer.
