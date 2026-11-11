# Round 2 disposition — vortex-stdlib-prebuilt-store

Reviewer tally: writer 0B/2M/7m, vortex 0B/5M/9m, codebase 0B/3M/10m,
architect 0B/4M/13m, ergo 0B/4M/7m, zigzag 1B/3M/5m. Zero converged; one BLOCKING.

## Cluster R2-1 — Shared `"cell"` marker is impossible (zigzag B1; architect M3; codebase m10)

**Disposition: ACCEPTED.** Verified `link()`'s `displacedIt` arm clears the *previous
slot's forward edge* (`manifold.cpp:679–688`) — one shared marker leaves only the last
mark standing; harvest misses every earlier referent AND promote refuses every earlier
OpHandle. Fix: **per-ref marker cells** (the `refCell` convention, `records.hpp:63–72`).
Additionally adopted architect's positional-member point: `"ops"`/`"in"`/`"out"` record
members carry Int64 refs **without** marks — record membership is itself the ref
declaration (the `seedProgramImage` precedent harvests/restates positionally); the graft
translates members by walking each record chain. Refusal: a member/marked cell whose
referent misses `map[]` is always a named refusal, never verbatim.

## Cluster R2-2 — `rebindInstance` empties `routineBindings_` under `Cpp`/fallback (ergo M1; vortex M2; codebase M3)

**Disposition: ACCEPTED.** The `d.params`/`"ops"` records are emitted **inside
`build*Module`/bootstrap** (self-describing arenas — uniform rebuild in both modes; the
generator simply exports what bootstrap minted, which also fixes the mint-ordering
finding R2-16). Absent `"ops"` records are a no-op (`mintOpcode` self-registers). The
goal trio gets `{in:{},out:{}}` `d.params` records like `fail`.

## Cluster R2-3 — `opcodeMap_` staleness on `bindManifold` (ergo M2; vortex M3)

**Disposition: ACCEPTED.** `rebindInstance` clears `opcodeMap_` on arena swap — the whole
arena is replaced so every entry is dead; `contains()` can't filter because stale ints
*alias* fresh cells — then re-registers the manifest. Runtime mints re-register via
`mintOpcode` (non-decodable kinds re-mint on use); a shared `VortexVM` second instance
just re-registers the same arena's manifest — the constraint is named.

## Cluster R2-4 — Graft atomicity (ergo M3; vortex M4)

**Disposition: ACCEPTED.** `graftImage` runs inside a taken `mark()` — `discard` on
success, `release` on any mid-graft refusal (rolls back cloned cells and membership
appends; interned primedia stays as harmless dead spans). Registration/rebind run after
`discard` (untrailed). The §5.4 matrix gains a graft-refusal row.

## Cluster R2-5 — `UserPermascroll::Config.readOnly` doesn't exist; ctor writes before fallback (codebase M1; architect M1; writer M2)

**Disposition: ACCEPTED.** P34 widened: `Config` **gains** `readOnly` (new field, wired
to `spool_`); under it the ctor skips `permascrollState`/`create_directories`/write-back
entirely (no LMDB, no key mint, no asset-dir mutation); the primedia spool honors a
*requested* `O_RDONLY` (not just EACCES-detected); ops-spool append/flush/seal **refuse**
readOnly (not silent-drop); `Store::save`/`writeSegmentFile` refuse early with a named
error. Repriced ~60–70 lines.

## Cluster R2-6 — Unscoped all-`"ops"`-records walk (codebase M2)

**Disposition: ACCEPTED.** Registration targets the graft's *mapped* records
(`map[imageOpsRecord]` per image grafted) — not every `"ops"`-texted record on the
shared `home +d.vars` rank (document/vagent records must never be scanned). The
`handleTarget` member-decode arm stays for generality.

## Cluster R2-7 — Membership-append anchor + collision rule (zigzag M1; vortex m1; codebase m1)

**Disposition: ACCEPTED.** Append is pinned: `link(rankTail(anchor, dim), dim, POS,
member)` — the `exportSymbol`/`getOrCreateModule` pattern verbatim (equivalently
`insertIntoRank(member, rankTail, …)`); banned is `link()` onto an *occupied* mid-rank
slot. Collision rule stated: same-text local members land posward of base and are
shadowed by `resolve()` first-match — local images add, never replace (consistent with
the U-X1 deferral).

## Cluster R2-8 — `+d.owner` scope Int64 (zigzag M2)

**Disposition: ACCEPTED.** Manifest/signature records are minted **unscoped** (no
`+d.owner` arm) — stated deliberately; the unscoped-matches-any rule is relied on for
these singleton keys. No new ref class enters the image.

## Cluster R2-9 — Single-owner invariant (zigzag M3)

**Disposition: ACCEPTED.** Stated in §4 and §5.2: every `+d.values`, `+d.params`,
`±d.contract`, `+d.spin`, `+d.step`, `+d.clause` edge targets a cell owned by exactly
that edge — aliasing silently evicts at mint and at replay; the generator asserts it.

## Cluster R2-10 — Marks-free assert-only hole (architect M4)

**Disposition: ACCEPTED.** `outstandingMarks() > 0` at graft is a **named refusal in any
grafting mode** (`Image` refuses; `ImageThenCpp` records + falls back); the assert stays
as debug tripwire. `MarksOutstanding` joins the `StdlibImageRefused` cause enum.

## Cluster R2-11 — Config channel + observability (architect M2; ergo M4)

**Disposition: ACCEPTED.** Semantics pinned: `vortex.stdlib_source`/`vortex.stdlib_image_dir`
store keys (new `settings::` constants + `fromStore` parse arms — added to P31's
Location) write the process `StdlibPolicy` at `loadConfigFromStore`, effective for
contexts built *after*; `primary_` and in-flight contexts keep the policy in force at
their graft. Precedence: explicit setter > store keys > env > default.
`GLEDITOR_STDLIB_SOURCE` values: `image`/`cpp`/`imagethencpp`. Observability:
fallback/refusal logged on the `"vortex.stdlib"` spdlog category; `VortexStdLib::status()`
exposes effective source + recorded refusal. Policy/dir changes after first fold take
effect for not-yet-grafted contexts' *mode*; the folded image itself is resolved once —
`resetStdlibImageForTesting()` is required to re-fold (called after env/policy change;
lazy open is `std::once_flag`-synchronized for Stage-3 parallel contexts).

## Cluster R2-12 — Marked-cell detection predicate (vortex M1)

**Disposition: ACCEPTED.** Graft detects marked cells by *edge-existence* — "has an
outgoing `d.mesh.relocation` edge", kind-agnostic — not `isCellRelocationHandle`
(OpHandle-gated, would detect nothing). Stated explicitly in §5.2.3/P26 with the
wrong-predicate warning; DRY note: the scan half should share a home with
`foreign_op_handle.hpp`'s constants (the vagent `markedRelocationSlots` sweep is the
same shape) — recorded as an implementation note, since the write halves genuinely
differ.

## Cluster R2-13 — Validation order (vortex m2; architect m1)

**Disposition: ACCEPTED.** Order pinned: `permascroll/` dir-exists (precheck) → open →
fold → read `expectedScrollBytes` (Int64 survives truncation) and compare `perma->size()`
→ then `imageEpoch`. A truncated scroll can no longer masquerade as `EpochMismatch`.

## Cluster R2-14 — Flow diagram order (writer M1; ergo m1; vortex m4)

**Disposition: ACCEPTED.** Diagram swapped to `ensureCells` → `rebindInstance`;
`bindManifold` calls both.

## Cluster R2-15 — Error channel coverage (ergo m2; architect m2)

**Disposition: ACCEPTED.** The deferred throw fires only at public use-points
(`call()`/`solve()` and any routine entry); `resolve()`/`modules()`/`symbolsInModule()`
answer honestly (`nullopt`/empty) so `stdlibPresent` and internal rebinding never wedge.
`rebindInstance` is total (`nullopt`-safe). Recorded refusals cover the `bindManifold`
path. `StdlibImageRefused::what()` carries cause, both epochs, resolved dir, and the
remedial env var. §6.4 states local images pass the same §5.1 validation.

## Cluster R2-16 — Manifest mint ordering (vortex M5)

**Disposition: ACCEPTED via R2-2.** `ops`/signature records mint arena-side **during
bootstrap** (restate covers their members; precedent order); `stdlib.image` mints
store-side last — its members are scalars depending on the finished output.

## Minors — all accepted

zzResolveDim note reworded ("fixed in-flight during this design's review");
`candidatePreds` ~:4887–4909; §1 module list corrected (21, mint order);
`"(U-X4 → P34)"` table note; U-X6 anchored at §5.4; `findall` → "vagent `findall` over
`vortex_*`"; table cells wrapped; `exportLibrary` `vortex_host.cpp:505`; test `:674`;
intended-subgraph defined (all image cells minus name-mapped dims/`home` — whole-space =
everything); edge replay once pos-side; non-enumerated-dim catch-all (name-merge
`"d."`-texted dims via `findOrMintDimension`); local `"stdlib.image"` coexistence
clause; `"ops"` name-collision note (kept for `registerProgramImage` convention);
§12 repriced ~18–22K ops; wing/member conflation fixed; `rawWingSlotsOf` decode contract
noted; every-OpHandle-marked invariant stated; portable-zip `./assets` boundary noted;
`resetStdlibImageForTesting` contract + `once_flag`; `documentId` as reserved constant +
inert scroll fields; WorkerContext site list completed (`vagent/lineage.cpp:564`,
`vprolog/compiler.hpp:78`, `vicon/compiler.hpp:160`, `vpl_engine.hpp:132–136`);
install-scope line; P31 Location gains `fromStore`/`settings::` files;
`outstandingMarks()` accessor name; "stale" disambiguated (format-stale vs version-old);
`status()` optional accessor; graft-refused matrix row.

## Refused / partial

- **Uniform per-ref marks on record members** (zigzag B1's fix text said "for…manifest
  members, sig members alike"): partially adopted — positional restate instead
  (architect M3's cheaper arm, the `seedProgramImage` precedent); marks remain on wing
  slots and free-standing ref scalars where membership is not self-declaring.
