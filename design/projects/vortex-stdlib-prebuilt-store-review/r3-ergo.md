# Round 3 critique: Ergonomics Expert (vortex-stdlib-prebuilt-store.md)

Paths relative to repo root. Tally: 0B/1M/7m.

R2 dispositions verified: R2-M1 landed (§4 step 1 — records mint "at the end of each
`build*Module` so every arena — `Cpp`-mode included — is self-describing"; §5.2.5 "present
in *both* modes"; P28 "no `"ops"` record … a no-op, not a refusal"; P29 goal-trio
`{in:{},out:{}}`; §11 Cpp-parity gate). R2-M2 landed (`opcodeMap_` clear+re-register,
`hasOpcodeKind` map-only verified `vortex_vm.hpp:194–196`, shared-VM constraint named).
R2-M3 landed (mark-scoped graft, `release` on refusal, post-`discard` registration,
matrix column). R2-M4 landed except env-value spellings. "Stale" disambiguated (§5.4
footnote). §1 module list verified against `vortex_stdlib.cpp:1508–1530` — exact match.

## MAJOR

### 1. (§5.2 step 4, P28, §6.4, §11.5) `registerProgramImage(map[home])` cannot implement record-scoped registration — it resolves `"ops"` by first-text-match

`registerProgramImage(imageRecord)` walks `imageRecord +d.vars` for the **first**
`"ops"`-texted record (`vortex_vm.cpp:130–137`). `map[home]` is the arena's shared `home`,
so the scan covers *every* vars record — document records, user records, base and local
grafted `"ops"` — and returns the first on the rank. Three specified behaviors are
unsatisfiable as written:

- **Foreign `"ops"` collision** — doc promises "foreign records are never scanned"
  (§3.1, §5.2.4), but any earlier `"ops"`-texted record wins first-match: its Int64
  members decode positionally → `unexpected` ("ops member does not name a cell",
  `vortex_vm.cpp:148–151`) turning a good image into refusal/rollback, or silent
  misregistration.
- **Local-image registration impossible** — §6.4/§11.5 require the local image's own
  `"ops"` to register, but `registerProgramImage(map[home])` re-returns the base record
  (tail-appended first) every time.
- **Absent-record no-op unexpressible** — `registerProgramImage` returns `unexpected`
  for a missing record (`vortex_vm.cpp:138–141`), indistinguishable from malformed
  members without a pre-scan.

**Fix:** graft captures `map[opsRecord]` while translating record members, then feeds the
translated span to `registerProgramOps` (the P28 Fallback itself admits this equivalent) —
or spec a `registerProgramImage` overload taking the record cell directly. Record-absent
→ skip = the specified no-op.

## MINOR

1. Accepted env/store-key spellings never stated (`=cpp` §11.7 vs `stdlibSource=Image`
   §11.8 use different casings); state the `GLEDITOR_STDLIB_SOURCE`/`vortex.stdlib_source`
   value grammar incl. case sensitivity.
2. `vortex.stdlib_image_dir` is effectively dead config — image folds lazily on first
   `ensureCells` in the primary ctor, before `loadConfigFromStore`; say plainly that
   repointing is setter/env before first context (process-start config) and `status()`
   exposes the folded dir.
3. Local-image failure semantics unpinned: absent = silent skip; malformed local under
   `Image` = scoped refusal (rollback local graft, warn, keep base — never re-bootstrap);
   `paths::dataPath("xuzz",…)` implies per-app dirs and no headless chain; how a user
   *produces* a local image unstated.
4. Precedence `store keys > env` unconventional and unjustified — env-as-override is the
   norm; invert to `setter > env > store > default` or justify.
5. §12 "memory status quo per arena" understated — sig records + ops members + markers add
   ~+4K cells/edges per arena in `Cpp` mode; shared image ~8K cells not ~5–6K. Restate.
6. `StdlibImageRefused` granularity gaps: missing-permascroll-dir cause vs `ScrollTruncated`
   distinct? Legacy per-module image lacking `home`/`d.dims` prefix → named `FormatRefused`?
7. `home +d.vars` record surface varies by mode ("ops" always, `"stdlib.image"` only in
   image mode) and by graft count (two `"stdlib.image"` after base+local) — one sentence +
   `status()` as the provenance probe.

VERDICT: NOT CONVERGED
