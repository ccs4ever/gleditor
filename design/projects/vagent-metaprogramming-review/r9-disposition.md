# Round-9 disposition — orchestrator rulings on v1.8 findings

1 blocking + 4 majors — a regression driven by one mechanism: **R8's "positional assertion"
conflated two landmarks**, and four lanes (vortex-blocking, codebase, zigzag, meta) independently
verified the literal reading is unsatisfiable. Ergo converged; writer holds 2 minors.

## Blocking

**R9-A — genesis (a) `walk-max == E` is unsatisfiable** (vortex BLOCKING, codebase & zigzag &
meta majors — verified: `store.hpp:677`, `manifold.cpp:434`, `arena_manifold.cpp:2010`). `E` as
stated is the ops-segment *length*; cell refs are birth-op *positions*; `promote` emits links
last so the image's last op is never a birth — the assertion refuses every honest artifact.
Separately the walk is un-pruned: `+d.code`/`d.vars`/`d.belief` legitimately reach `>E` subgraphs
(registered learned heads, state cells) — a program-side `agent_cell()` root makes every stateful
resume fail. **Ruling:** replace the assertion with the two distinct landmarks —
`E_ops` = image prefix length in ops (the ref-space ordering bound), and per-walk-cell
*classification*: each `program()`-walk cell tests its own ref position against `E_ops`
(`≤E_ops` program-side, `>E_ops` state-side) — cells beyond the bound are *classified*, not
refused. The merged-lineage detection moves to a **mandatory ops-segment content comparand**
(see R9-B's canonicalization), not the walk; a walk-max assertion is dropped entirely.
State the coverage explicitly: the comparand detects permutation/scramble (`store.cpp:3880-3938`
topo-resort is within-band-permutable), the per-cell test detects non-image ingress.

## Majors

**R9-B — `attach` installs the base *after* `WorkerContext` construction** (architect). Frozen
`home_`/dims (`initGenesis`, `vortex_core.cpp:14`,`:29-70`) and an empty `opcodeMap_` leave
`core.home()` anchoring a twin and P15 unarmed through incarnation 1. **Ruling:** spawn adopts
resume's injection shape (`mesh_actor.cpp:270-271`) — the seeded store is bound *before* the
worker context/genesis freeze, named as part of the same owed adapter-API seam. Companion
correction (vortex minor): `ArenaManifold::store()` is already public (`arena_manifold.hpp:269`)
— detection needs no new surface; only *supply* is owed. Companion minors: seeded-store
`documentId` is stated (lineage `storeId` adopted, `store.hpp:1435` — the seeded ops carry the
identity or it is minted-and-recorded, pick per `applyOpsSegment`/`adoptOpRecords` behaviour);
the verify comparand is the **canonical post-remap form** (op kind + non-scroll fields —
`CompactOp` bytes differ under the ScrollId remap, `publication.cpp:981-1008`).

**R9-C — copy pre-rehydration contradicts the recorded-template rule** (zigzag major, meta
minor). "Inherits the head's *current* contents" clashes with §6.1's recorded-emission-template
replay (:1097); recipe literals arrive stale and the kind-guard freezes the misbind. **Ruling:**
copy ref slots derive from the *capture-record endpoints* at replay time (the rehydration source
is the record, not the donor cell's live value) — restate the mechanism, keep the conclusion.

## Minors — all adopted

- **`ops.nodes` → the sealed `ops` segment** (writer): `applyOpsSegment` reads `\x7fXSO\x01`
  (`publication.cpp:1033`); `ops.nodes` is the native CompactOpNode spool — fix both sites
  (:601, :1367) with the right filename for the right mechanism.
- **P16-row "spawn store parameter" vs §6.6 `attach(Space)`** (writer): reconcile to one API
  shape — the owed seam is supply-side injection matching resume, not a store parameter.
- **Link-emission cite** (codebase): `arena_manifold.cpp:1950,:1996` is OpHandle *value*
  emission; link emission is `setLink` ~:2035 via the `:1864-1867` identity map.
- **`mergeValues` sort label** (codebase): `(contentHash, authorKey, parent)` sort
  `mesh_value.cpp:519-523`, not "discovery order"; "fresh store" is the caller's choice
  `mesh_host.cpp:507` vs `:509`.
- **`plan_named` "event-handler clauses included"** (codebase) vs declared-handler `+d.name`
  anonymity (:1521) — resolve: `plan_named` matches verbatim `+d.name` labels; declared
  handlers carry none, so they are unmatchable — consistent, but the row must stop claiming
  inclusion.
- **`code_named(L)` qualified-L** (ergo R9-F1 + meta): qualified `module:name` args can't match
  verbatim labels — but `:`-bearing args like `patch:<n>` legitimately can. Keep verbatim
  matching; state the unresolved-at-emission convention for `run` records (the genesis
  `int64(head)` write needs the static-slot reading, not per-`run` eval); a qualified-L record
  stays unresolvable — that is the documented headless-`run` refusal, worded as such.
- **Anonymous handlers** (ergo R9-F2): dedupe-by-name can't match them — state the full gap
  (un-replaceable/un-removable durably; `learn_handler` coexists under priority).
- **`label_of(c: Cell)` vs `Code<Cell>`** (ergo R9-F3): name the `cells"…"` path or a `head_of`
  read for the `reify:`-label workflow.
- **"two reasons" copy-inheritance header** (writer residual) + remaining nits — apply all.

## Round status

v1.9 to follow. The blocking finding is a landmark-conflation in the assertion I authored — the
mechanism (lineage + per-cell classification + content comparand) is sound; the *stated test*
wasn't.
