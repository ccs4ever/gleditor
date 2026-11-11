# Round 5 critique: Codebase Expert (vortex-stdlib-prebuilt-store.md)

(Agent self-labeled "Vortex Expert" in its report; this is the codebase-critic retry.
Report reproduced verbatim below.)

Paths relative to repo root. Tally: 0B/1M/7m. **VERDICT: NOT CONVERGED.**

Every R4-disposition cluster re-verified against the current tree — all seven landed
correctly: two-cell holder matches `registerProgramImage`'s walk and the vagent
precedent; §5.2.3 ordered edge classes match `rankContains`/`link()`/`insertIntoRank`;
the Settings seam, per-field policy setters, `unique_ptr<State>`+`once_flag` holder,
and consumer lists all resolve.

## MAJOR

### M1 — The named local-image production path cannot produce a non-duplicating image — every reachable shape re-imports the whole base stdlib at graft

§6.4 names the local-image production path "a whole-space export of a scratch context
after dev additions, produced by `vortex-stdlib-gen --out <local-dir>`" — and P32 calls
`--module <name>` "the §6.4 local-image producer". But the generator's scratch context
is forced `StdlibPolicy{Cpp}` before it exists, so the ctor-time `ensureCells`
**bootstraps the full base stdlib into that scratch** — "after dev additions" the
scratch holds base stdlib *plus* dev work, and a whole-space export carries all of it.
Nor does `--module` help: `promote`'s reachability walk expands **both** `edge.pos`
and `edge.neg` (`arena_manifold.cpp:1791–1797`), so a module-rooted export floods
`module -d.stdlib→ home` and reaches the entire library — a "per-module" export is
whole-space too.

At graft, §5.2's name-merge covers only *image `home +d.stdlib` rank members* (module
cells — "excluded from cloning"). Symbol cells are never merged: every `module +d.vars→
sym` edge replays as a graft-minted member tail-appended onto the name-merged module
(§5.2.3 membership class). So a whole-stdlib local image re-appends **every base
symbol** — each shadowed by `resolve()` first-match (the doc's own collision rule).
Resolution stays correct, but `symbolsInModule`/`vortex_function`/`vortex_param`/
`findall` enumerate the duplicates — exactly the multiplicity §6.5 says the image
removes. A dev adding one symbol via the named path produces a graft carrying ~4,300
shadowed cells. The only non-duplicating shape — a scratch that never bootstraps the
base (dev-only modules) — is never stated, and its consequence isn't priced: dev
routines in a stdlib-less scratch cannot bind stdlib ops at build time, so dev modules
must be self-contained.

**Fix:** pin the local-image shape explicitly. Either (a) spec the dev-only scratch:
`--module`/`--patch` constructs `ArenaManifold`+`VortexCore`+`VortexVM` **without**
`VortexStdLib` (no bootstrap), mints only the dev module(s), exports whole-space —
self-contained modules only, stated as a v1 limit — or (b) accept a whole-stdlib local
image and name the consequence: every base symbol shadow-duplicates at graft, §6.5's
"single-bootstrap" claim scoped to the shipped image. If (b), also bound it — a
`--module` export needs a reachability bound `promote` does not have (a new mechanism
needing a P-item).

## MINOR

m1. "already in hand at `xuzz_app.cpp:1589–1593`" — the Settings store is not acquired
    there: `xuzz_app.cpp:1589` holds `session->store(0)`, `:1590` calls
    `bindXanaduStore` (itself triggering `ensureVortexHost` → ctor-time `ensureCells`),
    `:1592` acquires `systemStore(Layout)`, `systemStore(Settings)` only at
    `:1596–1597`. The window contains the host build itself. (Subsumed by architect
    M1 — the seam must move before `:1588`.)
m2. `bindManifold` reachability misattributed (§1:34, §5.2.5/P30) — "re-runs every
    time `bindManifold()` rebinds a document" + "the reachable driver is
    `bindXanaduStore`" are both false; zero in-tree callers; shipped path replaces
    the host wholesale. (Same cluster as ergo B1 / zigzag M2.)
m3. `varsRecord` cannot mint unscoped records — `varsRecord` with `mint=true` **always**
    files `record +d.owner→ Int64(owner)` (`records.hpp:113–118` — no scope flag), so
    the "deliberately unscoped" manifest needs a named mint path: a `varsRecord`
    overload/scope parameter or a `keyRecord`/`fieldRecord` helper (three lines of the
    same pattern minus the `+d.owner` arm). This also removes `varsRecord`'s
    unconditional `findOrMintDimension("d.owner")` (`records.hpp:101`) from the
    registry, keeping the dim count at ~21–22.
m4. `localImage` default under `Cpp` is ambiguous against "explicit opt-in" — if "app
    modes" includes `Cpp`, default-on contradicts opt-in and makes
    `GLEDITOR_STDLIB_SOURCE=cpp` + the §11 Cpp-parity gate silently pick up local
    patches (the exact thing `localImage=false` on the generator prevents). Pin the
    per-mode default: `true` under `ImageThenCpp`/`Image`, `false` under `Cpp`.
m5. "primedia interned by `makeCell(text)` stays as harmless dead spans" — wrong
    mechanism: `release()` truncates `slots_`/`links_`/`content_`/`scratch_`
    (`arena_manifold.cpp:1671–1674`); interned bytes are truncated with the cells that
    owned them, not retained as dead spans. (Harmless either way; fix the mechanism.)
m6. `ChoiceStackLive` and the map clears don't name the VM — `vplEngine_.ownedVm_` is a
    second `VortexVM` over the same arena with its own `choiceStack_`/`opcodeMap_`/
    `cursorPC_`/`memoizedOps_`. The robust form is already in the doc's toolbox:
    choice points take *arena* marks (`vortex_vm.cpp:320`), so
    `primary_.arena.outstandingMarks() != 0` on the swapped arena covers **every**
    VM's live choices — strictly stronger and simpler than enumerating VMs. Each
    instance's `rebindInstance` clears its own `vm_`'s tables.
m7. Translated Int64 cells keep image-space text — `setValueBits(Int64, map[ref])`
    rewrites bits only; the content span is cloned verbatim, so grafted cells show
    image-space renderings while bits name arena twins. Functionally inert (`asInt64`/
    `registerProgramImage` decode bits) but `textOf`/`xudu-dump` readers see stale
    values. Name it in the transclusion-honesty paragraph, or restate content spans
    alongside bits.
m8. Ambiguous cite — "already used by `or_parallel` at `:210`" reads as
    `arena_manifold.cpp:210`; the actual call is `or_parallel.cpp:210`.

## Verified-correct (abridged)

Two-cell holder + `registerProgramImage`/`registerProgramOps` gates; ordered graft
classes; per-cell marker + OpHandle-gated `isCellRelocationHandle`; `wholeSpace`
d.dims lifting exact; Settings seam timing; worker/consumer inventory (`vicon` in /
`vquery`/`vplc`/`vlace` out); `VortexCore` ctor-only caching → resync needed;
P26–P33 anchors (bindInput/Output Int64 slots, wingOf/masterOf, opref/contCell/lit,
goal trio, 19 cached preds, `goalHalt_`/`goalSentinel_` unexported → P33 required);
process state/read-only (`outstandingMarks` public, `release()` truncates, choice
points take arena marks, `O_RDWR|O_CREAT` only, `permascrollState` throws,
`~UserPermascroll` unconditional `flush()`); data path/policy (`paths::dataPath`
honors `XDG_DATA_HOME`, `findAssetDir` env-only, `DocumentId` random, `SettingSpec`
pattern); `SetValue` retargets span+value, `extraRoots` seed order/frontier,
`formatVersion = 2`.

VERDICT: NOT CONVERGED
