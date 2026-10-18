# Critique panel findings — vortex-multithreading-study.md (loop 4)

Panel: interpreter expert + system architect + distributed systems expert + C++ concurrency expert,
reviewing jointly. Scope: factual accuracy vs. the real code, technical soundness of the threading
recommendation, internal contradictions, concreteness of each frontend's thread-primitive exposure,
AGENTS.md constraints, and mdformat/mdl cleanliness. Every citation below was re-verified against
the source in this worktree.

## Verification performed (all citations re-checked, all confirmed correct)

| Study claim | Verified source | Verdict |
| --- | --- | --- |
| VM four mutable members | `vortex_vm.hpp:148-151` (`opcodeMap_`/`memoizedOps_`/`cursorPC_`/`choiceStack_`) | correct |
| `cursorPC_` written every step | `vortex_vm.cpp:120` (`cursorPC_[cursor] = op;`) | correct |
| `spawnCursor()` | `vortex_vm.cpp:127` | correct |
| `+d.cursors` link call | `vortex_vm.cpp:139` (call spans :137-139) | correct |
| `stepScheduler()` serial `for` body | `vortex_vm.cpp:1076` def, loop body :1079-1087 (brace :1087) | correct |
| `VortexCore` one `ArenaManifold &arena_` | `vortex_core.hpp:256` | correct |
| `d.name` "Thread, pin, and store naming rank" | `vortex_core.hpp:48` | correct |
| host ctor + init list | `vortex_host.cpp:76` ctor, `:77-78` init list, members arena_/core_/vm_/stdlib_/vqlEngine_/vqlCompiler_/vplEngine_ (no vprolog) | correct |
| `compileAndAttachVQL(..., bool spawnThread)` + `if (spawnThread)` | `vortex_host.cpp:513` / `:526` | correct |
| `sys:gc` cursor | `vortex_host.cpp:86` | correct |
| core primitives link/value/newCell/breakLink/pin/collectGarbage | `vortex_core.cpp:124/165/220/235/454/509` | correct |
| memo getOrCreate/lookup/record | `vortex_core.cpp:515/524/537` | correct |
| arena class comment "dense vectors only. No ops. Dies with its owner." | `arena_manifold.hpp:15` | correct |
| arena backing vectors | `arena_manifold.hpp:661-666` (slots_/links_/content_/scratch_/trail_) | correct |
| overlay_/shadowOrder_/floors_ | `arena_manifold.hpp:675/678/696` (study range :675-696) | correct |
| `Mark` has twelve `uint32_t` fields, all named | `arena_manifold.hpp:169-184` | correct |
| `makeCell()` / `compact()` / `promote()` | `arena_manifold.hpp:518 / 624 / 759` | correct |
| `linkedMinting` / `bindSharedIdentities` | `arena_manifold.hpp:460 / 377` | correct |
| compact refused while mark outstanding | `arena_manifold.hpp:612-624` doc comment | correct |
| `promote(store, parent, from, root, budget)` signature | `arena_manifold.hpp:759` | correct |
| stdlib `map` def / `fnOp` overload | `vortex_stdlib.cpp:2212 / 2224` | correct |
| stdlib `filter` def / `fnOp` overload | `vortex_stdlib.cpp:2232 / 2248` | correct |
| stdlib `fold` def / `fnOp` overload | `vortex_stdlib.cpp:2257 / 2269` | correct |
| `function_ref` surfaces | `vortex_stdlib.cpp:2214 / 2234 / 2259` | correct |
| `call` spawns cursor | `vortex_stdlib.cpp:2075 / 2093` | correct |
| `CellRef` is bare `std::uint32_t` | `dim_vector.hpp:21` | correct |
| `sizeof(CellSlot) == 32`; SlotRef "valid until the next fold or write grows it" | `manifold.hpp:309 / 312` | correct |
| `TrailEntry == 40` | `arena_manifold.hpp:154` | correct |
| vprolog opcodes Unify/Choice/Fail/Cut | `vortex_vm.cpp:789/878/887/899` | correct |
| choiceStack push_back / cutBarrier=size() / .back() | `vortex_vm.cpp:169 / 174 / 180` | correct |
| `render::WorkerPool` class | `worker_pool.hpp:36` | correct |
| vql / vpl `spawnCursor(entryOp, "main")` | `vql/compiler.cpp:866` / `vpl/compiler.cpp:1121` | correct |
| ephemeral refusal in setLink/applyStructure | `store.cpp:757/807/891-892/928` (`isEphemeral`) | correct |
| `VortexHostConfig::fromStore` reads cycle_budget/gc_interval_frames | `vortex_host.cpp:35/42/49` | correct |
| `bindManifold(const Manifold*)` exists | `vortex_host.hpp:65` / `vortex_host.cpp:110` | correct |
| Makefile SANITIZE_ADDR_OPTS / SANITIZE_THR_OPTS under `ifdef DEBUG` | `Makefile:435/436/438` | correct |
| sanitize/address & sanitize/thread depend only on `gleditor`; `/run` runs `$(OBJDIR)/gleditor` | `Makefile:848/855/851/858` | correct |
| `gleditor` links only `$(GLEDITOR_OBJS) $(LIBLINK)` (no Vortex) | `Makefile:823` | correct |
| `TSAN_OPTIONS=second_deadlock_stack=1` on stock target | `Makefile:858` | correct |

## Soundness of the recommendation

The core recommendation — reject a coarse global lock (it is the GIL), reject fine-grained arena
locking (whole-arena `mark()`/`release()` truncation + dense-vector reallocation defeat it), reject
lock-free (no undo story); adopt per-worker share-nothing `ArenaManifold`s over one immutable base
`Manifold`, M:N scheduler as a sibling of `render::WorkerPool`, per-worker
`choiceStack_`/`cursorPC_`/mark-release, single serialized `promote()` merge, determinism by
submission index — is technically sound and, crucially, correctly diagnoses the GIL as an artifact
of ONE shared mutable arena rather than of the green-thread model. The three load-bearing structural
facts (CoW overlay over immutable base; truncation-undo that is coherent only with a single writer;
`ephemeralBit` + `promote()` as the enforced isolation/commit boundary) are all real and are the
right pillars for the argument. The prior-art survey (CPython GIL/PEP 684/703, Ractors, BEAM, Go,
Aurora/Muse) maps correctly onto the arena's shape, and the WAM precedent is the right lens for the
global `choiceStack_` obstacle.

## Internal consistency

No contradictions found. The one genuine inter-expert divergence (eagerness about OR-parallel
vprolog) is explicitly flagged in §7 and reconciled by making vprolog parallelism the last stage,
opt-in, default-off, gated on logic-equivalence tests under TSan — the cautious reading governs the
plan. The sanitizer story is now consistent end to end (exec summary / §3.6 / §5.5 / §8 all agree
the stock targets cover only `gleditor` and a NEW engine-and-test Make target is required); the
earlier loop-2 inconsistency ("run the existing suite under make sanitize/address") is gone.

## Frontend thread-primitive exposure

Concrete and plausible for all four surfaces:

- stdlib: parallel-for over a rank with deterministic merge (the shared primitive). Grounded in the
  real `map`/`filter`/`fold`/`call` surfaces.
- VPL: implicit data-parallel array verbs over disjoint chunks past `vortex.parallel_threshold`;
  no new syntax, force-serial opt-out. Correct fit for an APL/J language.
- VQL: declarative query parallelism (partition scans/join probe side), `spawnThread` bool
  honestly repurposed, deterministic result set/ordering contract. Sound.
- vprolog: opt-in OR-parallel search, per-worker binding store spelled copy-on-write, deterministic
  first-solution by submission index, cut confined to its branch, AND-parallel deferred. Correctly
  identified as the hardest and gated last.

## AGENTS.md constraints

- No CMake: the new sanitizer build target is explicitly specified as a Make target, "no CMake"
  (study :296, :549). Respected.
- `sizeof(CompactOpNode) == 64`, 64 KiB Merkle pieces, append-only: repeatedly asserted untouched;
  only *which thread* calls the single existing writer changes. Respected.
- Config in system xanadocs: every tunable routed through `defaultSettingSpecs()`/`getSetting` in
  `system://settings`, joining the already-read `vortex.cycle_budget`/`vortex.gc_interval_frames`.
  Respected.
- "Justify why not Vortex": the worker pool, scheduler and sanitizer target are OS-thread / build
  infrastructure (raw-OS and allocator-adjacent), which is the sanctioned exception. The
  language-visible unit stays the green cursor. Respected.
- No new C++ dependency: explicitly a non-goal (rules out vendored RCU/hazard-pointer libs).
  Respected.

## Format gates

- `mdformat --check --wrap 100` with gfm/dollarmath/frontmatter plugins: exit 0 (clean).
- `mdl -c .mdlrc`: exit 0 (clean).
- Only >100-col lines are inside the two markdown tables (§2.1 survey, §6.5 summary), which MD013
  exempts via `.mdlrc`; mdl confirms clean. The one fenced `text` component diagram is fence-exempt.

## Findings

1. [MINOR] §4.1 asserts a node "owns ... its choice-point `floors_`." `floors_` is a member of
   `ArenaManifold` (`arena_manifold.hpp:696`), not of the VM's `choiceStack_`; the sentence conflates
   the arena's nested-mark floor stack with the VM choice stack. The claim is still true (the arena,
   and thus its `floors_`, is per-worker), but the phrasing invites a reader to think `floors_` is
   choice-point state in `VortexVM`. Fix: reword to "owns its dense vectors and its arena-local
   nested-mark `floors_`, plus its VM's `choiceStack_`" (or similar) so the two stacks are not merged.
   Non-blocking; the architecture is unaffected.

2. [MINOR] §1 lists `splice()` among the "every structural primitive funnels into it" set in prose
   earlier (abstract/§1 narrative) but the enumerated file:line list in §1 omits `splice()` while the
   briefing names it. Not an error (the list is explicitly a sample, "Every structural primitive
   funnels into it: link() ... value() ... newCell() ... breakLink() ... pin() ... collectGarbage()
   ..."), but adding `splice()` with its `vortex_core.cpp` line would make the enumeration match the
   briefing's primitive list exactly. Optional.

No [BLOCKER] and no [MAJOR] findings. All previously raised citation and sanitizer-plan issues from
loops 1–3 are confirmed fixed in the current text.

## Verdict

The report is sound: every spot-checked citation is accurate to the line, the threading
recommendation is technically correct and correctly reasoned from real structural facts, there are
no internal contradictions, each frontend's primitive exposure is concrete, all AGENTS.md
constraints are respected, and both format gates pass clean. Only two MINOR polish items remain,
neither of which blocks acceptance.

LGTM
