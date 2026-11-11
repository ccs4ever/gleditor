# Round 3 critique: Vortex Expert (vortex-stdlib-prebuilt-store.md)

Paths relative to repo root. Tally: 0B/4M/6m.

## MAJOR

### M1 — `registerProgramImage(map[home])` cannot deliver scoped registration (§5.2.4, P28, §6.4, §11.5)

`rankAfter(imageRecord, dims.vars)` first-text-match on `"ops"` (`vortex_vm.cpp:131–135`,
`break`). `map[home]` is the shared home; the walk scans the shared rank: local image's
`"ops"` never registers (base's record first-matches — §11.5's own gate fails); a foreign
`"ops"` posward of the grafted one is scanned first, falsifying "document/vagent records
must not be scanned"; and `map[imageOpsRecord]` doesn't fit the signature — a record cell
carries no `+d.vars` "ops" rank → "carries no `ops` rank" refusal. **Fix:** register by
the mapped record — a record-taking entry (`linked(record, +d.values)` → `rankAfter(head,
+d.step)` → member decode → `registerProgramOps`), or nest `"ops"` under a per-image
holder (e.g., as a `+d.vars` member of `"stdlib.image"` so
`registerProgramImage(map[imageRecord])` scopes per graft).

### M2 — `cursorPC_`/`memoizedOps_`/`choiceStack_` uncleared on the `bindManifold` arena swap (§5.2.5, P30)

The doc's own argument — "the whole arena is replaced so every entry is dead, and
`contains()` cannot filter because stale ints *alias* fresh cells" — applies verbatim to
all three (`vortex_vm.hpp:304–306`, `ChoicePoint::mark` :163). `primary_.vm` persists
across `bindManifold` (`vortex_host.cpp:149–157`): stale `cursorPC_` keys alias fresh
cells (`getCursorOpcode`/`setCursorOpcode` :279–280); `memoizedOps_` keys alias non-memo
cells; `choiceStack_` holds `Mark`s snapshotting the *old* arena's sizes — a later
`backtrack()`/`release()` truncates the *new* arena's `slots_`/`links_`/`content_`/
`scratch_` to dead counts (`arena_manifold.cpp:1671–1674`) — corruption. The
`MarksOutstanding` gate protects the graft, not the swap; a `goalProduce` cursor +
outstanding choice legitimately outlives a `call()` boundary. **Fix:** swap/rebind clears
`cursorPC_` and `memoizedOps_`, and drains or refuses on non-empty `choiceStack_` — or
pins "no live produce cursor at `bindManifold`" as a named precondition.

### M3 — `PromotionBudget::wholeSpace` semantics never stated; the registry rank depends on the unstated skip-lift (P27, §3.1, §5.2.1)

`promote` skips `d.dims`/`d.lineage` edges (`arena_manifold.cpp:1778–1782`). All 19 dims
mint at genesis (`vortex_core.cpp:52–73`), but unused ones (`d.stack`, `d.cursors`,
`d.cache`, `d.stores`, `d.role`, `d.pinning-cursors`) are reachable *only* via the skipped
`+d.dims` chain — and the rank edges are `+d.dims` links themselves.
`extraRoots{home, dims_.dims}` adds one already-reachable cell. As written, the image
ships no registry-rank edges and misses the unused dim cells — contradicting §3.1's "the
full `d.*` registry rank" (§5.2.1's "every cell on the image's `d.dims` rank" would
iterate an empty rank; the step-3 catch-all silently rescues it, which is why the gap
hides). Also pin whether `d.lineage` stays skipped. **Fix:** state `wholeSpace` lifts the
`d.dims` (and which `d.lineage`) walk-skip, or have the generator emit the registry rank
directly.

### M4 — `expectedScrollBytes` is unreadable under the dominant truncation shape (§5.1, §4.5, §6.3, §11.3)

Reading the member needs record/member *names* — primedia text minted **store-side last**
(§4.5), i.e., the scroll tail: the first bytes tail-truncation loses. `read` clamps to
`""` (`segmented_primedia_spool.cpp:119–150`) → `"stdlib.image"` lookup fails → refusal
reads "missing/malformed image", never `ScrollTruncated`; §11.3's gate can't pass. **Fix:**
mint the manifest's record/key cells early (only Int64 member values depend on the
finished spool), or attribute truncation by comparing `perma->size()` to the max span-end
referenced by the ops.

## MINOR

m1. Post-discard registration failure has no disposition: `registerProgramImage` runs
    after `discard`; a refusal there leaves committed-but-unregistered cells, and
    `ImageThenCpp` would `bootstrap()` over them (`exportSymbol` appends unconditionally).
    Dry-run the member decode inside the mark (read-only) so post-discard registration is
    infallible, or pin a post-discard cause that refuses rather than falls back.
m2. "for `vortex_param` symmetry" misattributed (P29): `vortex_param` enumerates wings
    (`inputsOf`/`outputsOf`, `vortex_stdlib.cpp:709–781`), not `routineBindings_`; the
    trio's records serve `call()` arity/`routineInputs`/`routineOutputs` (`:4293–4307`).
m3. Empty `ops.nodes` under requested `O_RDONLY` throws a bare `runtime_error` (header
    write fails, `segmented_ops_spool.cpp:619–625`; `store.cpp:4410`) — pin as a named
    loader refusal; retry scope pinned to `errno ∈ {EACCES, EROFS, EPERM}`.
m4. Refusal mechanism unpinned: `~UserPermascroll` calls `spool_.flush()` unconditionally
    (`user_permascroll.cpp:401–403`) — a throw terminates. Pin refusals to
    bool/`std::expected` returns on `flush`/`seal`/`adopt`/`writeSegmentFile`, or exempt
    `flush()` when nothing is pending.
m5. `imageEpoch` omits the decodability boundary and no `OpcodeKind`→label table exists:
    `kLastContentDecodableOpcode = FieldSet` / `kLastOpcodeKind = ScanSubject`
    (`vortex_vm.hpp:145–158`) are part of the decode contract yet absent from the digest —
    an image under a moved boundary but identical labels passes `imageEpoch` and decodes
    differently. Include the bound constants (or the `isContentDecodable`/`isRegistrable`
    partition); author the label table.
m6. Stale cites: `vortex_host_config.cpp` doesn't exist — `VortexHostConfig::fromStore` is
    `vortex_host.cpp:37–98`; `settings::` constants `system_docs.hpp:135` (`kVortex*`
    :261–265). P27 `~:5502` vs §2.3 `~:5674` — actual :5674. P34's "mirroring the primedia
    spool" overclaims — primedia `readOnly` is a silent no-write at `flush()`
    (`segmented_primedia_spool.cpp:352`), the named refuse is new for both spools.
    `mesh_evaluator.cpp:702` → `bindInput` at :696.

Confirmed good (verified): `registerProgramImage` Int64-only decode matches the
positional-member design; `handleTarget` arm correctly new; mark/`release`/`discard` +
dead-primedia verified (`arena_manifold.cpp:1596–1700`); `mintOpcode` self-registration
(:157–165); `UserPermascroll` ctor write order vs P34(b) complete
(`user_permascroll.cpp:349–395`); `Store::load` read-only modulo the two opens
(`store.cpp:4358–4454`); `wingOf` Int64 arm + `contains` gate (`vortex_core.cpp:383–399`).

VERDICT: NOT CONVERGED
