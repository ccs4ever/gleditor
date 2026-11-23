# Restart: performance architecture implementation

Continue the authorized work in `/data/git/gleditor-restore`. Do not stop at planning. The owner
accepted P1–P5, release ThinLTO/PGO, the implementation order, and iterative critique using **Luna
subagents**, including an independent C++ performance expert. Implement, measure, fix findings and
repeat review until convergence. Accepted fallback choices are legitimate when measurements reject
an optimization. Never declare the whole goal complete while M3–M6 remain unfinished.

The owner requested this handoff because usage was low. No further implementation work should be
inferred as completed beyond the evidence below. The background PGO work finished during handoff
preparation. **Read the final handoff update below before the earlier process snapshot.**

## Final handoff update: overrides earlier running-process snapshot

Both recorded processes have exited. The C2 PGO driver completed generation, training, merge,
release, artifact snapshot, primary timing and hardware collection. The follow-through stopped at
its hardware-report success assertion, before full runtime tests, paired comparison and held-out
analysis. Do not infer those gates passed.

`build/performance/variants/m3-hybrid-unordered-c2-pgo/hardware.json` reports:

- `success: false`, `identity_unchanged: false`.
- Failure: `source or artifact identity changed during collection`.
- 168 samples, 21 workloads, and an empty `counter_failures` list.
- The initial release manifest matched source
  `b413cd256f60ec0549d0a8fa3b181026272a059357391ac8e9463f8689f5cf65`.

First compare the report's before/after source identities and release manifests to identify the
exact mismatch. Preserve this failed report. Do not bypass the identity guard or treat raw counters
as accepted evidence. Check primary timing's independent identity status too. Repair or repeat only
the affected collection after establishing exact source/artifact identity, then resume the remaining
follow-through gates. No source change was intentionally made during this handoff; Markdown and
inert patch/JSONL archives were saved. The cause of the mismatch has not been investigated.

## Read first and constraints

1. Read the repository `AGENTS.md` and relevant README sections. The system-architect skill was
   applied; read `.claude/skills/system-architect/SKILL.md` when continuing its review workflow.
1. Read `design/performance-architecture-implementation-plan.md` and the durable journal
   `design/projects/performance-architecture-implementation-progress.md`.
1. Proposal: `design/performance-architecture-proposals.md` (accepted v8). Reviews are in
   `design/projects/performance-architecture-implementation-review/`.

Important operational constraints:

- GNU Make only, **always `make -j$(nproc)`** for builds. Here `nproc` is 32. Do not hardcode a new
  compiler or language standard; use Make's probes/expanded flags.
- **Never inspect `.worktrees/vql-completion`.** Do not kill other worktrees' processes. Previous
  unrelated orphaned processes were left untouched; no current dedicated-host claim is justified.
- All runtime/tests headless. Make exports the six variables; outside Make set all six:

```sh
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  XDG_DATA_HOME=$PWD/build/xdg/data XDG_CONFIG_HOME=$PWD/build/xdg/config \
  XDG_CACHE_HOME=$PWD/build/xdg/cache <command>
```

- Normal shell sandbox setup fails because `.agents` crosses a writable symlink. Root used
  `exec_command` with `sandbox_permissions: "require_escalated"` successfully. That setup error is
  distinct from an actual approval-review quota rejection. If quota is rejected, stop that route; do
  not bypass with a different tool/model/new agent. User authorization persists.
- Format C++ with clang-format **19**, root `.clang-format`. For `/tmp` overlays use explicit
  `--style=file:/data/git/gleditor-restore/.clang-format`; isolated default style caused churn
  twice.
- Markdown: mdformat with `--wrap 100` and gfm/dollarmath/frontmatter extensions. Touched Markdown
  lint uses `GEM_HOME=$(ruby -e 'print Gem.user_dir') mdl -i -s .mdl_style.rb ...`.
- No thirdparty edits, no persistent layout changes. CompactOpNode stays 64 bytes/aligned; fixtures
  have not changed. The new tables are derived state in the xanalogical engine only.
- Keep source frozen through profile generation/training/merge/use/validation/measurement. The
  profile driver rejects source/flags/compiler/tool/profile mismatches. Never weaken stale-profile
  errors or reuse an old profile for changed source.
- `collaboration.send_message` does **not** start an idle/completed agent. Use `followup_task`.
  Available existing agents are Luna; new ones, if needed and allowed, must use the requested Luna
  model with `fork_turns: "none"`. Do not create replacements to evade a quota rejection.

## Current root and background processes: first action

Root contains uncommitted experimental **C2 final candidate** source, plus pending journal/review
files. These edits are ours; preserve them. Current source fingerprint:

```text
b413cd256f60ec0549d0a8fa3b181026272a059357391ac8e9463f8689f5cf65
```

A fresh ThinLTO+PGO arm for that exact source is running:

- Driver PID **2499794**, script `/tmp/perf-m3-pgo-run.py hybrid-unordered-c2-pgo`.
- Follow-through PID **2499795**, script `/tmp/perf-m3-c2-pgo-follow-through.py`.
- Driver log: `build/performance/iterations/m3-c2-pgo-run.log`.
- Follow-through log: `build/performance/iterations/m3-c2-pgo-follow-through.log`.
- Variant archive: `build/performance/variants/m3-hybrid-unordered-c2-pgo/`.
- At handoff, generation passed **731.68s**, training **53.70s**, merge **8.10s**; optimized release
  build was still running. These may finish after this prompt is written.
- Profile SHA:

```text
 ef269cebb31f8df34f06165c1e0554abf27822b434305a58badb10a6121a2bd7
```

Check state with small reads, not a second build:

```sh
ps -p 2499794,2499795 -o pid,etime,stat,comm
cat build/performance/iterations/m3-c2-pgo-run.log
cat build/performance/iterations/m3-c2-pgo-follow-through.log
cat build/performance/variants/m3-hybrid-unordered-c2-pgo/perf-release.resources.json
```

The release driver buffers compiler stdout until Make completes. The on-disk
`build/performance/profile-use-build.log` and `validation-report.json` can still belong to the
previous run while this build is active. Do not claim their old contents describe the new run.
`pipeline.json` normally remains `merged` even during/after profile-use. An active lock is not a
stale lock; do not delete it while its owner is alive.

Driver sequence: instrumented generation, 30 training workloads, merge, optimized release plus 21
held-out validation cases, snapshot of 26 artifacts, seven-run held-out timing with warm-ups and
separate allocation counts, then 168 hardware samples. Follow-through waits for successful hardware
report, runs full Xuzz/ZigZag suites, then the 252-sample P3/C2 PGO probe comparison, its analysis,
21-case independent held-out analysis, and disassembly. Check stage return codes/reports; process
existence alone is not success.

Expected result locations:

```text
build/performance/comparisons/m3-c2-pgo-interleaved/{report,analysis}.json
build/performance/comparisons/m3-c2-pgo-heldout-analysis.json
build/performance/iterations/m3-c2-pgo-full-{xuzz,zigzag}.{log,json}
```

If a full UI test fails, preserve its workspace/captures/transcripts before rerunning; the test
removes its fixed workspace on its next invocation. Known case workspace is
`build/integration_workspace_foreign_drag`. Do not label an unexplained transient as diagnosed.

## Completed milestones and commits

- M1 complete: reproducible ThinLTO/PGO release pipeline, strict provenance and held-out validation.
  `387eff44 perf: add reproducible ThinLTO and PGO release pipeline`.
- M2 complete: retain guarded local `ArenaManifold::linked` traversal (P3). Measured accessor moves
  (P4) were rejected and reverted, per expert/fallback gate. Exact accepted source fingerprint:

```text
09da6e1b74a0cd90328882713f3591b03f6f84285552aa903871c9530a930873
```

- Relevant commits: `0c029b60`, `7ba0513e`, **`3142aac4`** (M2 closure).
- Latest HEAD **`ed09f5b7`** is documentation/evidence only, following `384bc24f`. Neither accepts
  or commits the M3 experimental implementation. Root CPP files remain modified/untracked.
- M0 harness exists; detailed copied-payload/trail/table telemetry remains pending for M5.
- M4/M5 are isolated preparations only. M6 final retrained release remains pending.
- M1 full checks passed library 842, Xuzz 4380 then-current tests, ZigZag 268, network and backend
  checks. Global format-check passed then; global Markdown lint had pre-existing debt (590 findings
  in 162 unchanged files). Touched Markdown lint passes; do not silently call skipped/global lint
  green or clean unrelated debt.

## M3 experiment history: do not select the wrong candidate

Three measured ThinLTO representations:

- A `m3-hybrid-flat`: dense/presence byRef + flat sparse overflow + flat births; source
  `c427cb45...`.
- B `m3-reserved-unordered`: staged PMR unordered maps for both; source `b7773c5c...`.
- First C `m3-hybrid-unordered`: dense/overflow byRef + staged PMR unordered births. Measured
  ThinLTO revision `e1ad7a0d...`; final first-C PGO source `18deb2a5...`.

Full correctness passed A/B/C. A saved allocation calls but flat births cost roughly 101.19 MB
current /151.78 MB peak PMR bytes at one million rows, versus about 64.45 MB staged unordered in
that experiment. These are allocation requests, not resident memory. First A synthetic global-hint
memory results were not the actual branch-local Store policy; do not conflate them.

First C fresh PGO completed but **was not accepted**. Whole-scenario held-out timings masked kernel
regressions. Paired C/P3 text ratios were 4.780/2.247/1.285 at 144/1k/10k; branch 2.691/2.359/2.580,
while larger structure kernels improved. Archived report:
`comparisons/m3-pgo-interleaved/{report,analysis}.json`. All raw evidence, rejected patches and
review rationale are committed as `iteration-03-*` files. A's original patch missed untracked
helpers; `ed09f5b7` reconstructed the complete 11-file patch from verified backups and checked all
hashes after applying it to a fresh P3 copy. No measured binary was changed by that archive repair.

C2 is the bounded correction, same representation:

1. Small `applyStructure` dispatch handles ignored non-Structure/no-cell contexts without the
   allocating mutation frame; zero-index refusal and cell-context/scratch/PageBreak semantics stay.
1. `optional<size_t>` distinguishes unknown ancestry count from known zero. Known count uses tight
   `2*n` eligibility; unknown raw callers retain 4096 fallback. Advance increments known only and
   restores exact optional on resource refusal. Overflow remains searchable after hint changes.
1. Birth preparation uses destination `try_emplace` then immediate `extract`, no second staging map
   or explicit per-insert reserve. Successful temporary insertion proves final-size capacity;
   extraction preserves it, and unchanged-generation node publication allocates nothing.
1. Prepared tokens are opaque owner-constructed/move-only. Walker nonzero ancestor -> zero target
   correctly fails without callbacks. Dead geometric helper/test removed.
1. Final token move assignment destroys/reconstructs via noexcept move constructor, avoiding PMR
   node-handle unequal-allocator move-assignment UB; distinct-resource tracking test covers it.

C2 r1 full build passed but two fixtures failed (not production bugs): unknown-hint test advanced
without replaying genesis; chain-failure test preallocated overflow via hint=1. Corrected fixtures
are in root. C2 r2 source `b8f95f7f...` passed 208 focused tests. First full run had one UI capture
failure (`ForeignRunDrag...`, scaledBounds), then two isolated passes and an unchanged-source full
rerun passed **4428 Xuzz, 28 skipped, 12 disabled; ZigZag 268**. Original failure JSON/log retained;
original image was overwritten by repeats and no image-based cause was claimed.

C2 r2 ThinLTO passed 21 pilot cases, seven repetitions + warm-up, 168 hardware samples and 252
interleaved samples against a **fresh P3 ThinLTO control**. Both fresh P3 files were 16 bytes larger
than old archive, with equal `.text` sizes; exact cause unknown. New hashes/functional smoke checks
were recorded, not relabeled as original hashes. PGO P3 pair below *did* match original hashes.

C2/P3 ThinLTO ratios: text 144 **1.164 [1.080,1.493]**, 1k **0.910 [0.818,0.965]**, 10k **0.853
[0.757,0.964]**. Branch intervals crossed one (10k wide); structure 1k **0.948 [0.930,0.968]**,
other structure and five link intervals crossed one. Public dispatch 164 bytes / three pushes, no
former 408-byte mutation frame; heavy body separate. No noinline attribute was needed. Expert
approved one fresh final C2 PGO arm, **not retention yet**.

Current final source adds only the allocator-safe token move fix beyond measured b8. Author ran 13
prepared-map tests normal + ASan/UBSan and syntax checks; expert endorsed the fix. The fresh PGO run
will validate this exact source, including its extra test.

## M3 next steps after background run

1. Verify final C2 PGO provenance, runtime checks and reports. If failed, fix actual cause without
   weakening profiling or inventing passing evidence. Preserve failure logs/captures before repeats.
1. Ask independent Luna C++ expert to review actual final paired and 21-case held-out reports,
   memory/header costs, limitations and source identities. Do not equate whole-scenario timings with
   fold kernel timings. Seven serial repetitions are not run-index paired; use independent bootstrap
   for old/new serial held-out blocks. Probe runs are temporal paired blocks. Intervals are
   exploratory/unadjusted on a shared host; no invented workload weights or universal speedup.
1. Converge on C2 or the accepted original/reserved-map fallback. Do not hide cold-text penalties.
   Avoid an unbounded fourth storage experiment; C2 was the bounded correction.
1. If retaining C2, update proposal/implementation-policy prose: known counts use tight bounds,
   unknown retains floor; unordered births chosen on measured cost. Record actual capacity slack,
   header and PMR costs. No persistent format/fixture changes.
1. Run required appropriate checks, archive final reviews/evidence, commit reviewable M3 source.
   Only then integrate M4. Do not mark overall objective complete.

Memory sources: `/tmp/perf-m3-c2-memory/` (also archived restart assets). C2 Manifold **488B** vs P3
296B and first C 544B; fixed +192B vs P3 is separate from PMR allocations. Three-key fixture
`1,5000,9999`, known count148: 64 dense values (256B) +8 presence +8 overflow buckets (66B) =330B.
The same current C2 with intentionally inflated *known* hint10k uses 41,250 current /61,875 peak.
That is a sizing control, not unknown fallback or historical C1 production measurement. Old ABI
artifact is `build/performance/comparisons/m3-abi-accounting.json`. PMR peak excludes allocator
metadata and pointer-ledger allocations, not RSS/virtual reservation. C2 births now use natural
unordered growth; use its new CSVs, not first-C reserved-growth numbers.

Verified accepted P3 PGO probes/profile: `build/performance/variants/m3-p3-baseline-pgo/`. Exact
original probe hashes and runtime libraries were verified after reconstruction with the baseline's
own profile. No private engine DSO. Fresh ThinLTO P3 probes: `m3-p3-baseline-thinlto/` with explicit
new identities. First-C/C2 binary pairs and complete hash-verified sources are in their respective
variant archives.

Useful scripts (all `/tmp`, exact text can be archived in evidence):

```text
perf-m3-pgo-run.py
perf-m3-c2-pgo-follow-through.py
perf-m3-c2-pgo-interleaved.py
perf-m3-c2-pgo-interleaved-analyze.py
perf-m3-c2-pgo-heldout-analyze.py
perf-m3-inspect-fold-dispatch.py
perf-m3-c2-export-evidence.py
performance-stage-resources.py
performance-m3-snapshot.py
```

`perf-m3-c2-export-evidence.py` is prepared but **not yet run**, since final PGO results are
pending. It writes a separate C2 JSONL archive. Resource RSS values are maximum child RSS, not
aggregate concurrent process-tree RSS. Inspect every stage result. Restoring a profile uses actual
APIs `validate_profile_manifest`, `llvm_profdata_identity`, `source_identity`; there is no
`validate_recorded_llvm_profdata` helper.

## M4 preparation (not integrated)

Overlay `/tmp/perf-m4-operand-list/`, patch `/tmp/perf-m4-operand-list.patch`. Latest patch hash:

```text
50cbbd102f65887827294de6488f0a6d94b64df57d2177444b7d9f8d475f1d8a
```

Eight-inline `CellRef` list with unbounded vector spill; 64B default host object; no vector
conversion. Core/VM/frame/stdlib/VQL/VPL consumers migrated; resolveCell read boundary becomes span.
Opcode register exclusions/map/numeric fallback precedence unchanged. Five helper tests passed
native and forced Beman, normal and ASan/UBSan; eight original TUs passed syntax only. No linked
engine/runtime/performance gate passed.

Latest expert found two missed vector comparisons, now fixed in overlay: `vql_conformance_test.cpp`
and `vortex_foreign_mask_export_test.cpp`. **Syntax checks for those two new TUs remain pending.**
Caller inventory `consumer-assumptions.txt`; recheck patch/apply and all callers. Do not treat the
earlier eight syntax checks as checking these new fixes.

Experiment `/tmp/perf-m4-operand-list/experiment/`: allocation/layout matrix 0/1/2/4/8/9/64/1024,
vector vs inline4/8 vs caller span vs visitor; normal/ASan passed matching checksums. No primary
throughput timing collected. `operand_timing_primary` is separate, no global new/delete replacement
definitions (nm confirmed); diagnostic allocation binary does not supply primary timings. Primary
source hash starts `fb94c5fc...`, binary `0040d8c6...`. Visitor has no retained mutable operand
list; span fixture knows arity and exact-reserves, unlike unsized RankView. Do not claim them
equivalent full integrations or infer application weights from synthetic 0/8/9 cohorts. Measure
thresholds, full app/test compile, runtime-slot rewrites, spill/copy/move failure, then
review/choose fallback.

## M5 preparation (not integrated; largest unfinished work)

Snapshot-only overlay `/tmp/perf-m5a-vm-integration/` and its README. Patch hash:

```text
3a78be6047f700e90a2287af3076e419a5d22b981a4053efe4c6469d6e5ccaf2
```

Ten private containers = eight maps + two sets; public RuntimeBindingSnapshot DTOs remain standard
maps. Existing snapshots/rollback retained, no trail enabled. Ten Vortex TUs passed syntax. Exact
latest helper passed **29 normal +29 ASan/UBSan** with leak checks. No engine link/runtime. Rebase
after M4 span/OperandList changes; keep patch hunks separate. Static architecture/C++ reviews found
no blocker, but call-site reference invalidation remains a linked/runtime gate.

Helper `/tmp/perf-m5-reversible-map/`: actual pair\<const CellRef,Value> references; explicit
occupancy; retain absent keys for allocation-free undo; owner/generation tokens; resources outlive
maps/tokens; copy/move failures tested. Convenience erase is irreversible; retained erase requires
explicit prepare/commit into caller-owned trail storage. Do not mistake ordinary operator[]/clear/
eraseIf/swaps for trail-aware writes. Snapshot-preserving container migration must be a separate
commit and pass before enabling trail/local savepoints. Extract common private hash if retained M3
overflow and M5 helper duplicate it; do not promote Xanadu code to generic library.

Mutation inventory `/tmp/perf-m5-mutator-inventory.md`; telemetry proposal
`/tmp/perf-m5-telemetry-plan.md`. The current vm-snapshots probe does not exercise mutable payload
copies. Count-only instrumentation must use separate count-object replacements/external fixed TLS
sink without conditional class-layout/ODR mismatch; no release/timing overhead, no extra shared
ownership inducing COW. Distinguish handle-retain events from unique physical payload versions;
post-window pointer-identity dedup is not exact high-water tracking. Do not turn unmeasured into
zero.

Important remaining P1 contracts: route every mutation including pruneAssemblyState; VM undo before
arena release; retain restorable keys; no allocation on unwind; per-boundary first-write payload
retention before uniqueness; child commit keeps outer records; root steal promptly frees dead prefix
owners and safely rebases; preserve Dewey/spark/callback metadata, local cursorPC, streamCursor.
Resume IDs/values stay untrailed. Include nested prune/rollback with dead refs (undo only moves
handles, no arena dereference), allocation/budget failures, mutable aliases and all oracle routes.
Do not retire snapshots until differential gates pass; no dual production engines in final P1. M6
must retrain final source and run all required correctness/performance/backend gates.

## Durable restart assets and agent handoff

The review directory now contains restart copies of M4 patch, M5a patch, inventory, telemetry plan,
M4 expert review and `restart-c2-final-candidate.patch` (unaccepted final C2 source against P3).
`restart-preparation-assets.jsonl` preserves source text/SHA records for m4, m5a, m5-helper and
c2-memory roots. If `/tmp` disappears, reconstruct these overlays from its labeled records; do not
silently treat absent drafts as finished work. No `.cpp/.hpp/.py` was copied as a live design input
while PGO was frozen; archives are inert patch/JSONL/Markdown data.

Existing Luna agents, if available in the resumed thread:

- `/root/m1_four_mode_cpp_review`: independent C++ performance reviewer.
- `/root/m0_telemetry_inventory`: architecture/codebase/measurement review.
- `/root/m2_accessor_draft`: isolated implementation author (latest M4 checkpoint above).
- `/root/m5_mutator_inventory`: code quality and reversible-container author/reviewer.

A new thread may not retain those agent handles; create authorized Luna replacements only for work,
not quota bypass. Keep four-slot limit in mind. Resume the actual unfinished M3 release gate first.
