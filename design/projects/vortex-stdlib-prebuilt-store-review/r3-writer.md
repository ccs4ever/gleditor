# Round 3 critique: Technical Writer (vortex-stdlib-prebuilt-store.md)

Paths relative to repo root. Tally: 0B/1M/7m.

## MAJOR

### 1. (§5.2.4, P28, §6.4, §11.5) `registerProgramImage(map[home])` cannot deliver record-scoped registration — it is a first-text-match scan

`registerProgramImage(CellRef imageRecord)` treats its argument as a *holder* and walks
`rankAfter(imageRecord, dims.vars)` returning on the **first** `"ops"`-texted record
(`vortex_vm.cpp:130–137`, `break` at :136). `map[home]` is the shared arena `home`, so the
call registers the earliest `"ops"` record on the rank — not "each grafted image's own
mapped record" as stated. Two documented flows break:

- §6.4 local image: base `"ops"` tail-appends first, so the local call re-registers the
  base record; the local image's opcodes never register — contradicts "its own `"ops"`
  record registers" and §11.5's "both `"ops"` records registered".
- Any foreign `"ops"` record ahead on the rank (name is user-collidable — doc's own
  caveat §3.1/P28) is first-match: `unexpected` refusal or silent misregistration of
  document cells — the exact hazard P28's Problem section warns about.

**Fix:** the scoped mechanism is P28's own "Fallback" — the graft already walks each
grafted `"ops"` chain to translate members; register via `registerProgramOps(span)`
(`vortex_vm.cpp:97–119`) over the walked members, or spec a `registerProgramImage`
overload taking the record cell directly (read `record +d.values → opsHead`, no name
scan). Absent record → skip = the specified no-op. Update §5.2.4, P28 Change/Fallback,
§6.4.

## MINOR

1. §4 generator `Config{…, readOnly = false, …}` implies `readOnly` pre-exists — `Config`
   (`user_permascroll.hpp:103–114`) has no such member; P34 specifies it as NEW. Drop the
   field from the init or annotate "(P34's new field; omit until it lands)".
2. `exportStandardLibraryToStore` cited `~:5502` in P27 Location but `~:5674–5691` in §2.3;
   actual `:5674` (`:5502` is inside `buildBridgeModule`). Re-pin P27.
3. §5.1's "five-step" dir chain omits the `vortex.stdlib_image_dir` store-key step that
   §5.4/P31 place between explicit setter and env. Insert it as step 2 or state it folds
   into "explicit path".
4. `§5.2.3/.4/.5`, `§8.8`, `§2.3.1–9` cite numbered *items* not headings — convention works
   but is never declared. One clause in the drift header: "§X.Y.N cites numbered item N
   under §X.Y".
5. "`"ops"` minted at the end of each `build*Module`" (§4, P28) literally reads as 21
   records vs P28's "one record" cost; pin: single record, each module appends its opcodes
   (or reconcile with the MAJOR fix if N records are intended).
6. "Stage-3 mesh contexts" (§5.1) uncited — it's `vortex-multithreading-study.md`'s
   Stage 3 (parallel cursors/worker contexts). Cite it.
7. `(critique skill §1, rule 2)` in §8 item 4 — resolvable but no path; name
   `.claude/skills/vortex-design-critique/SKILL.md` or drop.

## Verified clean

- §5 diagram order (ensureCells → graft/bootstrap → rebindInstance) matches text.
- No residual "shared marker" claims; per-ref convention consistent §3.1/§4/§5.2.3/P26.
- `Config.readOnly` correctly new in §5.1/P34 (only the §4 init nit).
- §5.4 matrix mid-graft column; §6.4 shadowing rule; U-X4/U-X6 consistency.
- Module list = 21, mint order matches `vortex_stdlib.cpp:1508–1530`.
- ~60 spot-checked `file:line` anchors resolve (wingOf/bindInput/contracts/pipelines,
  mark/release, insertIntoRank, handleTarget, promote arms, spool opens, Store::load,
  refCell/varsRecord, lineage restate+:564, registerProgramImage/ops, test :674,
  Makefile:1786/1842–1848, all second-instance sites).

VERDICT: NOT CONVERGED
