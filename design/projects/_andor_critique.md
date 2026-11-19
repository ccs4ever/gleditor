# Critique panel findings — `design/vicon-andor-parallelism.md`

Reviewed jointly as concurrency researcher + interpreter/parallel-runtime expert + zigzag/vortex
expert + C++23/26 expert + system architect. Every finding was checked against the real code in the
`study/vortex-multithreading` worktree, `_andor_briefing.md`, and `vortex-multithreading-study.md`.

## Verification performed (tool-backed)

Code bodies read, not trusted:

- `apps/common/xanadu/zigzag/vlog.hpp` — `deref` = `m.cloneMaster(ref, clone).value_or(ref)` at
  **L81**; `bind` doc "splice v's clone rank onto the posward tail of t's" matches the body.
- `apps/common/xanadu/zigzag/vlog.cpp` — `Vlog::bind` opens **L85**; the splice
  `m.link(endOfRank(t, clone, POS), clone, POS, endOfRank(v, clone, NEG))` is at **L91**;
  "variablehood is membership of a rank, not a flag bit" comment at **L25**; `makeVar` mints a bare
  cell onto `d.vars`; `unify()` returns `std::expected<void, UnifyFailure>` and leaves partial
  bindings on failure.
- `apps/common/xanadu/vortex/vortex_vm.cpp` — `pushChoicePoint` **L163**;
  `.mark = core_.arena().mark()` **L170**; `.cutBarrier = choiceStack_.size()` **L174**; `backtrack`
  with `core_.arena().release(cp.mark)` **L181**; `cut()`
  `while (choiceStack_.size() > cutBarrier) { … discard(cp.mark); }` **L197–L203**. Opcode handlers
  `Unify` **L789**, `Choice` **L878**, `Fail` **L887**, `Cut` **L899** — exact.
- `apps/common/xanadu/vortex/vortex_vm.hpp` —
  `struct ChoicePoint { cursor, altOp, mark, stackFrame, cutBarrier }` **L86–L92**;
  `std::vector<ChoicePoint> choiceStack_` **L151** — exact.
- `apps/common/xanadu/zigzag/arena_manifold.hpp` — `Mark` has **12** `uint32_t` fields (cellCount,
  linkSize, contentSize, scratchSize, trailSize, liveLinks, liveContent, shadowCount, proxyCount,
  quoteOccurrenceCount, proxyShadowedEdgeCount, quoteSpaceCount) — the doc's "twelve `uint32_t`
  high-water lengths" is exact. `shadow()`, `shadowOrder_`, `overlay_`, `Mark::shadowCount`,
  `TrailEntry` (`sizeof==40`), `trail()`, `mark/release/discard/compact/promote`, `ArenaRefusal`,
  `ephemeralBit`, "promote maps refs," and the R8 refusal via `Store::setLink`/`applyStructure` all
  present and correctly described. CoW overlay semantics (reads fall through, first write shadows
  the whole cell keeping the base's CellRef) match the class comment.
- `include/gleditor/cpp26*.hpp` — facade covers exactly `function_ref`, `optional`
  (+optional-as-range), `inplace_vector`, `span_at`, two-span `views::concat`. **No `stop_token`.**
  The doc's enumeration and its "not the cpp26 facade" claim are accurate.
- `vortex-multithreading-study.md` — §3.2 (cursor pinned, no running-cursor migration), §3.3 (shared
  frozen base, serialized `promote` on one thread, determinism by submission index at the merge),
  §3.6 (the non-stock engine TSan target caveat), §6.4 (vprolog Stage-5 opt-in, default off,
  equivalence under TSan, independent-AND-later), §6.5 (per-worker arena/choiceStack/cutBarrier
  table). All consistent with the doc.

Gates re-run independently: `mdformat --check --wrap 100` (gfm/dollarmath/frontmatter) → exit 0;
`mdl -c .mdlrc` → exit 0.

## Technical soundness verdict

- **Multiple-binding dissolution is correct.** A binding is one `d.clone` splice consuming v's
  single slot (`vlog.cpp:91`); two branches sharing one arena collide on that slot; a per-branch CoW
  arena gives each its own shadow of v with its own slot. The §1.2 argument is faithful to the code
  and to the `Mark`/overlay model. The identification "arena CoW = lazy, cell-granular Muse
  environment copying" is sound and strictly fewer copies than eager Muse.
- **Cut-as-cancellation is correct.** Serial `cut` = `discard(cp.mark)` down to `cutBarrier` (keeps
  bindings, drops retry) — the doc's §3.1/§3.2 "discard not release" is exact. The structural `d.or`
  prune + `std::stop_token` for stolen subtrees is a coherent cross-worker lift; the cooperative-
  cancellation honesty limit (§3.3) is correctly stated, including the unbounded-generator step
  counter.
- **Independent-AND condition is stated correctly.** "Independent iff the `shadow()` write-sets are
  disjoint," made observable via `shadowOrder_`/`Mark::shadowCount`, is a sound sharpening of
  CGE/determinacy analysis; dependent-AND is correctly held out as provably out of reach under
  share-nothing, matching study §6.4.
- **Determinism by submission index** matches study §3.3/§3.5: merge in index order on the single
  promote thread; first-solution = lowest index regardless of wall-clock.
- **Cell/dimension honesty** respects `ephemeralBit`/`promote`:
  `d.or/d.branch/d.frontier/ d.submission` are explicitly ephemeral, host-arena-owned, never
  promoted; "promote the answer, not the search" is enforced by the reachable-subgraph contract of
  `promote()`. Honest.
- **C++ availability honesty** is sound: C++20 `stop_token`/`barrier`/atomics native, C++23
  `expected`/`move_only_function` native, cpp26 facade for `function_ref`/`inplace_vector`, P2300
  flagged aspirational and explicitly not written against or macro-probed (respects AGENTS.md's
  no-`__cpp_lib_*` rule). Settings via `defaultSettingSpecs()`, no magic numbers.

## Numbered findings

1. **[MINOR] §1.4 attributes "not thread-safe … single-owner" to the arena class comment, which does
   not say it.** The doc: "the arena is not thread-safe by design (`arena_manifold.hpp` class
   comment)." A `grep` of the header for `thread|single-owner|not thread` returns **no matches**.
   The property is true (dense vectors, no atomics, no synchronization primitives; the briefing
   lists it as a derived description) but it is not a quotable statement in that comment. *Fix:*
   change the parenthetical to cite the real evidence — e.g. "(dense vectors with no atomics or
   locks; the arena carries no synchronization — `arena_manifold.hpp`)" — or attribute it to the
   briefing/study §3.2 rather than to the class comment.

1. **[MINOR] §1.2 line citations are off by a few lines (point inside the right function).** The doc
   cites the `bind` splice as `vlog.cpp:86` and `deref` as `vlog.hpp:80`. Actuals: `Vlog::bind`
   opens at **L85** and its splice `m.link(...)` is at **L91**; `deref`/`cloneMaster` is at **L81**.
   Likewise §1.3/§2.2 cite `vlog.cpp:23` for the variablehood comment, actually **L25**. All land in
   the correct function and the semantics are described correctly, so this is cosmetic drift, but a
   `file:line` doc should be precise. *Fix:* bump to `vlog.cpp:91` (or `:85` for the function),
   `vlog.hpp:81`, `vlog.cpp:25`. (The VM citations `vortex_vm.cpp:174`/`choiceStack_` hpp:151/opcode
   lines 789/878/887/899 are already exact; `:164`/`:172`/`:178` round to the real 163/170/181 and
   are substantively right but could be tightened in the same pass.)

1. **[MINOR] §1.3 cites "`#Choice` … (`vortex_vm.cpp:164`)" where 164 is inside `pushChoicePoint`,
   not the `Choice` opcode handler.** The `Choice` opcode case is at **L878**; L163–L176 is
   `pushChoicePoint`. The intent (the push performed when a choice is reached) is correct, but a
   reader chasing "the `#Choice`" would expect L878. *Fix:* either cite `pushChoicePoint`
   (`vortex_vm.cpp:163`) by name, or cite the handler at `:878` and the push at `:163` separately.

No [BLOCKER] or [MAJOR] findings. No invented APIs; no misdescribed `bind`/`deref`/`mark` semantics;
no C++ facility claimed present that the toolchain or cpp26 polyfill lacks; AGENTS.md constraints
(no sentinels in the API-level design, settings not magic numbers, no-`__cpp_lib_*`, single promote
writer, on-disk invariants untouched) respected; mdformat/mdl clean.

LGTM — all three findings are cosmetic citation precision; the mechanism, cut, independent-AND
condition, determinism, cell/dimension honesty, and C++ grounding are all code-accurate and
consistent with the general study.
