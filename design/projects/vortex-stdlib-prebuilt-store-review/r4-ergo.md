# Round 4 critique: Ergonomics Expert (vortex-stdlib-prebuilt-store.md)

Paths relative to repo root. Tally: 0B/3M/7m.

R3 landing verified from the user's seat: holder-nested registration is implementable
(`registerProgramImage` walks `holder +d.vars` → `"ops"` → `+d.values → +d.step`,
`vortex_vm.cpp:121–155`; the vagent `"imageRecord"` holder precedent,
`lineage.cpp:332–337`); foreign-`"ops"` never scanned; absent-`"ops"` skip; residual
foreign-`"stdlib.image"` honestly stated; env grammar consistent; `stdlib.image` as
version-record-and-holder reads clean; local-image failure contract stated.

## MAJOR

### M1 — §6.4 "User patching" names a production path its own validation refuses

§6.4 says the local image "is produced by `vortex-stdlib-gen --out <local-dir>` or the
`:export-lib` flow against a scratch context." But `:export-lib` drives `exportLibrary` —
**per-module** (`zigzag_visualizer.cpp:3820–3831` → `vortex_host.cpp:505`; P32's own
fallback repeats it) — lacking the genesis `home`/`d.dims` prefix, which §5.1 names a
`FormatRefused`. "Against a scratch context" doesn't fix it — `exportLibrary` is
per-module regardless. `vortex-stdlib-gen --out` is a build tool never stated as
installed and emits the whole C++ stdlib — a full shadowed duplicate, not a patch.
`VortexHost::exportStandardLibrary` (`vortex_host.cpp:519`) is reached only by tests.
Net: "User patching" specifies no reachable way to author the one thing it exists for —
and §11.5's gate assumes producing exactly that. **Fix:** name the real flow (a
whole-space export command / `exportStandardLibraryToStore` on a scratch context, or an
installed `vortex-stdlib-gen` with `--out` spec'd in P32), or accept per-module local
images under a different validation arm, or state plainly that v1 local-image authoring
is developer-only.

### M2 — `MarksOutstanding` fallback dodges into the hazard the check exists to refuse

`outstandingMarks() == 0` is an entry gate because a graft under a live mark leaves
untrailed `opcodeMap_` refs to truncated slots. But the specified `ImageThenCpp` behavior
is "records the refusal and falls back to C++" — i.e., runs `bootstrap()` **under the
live mark**, which mints trailed cells *and* writes `opcodeMap_` via `mintOpcode`
(`vortex_vm.cpp:157–166`) — identical corruption, now silent because it's labeled
"fallback". `routineBindings_`/cached refs equally untrailed. Today's call sites are
mark-free — a latent trap armed for the first mark-holding caller. **Fix:** the gate
refuses `ensureCells` outright in all modes (or explain why bootstrap-under-mark is safe
when graft-under-mark isn't).

### M3 — "Settings store only" has no enforcement point at the named site

`vortex.stdlib_source` honored "only from the Settings store" — but the named read site
is `fromStore` (`vortex_host.cpp:37–98`) + `loadConfigFromStore` (`:169–172`), which
`bindStore` calls unconditionally (`:160–166`) with *document* stores
(`zigzag_visualizer.cpp:2860–2864`). `fromStore` takes no store-kind input;
`SystemStoreModel::fromStore`/`getSetting` is store-agnostic (`system_docs.hpp:
899–909,967–968`). Every other `vortex.*` key is already document-steerable this way.
Settings-only requires a discrimination mechanism (call-site tag, or a separate read
path used only by the `xuzz_app.cpp:1596`/`:4226` Settings calls) — never named; the
natural implementation silently violates the promise. **Fix:** name the mechanism.

## MINOR

m1. `status()` is load-bearing but undefined — location (`VortexStdLib::status()`?
    `VortexHost`?), signature, field list, and an in-band surface (a `vortex_*` builtin
    or REPL command) for vpl/vprolog/vagent users — one paragraph pins it.
m2. Unrecognized mode spelling has no disposition — `=Image` silently falls to the next
    precedence level, masking the very `ImageMissing` the user meant to exercise. Pin:
    warn naming the bad value and fall through, or refuse.
m3. `ChoiceStackLive` trigger + post-state unstated — it fires when a `bindManifold` runs
    while a produce cursor / open alternatives exist (results still streaming); does the
    refusal abort the swap (old arena retained) or record post-`move-assign` (Marks still
    corrupt)? Name trigger + recovery.
m4. "holder" names two objects — the manifold record (`stdlib.image`) and the
    process-static `{once_flag; optional<Triple>}` C++ holder — rename one.
m5. Refusal-matrix warn asymmetry: `image absent → "C++ fallback"` carries no "+warn"
    while every other fallback warns — for an installed app absence *is* the packaging
    failure. Justify or warn.
m6. `vortex.stdlib_source` is inert for every non-xuzz consumer (`loadConfigFromStore`
    called only from `xuzz_app.cpp:1596`/`:4226` and `vortex_host.cpp:165`) and
    `paths::dataPath("xuzz", …)` hardcodes the xuzz data dir — one sentence each.
m7. `rebindInstance` enumeration error-disposition under-specified: verbatim
    `registerProgramImage` returns `unexpected` on absent `"ops"` (`vortex_vm.cpp:
    138–141`) — the skip needs a `varsMember` pre-check or error-map, say which; a
    malformed foreign `"stdlib.image"` aborts `registerProgramOps` mid-list non-atomically
    (`:97–119`) — does the enumerate-all loop abort or continue?
m8. New introspection surface undisclosed: `d.params` + `d.mesh.relocation` join the
    visible registry and every wing slot carries a permanent marker edge in-arena —
    `bundle-stdlib`/dim listings gain entries; §6.5 should disclose it like the symbol
    change.

VERDICT: NOT CONVERGED
