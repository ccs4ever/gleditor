# Round 4 Disposition

Round: 4 (against design doc revision 3). Critics: six independent reviewers.
Findings: **0 BLOCKING, 12 MAJOR (→ 7 clusters), ~49 minor** across the six reports.
Three critics (vortex, codebase, zigzag) independently converged on the same holder
topology fix; zigzag and the writer independently converged on the same two edge-class
ordering defects; ergo and the architect independently converged on the
Settings-store enforcement gap. Verdicts: NOT CONVERGED ×6 — all majors are spec
amendments; none requires redesign.

## Cluster R4-1 — Holder topology: adopt the literal two-cell vagent shape

**Reports:** vortex M1; codebase M1 (+m1, m3); zigzag M1 (+m5); writer m5/m8 (partial).

**Finding.** `registerProgramImage` walks `rankAfter(holder, d.vars)` — a posward
first-match on the *flat* shared rank. The doc's one-cell `stdlib.image` is a member of
`home +d.vars`; its `+d.vars` edge *is* its rank position — there is no nested rank, so
`"ops"`-under-`stdlib.image` is realizable only as posward adjacency, under an ordering
invariant the doc never stated and which §4's mint order (ops at start, holder at end)
contradicts. The real vagent shape is a dedicated **unranked** holder reached via a
key-record's `+d.values` — `rankAfter` on it enumerates a genuinely private rank.

**Disposition: ACCEPT — adopt the two-cell shape.**

- `"stdlib.image"` stays a `+d.vars` member record of `home` (so `rebindInstance` can
  enumerate it) — but its `+d.values` **record head** (unranked, as all record heads
  are) anchors a **private** `+d.vars` rank containing exactly the `"ops"` record.
- Registration and rebind both run `registerProgramImage(head)` where `head =
  linked(stdlibImageKey, values, POS)` — the private rank gives true isolation:
  foreign `"ops"` records are *unreachable* (not merely unscanned), the holder→ops
  ordering invariant disappears, the first-match keyspace exposure (codebase m3)
  disappears, and an ops-less holder yields the genuine `unexpected` → skip.
- §4 mint order is freed: the `"ops"` record and holder are minted together at
  bootstrap end; member scalars still write late per names-early/values-late.
- Ops-member Int64s are translated during the graft's captured-record walk
  (codebase m2 — named explicitly), not by cell marks.
- §11 isolation gate gains the posward-foreign-`"ops"` arm.

## Cluster R4-2 — §5.2.3 edge-class precedence + marked-value dispatch

**Reports:** zigzag M2 (both halves); writer M1, M2 (+m1).

**Finding.** (a) *Membership-onto-shared-anchor* and *pre-existing-member-endpoint*
overlap: a name-merged `home +d.stdlib→module` edge satisfies both; applied in listed
order the tail-append severs the module mid-rank — the exact R3-M2 hazard. (b) The
kind-agnostic mark-detection bullet sweeps OpHandle wing slots (which carry the same
`+d.mesh.relocation` mark) into the Int64 arm — `asInt64` on an OpHandle cell yields
nothing. (c) The residual rule's "touching a pre-existing cell" must name src/dst
endpoints — every dim cell is pre-existing.

**Disposition: ACCEPT.**

- State the precedence: *classify by member-endpoint provenance* — graft-minted member
  → membership tail-append; pre-existing member → `rankContains`-skip-else-refuse.
- Pin the decode: *edge-existence detects ref-bearing; `ValueKind` dispatches* —
  Int64 → `asInt64`→`map[]`; OpHandle → `handleTarget`→`map[]`. Fix the
  `isCellRelocationHandle` parenthetical ("detects nothing among the Int64-marked
  cells").
- Residual rule pinned to src/dst endpoints.
- Also pin: `rankContains`/neg-`linked` as the "already holds" query (zigzag m1);
  name-merge candidate set = image `home +d.stdlib` members whose full text
  `findModule()`s (zigzag m2).

## Cluster R4-3 — `MarksOutstanding` refuses in all modes

**Report:** ergo M2.

**Finding.** `ImageThenCpp`'s "record and fall back to C++" runs `bootstrap()` under the
live mark — identical untrailed-map-vs-trailed-cells corruption, silently.

**Disposition: ACCEPT.** `MarksOutstanding` (and `ChoiceStackLive`) are
**environmental refusals**: they refuse `ensureCells` outright in *every* mode, no C++
fallback. The refusal matrix gains an "environmental" column; only image-content
refusals (missing/unreadable/truncated/epoch/format/graft) fall back.

## Cluster R4-4 — Settings-store enforcement + primary-context timing

**Reports:** ergo M3; architect M2 (both halves); arch m1, m4; ergo m2.

**Finding.** `loadConfigFromStore` is store-agnostic and `bindStore` feeds it document
stores — "Settings only" needs a named seam; and the first `ensureCells` fires inside
the `VortexHost` ctor (via `bindXanaduStore`→`ensureVortexHost`) before *any*
`loadConfigFromStore` — the key can never steer the primary context.

**Disposition: ACCEPT.**

- The `vortex.stdlib_source` read is pulled *out* of `VortexHostConfig::fromStore`
  entirely: the Settings call site (`xuzz_app.cpp` seam) reads it via a named helper
  (`settings::vortexStdlibSource()` or the `kVortex*` spec) and calls the mode setter —
  **before** `vortexHost()`/`bindXanaduStore` builds the first context
  (`systemStore(Settings)` is already in hand there). `fromStore` never sees the key.
- Pin the reachable seam: the first context fires in `bindXanaduStore`→
  `ensureVortexHost`, not the visible `vortexHost()`; `attachVortexHost` bypasses it.
- `StdlibPolicy` becomes per-field `optional`s with per-field setters (dir writer and
  mode writer can't clobber each other).
- Bad spellings: warn naming the value, treated as unset, fall through (ergo m2).
- Middle-chain dir steps: `GLEDITOR_ASSET_DIR`/`GLEDITOR_DATADIR` follow their existing
  taken-as-given convention — a bad one yields `ImageMissing` at the resolved path, no
  silent skip (arch m5).
- Non-xuzz consumers: `loadConfigFromStore` is only invoked by xuzz → the key is
  xuzz-only; `paths::dataPath("xuzz", …)` hardcodes the shared data dir — stated
  (ergo m6, arch m12).

## Cluster R4-5 — Arena-swap identity + `VortexCore` resync

**Reports:** architect M1 (a+b); vortex m3, m4; zigzag m6 (corroboration); ergo m3;
arch m3.

**Finding.** No identity token exists — `primary_.arena` move-assigns in place so
`&arena()` never changes, `base()` misses same-base swaps, no generation counter
exists. Deeper: `VortexCore::home_`/`dims_` are ctor-only — post-swap `resolve()`,
`findOrMintDimension`, `getOrCreateModule` consume dead refs (test 8 can't pass).
`bindXanaduStore`'s `engine_.reset()`+rebuild already dangles `vortex_host_->
baseManifold_` today. `ChoiceStackLive` checked *after* the move-assign can't un-swap.

**Disposition: ACCEPT.**

- Identity token: **`arena().base()` + a `VortexCore`-side home re-derivation**. The
  doc names the mechanism explicitly: `bindManifold` (or a new `rebindArena` entry
  point) must (i) run the `ChoiceStackLive` check *before* the move-assign; (ii) after
  swap, re-run the `VortexCore` genesis resync — `home_`/`dims_` adopt-or-mint against
  the new arena (own-home guard for null-base arenas: `arena().home()` returns only
  the *base*'s home) — then re-fire `ensureStdlib` on the rebound instances. A
  sentinel-check alternative (record `map[imageRecord]`, re-verify `contains()`+
  `textOf()`) is named for callers that can't resync.
- Driver set named: `bindXanaduStore` document rebind is the reachable case; state the
  pre-existing dangling-`baseManifold_` exposure as out-of-scope-but-noted.
- `ChoiceStackLive` trigger named (produce cursor / open alternatives surviving
  `call()` — "switch bound document with results still streaming"); refusal aborts the
  swap pre-assign, old arena retained.

## Cluster R4-6 — §6.4 local image: production path, mode-gating, dependency

**Reports:** ergo M1; architect M3; writer m12.

**Finding.** `:export-lib` is per-module (fails §5.1 `FormatRefused`); `vortex-stdlib-
gen` is a build tool; `exportStandardLibrary` is test-only — no reachable authoring
path. And under `Cpp` mode a local graft contradicts "Cpp = today's behavior" while a
hand-run generator (forced `Cpp`) would bake the build host's local image into the
artifact.

**Disposition: ACCEPT.**

- Production path named: v1 ships `vortex-stdlib-gen --out <dir>` **as an installed
  developer tool** (its `--stdlib`/`--module` flags select whole-image vs. per-module
  export — spec'd in P32); the local image is a whole-space export of a scratch
  context *after* dev additions. A `:export-lib-image` interactive command is deferred
  (U-X8); §6.4 states plainly: v1 local-image authoring is a developer flow.
- Mode gating: the local graft runs only after stdlib presence exists (image graft or
  C++ bootstrap, under `ImageThenCpp`/`Image`/`Cpp` alike — but **`Cpp` + explicit user
  opt-in**; a `Cpp`-forced generator suppresses it via a `localImage=false` policy
  field so determinism can't depend on the Makefile's XDG redirect).
- `Image` mode + base refusal: local graft is suppressed (nothing to graft onto).
- Reset: `resetStdlibImageForTesting()` covers both holders (arch m2).

## Cluster R4-7 — `makeWorkerContext`'s orphan `bootstrap()` + consumer lists

**Reports:** arch m7, m8, m6; writer m4.

**Finding.** `vortex_host.cpp:113`'s explicit `stdlib.bootstrap()` is outside the ctor —
under the split it double-mints over a ctor graft. `vicon` carries `VortexStdLib` but
was missed; `vquery` was wrongly listed as an image consumer.

**Disposition: ACCEPT.** P30 names `:113` → `ensureStdlib()` (or drop). §3.1/§5.4 lists
fixed: consumers = xuzz, vpl/vplc, vprolog, vagent, vicon/viconc; `vquery`/`vqueryc`
carry no `VortexStdLib`.

## Minor batch — all accepted

~40 minors applied verbatim: `status()` surface pinned (a `vortex_stdlib_status`
builtin + `VortexHost::stdlibStatus()`; ergo m1); refuse→warn on `ImageMissing` under
`ImageThenCpp` for installed apps (ergo m5); `rebindInstance` enumeration pins
`varsMember` pre-check + continue-on-malformed (ergo m7); §6.5 discloses the
`d.params`/`d.mesh.relocation`/marker introspection surface (ergo m8); `insertIntoRank`
mechanism corrected to `anchor→cell→displaced` (vortex m1, zigzag m6); process holder =
`unique_ptr<State>` under mutex carrying `expected<Triple, Refusal>` (vortex m2, arch
m2); extraRoots-seeds-`order` named for early manifest births (zigzag m3); grafted
mark-edge replay choice pinned: marks replay (needed for re-export) and read as
declared refs only under the marked-cell's own value kind (zigzag m4); sig records
mint at each `build*Module` end (writer m2); critique-skill path named
`.agents/skills/vortex-design-critique/SKILL.md` (writer m3); clone-set "minus" list
adds name-merged content cells (writer m5); `OpsSegmentUnreadable`/
`StoreTablesUnreadable` map to `ImageUnreadable`; empty `ops.nodes` → `ImageUnreadable`
(writer m6); dim-resolves-nowhere case = policy refusal on unfiled `"d."`-name mints
(writer m7); ops-presence precheck precedes `registerProgramImage` in both paths
(writer m8); §12 delta restated ~5.3–6.8K records+markers (writer m9, arch m10);
dev-fields appositive rephrased + late-write criterion pinned (writer m10); `discard`
boundary drawn between steps 3/4 + double-registration noted idempotent (writer m11);
"stdlib root" term dropped (writer m13); header citation row completed + unnamed
anchors named (writer m14); §6.3 gains "newer, same epoch" row (writer m15);
`rebindCaches()` named as `rebindInstance`'s cached-ref half (writer m16);
`ChoiceStackLive` trigger/recovery (ergo m3); "holder" two senses renamed — manifold
side = "image record", process side = "folded-image state" (ergo m4); wasm/android
packaging note added (arch m9); `d.params`/`sql_lens` qattr share noted honestly (arch
m11); `adoptOpRecords` mid-load write-through noted under read-only (arch m12); cite
drift batch (vortex m5: `system_docs.hpp:261–266`, `run(cursor,100)` :2511/:2529,
`DocumentId` wording).

## Refused: none.

## Round 4 scorecard

| critic | B | M | m | verdict |
|---|---|---|---|---|
| writer | 0 | 2 | 16 | NOT CONVERGED |
| vortex | 0 | 1 | 5 | NOT CONVERGED |
| codebase | 0 | 1 | 3 | NOT CONVERGED |
| architect | 0 | 3 | 12 | NOT CONVERGED |
| ergo | 0 | 3 | 7 | NOT CONVERGED |
| zigzag | 0 | 2 | 6 | NOT CONVERGED |

Accepted: 7 clusters + ~49 minors. Refused: 0. Next: revision 4 → Round 5.
