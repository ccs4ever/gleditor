# Round 2 critique: Programming Language Design & Ergonomics Expert (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: 0 BLOCKING, 4 MAJOR, 7 MINOR.

All R1 dispositions verified in place. `resolve()`/`call()`/`modules()`/
`symbolsInModule` equivalence holds under graft + appends; `std:goal/halt`/
`std:goal/sentinel` naming is consistent with `std:goal` vocabulary.

## MAJOR

### 1. (§5.2.5, P30, §5.4 `Cpp` row) `rebindInstance` is specified image-only — the `Cpp`/fallback path cannot satisfy "today's behavior byte-for-byte"

`build*Module` populates `routineBindings_` inline but emits no `d.params`/`"ops"`/
`stdlib.image` records. On the Cpp path: `clear()` + rebuild-from-records → empty map →
`clamp` drops `hi` (§2.2 reborn); `registerProgramImage` → `unexpected`. Also breaks the
second-instance case on the Cpp path (worse than today).

**Fix:** (a) builders emit the records in-arena (self-describing arenas, uniform rebuild,
also makes Cpp-mode export work) — priced ~3.8K cells/bootstrap; or (b) gate each
record-driven step on presence + keep inline populate. Also state: absent `"ops"` record
is a no-op on the Cpp path (`mintOpcode` self-registers), not a refusal.

### 2. (§5.2.5, P30) `rebindInstance` clears `routineBindings_` but ignores `opcodeMap_` — the same dead-refs argument applies to the map that feeds the write guard

`bindManifold` move-assigns the arena while `primary_.vm` persists; stale `CellRef` keys
alias fresh cells → `hasOpcodeKind` write-guard refuses innocent user writes;
`getOpcodeKind` map-first reports bogus kinds. Naive `clear()` is also wrong — drops
non-content-decodable mints (`ScanPush`…) and a shared-VM second instance's
registrations.

**Fix:** on arena swap drop entries not naming arena cells — or clear-then-re-register
the manifest; name the shared-`VortexVM` constraint.

### 3. (§5.2, §5.4) The graft is not atomic — a mid-graft refusal leaves a poisoned, presence-probe-passing partial stdlib

Refusals fire after cells minted; no rollback → `ImageThenCpp` bootstraps *on top of*
debris; a partial graft can pass `resolve("std:math/abs")` → every later instance binds
a half stdlib with no refusal recorded.

**Fix:** graft inside a `Mark`; `release()` on any refusal (undoes cells+appends;
interned primedia stays as dead spans — harmless, say it); registration/rebind after
`discard`. Add the graft-refusal matrix row.

### 4. (§5.4) The `vortex.stdlib_*` override contract and "why did my stdlib load from C++" observability are unspecified

Consultation timing (per-`ensureCells` vs once-per-context — provenance-per-arena is
user-visible via §6.5 counts), precedence vs the explicit setter, the warning channel
(named spdlog category — the `vortex.host` convention), and a queryable effective-source
surface are all unstated.

**Fix:** pin: store keys apply to the process policy at `loadConfigFromStore` for
later-built contexts; precedence explicit-setter > store > env > default; warn/refuse via
`GLEDITOR_LOG_*("vortex.stdlib", …)`; expose `VortexStdLib::status()` returning the
effective source + recorded refusal.

## MINOR

1. (§5 diagram) `rebindInstance` above `ensureCells` — invert; `bindManifold` calls both.
2. (§5.4) Error-channel coverage: `modules()`/`symbolsInModule()` bypass the throw —
   "every public read checks the refusal first" or enumerate; bindManifold-path failures
   need recording too; `rebindInstance` must be total (`nullopt`-safe); `stdlibPresent`
   probe bypasses the gated `resolve()`; `what()` carries cause+epochs+dir+remedial env;
   `MarksOutstanding` missing from the enum; §6.4 local images pass §5.1 validation.
3. `std:goal/sentinel` — benign (bare `Halt` → `{}` like `fail`); state whether the pair
   get `{in:{},out:{}}` `d.params` records like `fail` for `vortex_param` symmetry.
4. `"ops"` record name is user-collidable on shared `home +d.vars` — `stdlib.ops` would
   match `stdlib.image`; keeping `"ops"` for `registerProgramImage` reuse is defensible
   — name the collision surface either way.
5. (§1) Module list duplicates `std:collections`, misorders `sys:array`/`std:map`/
   `std:hypertime` vs `vortex_stdlib.cpp:1508–1530` — this is the introspection-observable
   `modules()` order.
6. (§4 step 4/P27) `extraRoots{home, dims_.dims, stdlibRoot}` — `stdlibRoot` reads as a
   distinct entity though stdlib root = image `home`; drop or annotate.
7. Terminology: "stale" means format-stale (§5.4) vs version-old (§6.3) — disambiguate;
   `GLEDITOR_STDLIB_SOURCE` spellings unstated; U-X4 row note.

VERDICT: NOT CONVERGED
