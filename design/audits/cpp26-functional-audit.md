# C++26, functional composition and chainability audit

Completed 2026-10-26 from fetched `origin/main` at `126be39e8e4936133ade433ddcc4c0f40e2678ec`,
branch `audit/cpp26-functional-chainability`, worktree `.worktrees/cpp26-functional-audit`. The
existing main checkout and its unrelated edits were left untouched. Submodules were initialized.
This deliverable is an audit and implementation backlog; no production source, API, dependency pin,
or persistent format was changed.

The subsequent [implementation record](cpp26-implementation.md) tracks the applied changes and
validation. Findings below describe the original baseline; saved range probes now check the repairs.

## Outcome

The current compatibility layer is useful and broadly adopted. Broader use should follow concrete
ownership, lifetime and capacity requirements. Research did not establish an additional dependency
that merits immediate production adoption under the requested maturity constraint. PLF hive is the
best conditional new-container candidate. Functional programming is already materially represented
by ranges, monadic optional/expected operations and shared traversal helpers. Setter chainability
has several small, concrete opportunities. Two reproducible generic range-helper contract defects
should be corrected before expanding those helpers: `nthOf` assumes random access implies a sized
range, and `stepped_path_view` advertises forward iteration but only models input iteration.

Reports and evidence:

- [GitHub library research and readiness decisions](cpp26-library-research.md).
- [Luna quality auditor: facade adoption and functional opportunities](cpp26-quality-auditor.md).
- [Luna chainability audit](cpp26-chainability-auditor.md).
- [Luna systems profiler: compatibility, coverage and probes](cpp26-systems-auditor.md).
- [Standalone facade probe](probes/cpp26_facade_probe.cpp).

## Phases performed

1. **Baseline and state review.** Fetched main, created an isolated worktree, read repository and
   architecture instructions, inventoried the five facade groups and pinned dependencies, and
   initialized submodules.
1. **Upstream research.** Inspected primary GitHub sources, official documentation and maturity
   statements. Distinguished exact standard-oriented implementations from mature adjacent APIs,
   experimental implementations, compiler features and C++29 work.
1. **Concurrent Luna audit.** Three Luna agents independently examined code quality/adoption,
   architecture/chainability and systems/performance. Each first inventoried candidates, then
   inspected implementation/callers and recorded constraints.
1. **Cross-review and validation.** Challenged empty callback assumptions, inheritance return types,
   temporary lifetimes and performance claims; ran independent facade matrices and staged-header
   checks. Findings below take precedence over tentative candidate rankings in individual reports.
1. **Synthesis.** Prioritized small migrations with clear acceptance criteria and deferred broad
   container, ownership and concurrency changes pending evidence.

This was a targeted repository-wide search followed by deep review of selected surfaces, not an
exhaustive proof of every method or every optional lifetime.

## Prioritized backlog

| Priority    | Opportunity                                                  | Concrete scope                                                                                                                                                                                                                | Acceptance gate                                                                                                                                                                                                       |
| ----------- | ------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| P1          | Make generic facade parity tests live with the library, once | `tests/xuzz/cpp26_compat_test.cpp` currently contains generic callable/container/concat tests; move these to `tests/lib/` per AGENTS, without duplicating them. Add direct optional-reference and optional-range tests there. | Library-linked native/fallback tests cover borrowed mutation, empty/singleton iteration, reference identity and rebinding. Keep engine integration tests in xuzz.                                                     |
| P1          | Correct stale compatibility documentation                    | `design/cpp26-compatibility.md` describes a 17-point tether use; actual `inplace_vector` consumers are `src/ui/segmented_pill.cpp:29-38,81-86`, two 32-slot float buffers with heap fallback.                                 | Describe current consumers; retain historical allocation claims only with their original scope and provenance. Do not infer current UI latency from an older microprobe.                                              |
| P1          | Align VPL setters with existing fluent configuration         | `VPLEngine::setStore` and `VplView::setOrigin`, `setExtents`, `setRagged`, `enclose`, `setScalarPayload`.                                                                                                                     | Return receiver pointers, preserve existing noexcept and state invariants, keep standalone calls valid, rebuild callers. Existing `setDependencies`/`withConsoleEffects` establish the convention.                    |
| P1          | Chain library property setters                               | `Doc::setDocIndex`; `ScreenOverlay` property/handler setters.                                                                                                                                                                 | Preserve locking/invalidation, release locks before chained calls, settle lvalue/ref-qualification policy and rebuild consumers. Retained callback types remain owning.                                               |
| P2          | Borrow synchronous view and parsing callbacks                | `TransductiveView::materializeFilter/materializeSorted`, `forEachMagnetParameter`, `MeshLensChannel::servePeerRequests`.                                                                                                      | Trace every caller, prohibit escape, resolve empty-callback behavior, compile and exercise native/fallback paths. Comparator copies are safe only within the borrow duration; ordering requirements remain unchanged. |
| P2          | Extend fluent engine/UI setup consistently                   | Selected `LinkForgeWidget`, `LinkPanelOverlay`, and `VortexHost` setters in the chainability report.                                                                                                                          | Return only after existing side effects complete. Do not transform commands, meaningful results, virtual contracts or asynchronous operations mechanically.                                                           |
| P3          | Redesign optional command handlers deliberately              | `CommandMailbox::submitTransaction` checks handler truthiness and produces “Unhandled action” when absent.                                                                                                                    | Preserve absence explicitly, perhaps with a no-handler overload or optional borrowed callable. A mandatory `function_ref` replacement alone changes behavior. Downgraded from the initial quality candidate.          |
| P3          | Preserve derived type before fluent base setters             | `Drawable::setModel` is inherited by `Doc` and `Page`.                                                                                                                                                                        | A returned `Drawable*` cannot chain derived setters. Leave alone unless derived wrappers or a broader design has a real benefit; do not introduce CRTP/virtual machinery solely for this return value.                |
| Conditional | Add hive or richer numerical/concurrency facilities          | Research report's candidate matrix.                                                                                                                                                                                           | Real workload, production-readiness review at an exact pin, compiler/packaging support, native/fallback semantics, and repository benchmarks.                                                                         |

## Functional programming direction

Reuse `include/gleditor/ranges.hpp` (`findRef`, `firstOf`, `lastOf`, `nthOf`, `singleOf`, `refOf`)
and `include/gleditor/stepped_view.hpp` (`stepped_path_view`, `filter_present`) where they express a
real query or traversal. Existing consumers include `src/a11y/tree.cpp:55`,
`apps/common/xanadu/pouch_zone.cpp:183`, `tension_layout.cpp:39-45` and `multi_store.cpp:300-309`.
Avoid adding another functional utility library for operations already available here or in C++23.

Existing `and_then`/`transform`/`or_else` composition in font lookup, store replay, dimension lookup
and Vortex configuration is the model. Use `transform` for a pure successful-value mapping,
`and_then` for another fallible operation and `or_else` for a deliberate recovery policy. Preserve
typed errors and meaningful absence; an absent lookup is not automatically success. Convert only
when the resulting expression is easier to understand than the branch sequence.

A borrowed callback is appropriate for a higher-order synchronous API; owned callback storage is
appropriate for events, transports and deferred execution. Do not infer ownership requirements from
`std::move` alone: the Vortex solver moves callbacks into recursive helpers, so inspect that entire
chain before classifying its API. Its current truthiness/null checks also need preservation.

Do not replace mutation-heavy replay loops, parser refusal logic, cyclic traversal or short-circuit
control flow with a pipeline solely for style. Shared helper contracts must be validated before
expanding their use. An observed small vector is not a capacity bound; keep configurable counts
unbounded with an explicit heap path when appropriate.

## Reproduced functional helper defects

The systems reviewer independently compiled an unsized random-access subrange and confirmed it
models `random_access_range` while failing `sized_range`. Passing it to the publicly input-range
constrained `nthOf` fails at `ranges::size`. This is a template contract defect, not an observed
production runtime crash. Require both random access and size for the fast path, and otherwise
retain the iterator walk.

The stepped view declares a forward iterator tag but postfix increment returns `void`. Its current
concept is `input_range`, and a `forward_range` assertion fails. Returning a prior iterator is only
one part of a repair: verify equality and multipass behavior, or document and advertise input
semantics. These are priority 1 prerequisites for broader adoption in this plan (the systems report
rates their current impact P2).

The reproducers are preserved in [the probe directory](probes). At the audit baseline,
`cpp26_range_probe.cpp` intentionally failed compilation. It now checks the repaired `nthOf`
contract and the honestly advertised input category; `cpp26_range_assert.cpp` proves the
unsized-range properties, and `cpp26_iterator_concept_probe.cpp` checks the input-only category.
They are audit artifacts outside the application build.

## Validation actually performed

The preserved standalone probe checks synchronous lvalue and temporary callable invocation,
optional-reference mutation, zero/one optional iteration, concat ordering, checked-span exceptions
and contiguous fixed-capacity resize. Parent reran it with Clang 23.1.1 and GCC 16.2.1 in both C++23
and C++2c, under automatic selection and forced fallback: all eight combinations passed. Automatic
selection in C++23 must not be described as necessarily native. Both C++2c toolchains report all
five native selector macros; forced fallback reports all five as zero.

Reproduce from this worktree (a standalone probe, not new build logic):

```sh
for audit_cxx in clang++ g++; do
  for audit_dialect in c++23 c++2c; do
    for audit_mode in automatic fallback; do
      audit_define=
      if [ "$audit_mode" = fallback ]; then
        audit_define=-DGLEDITOR_CPP26_FORCE_FALLBACK=1
      fi
      "$audit_cxx" -std="$audit_dialect" -Iinclude \
        -isystem thirdparty/nontype_functional/include \
        -isystem thirdparty/beman_optional/include \
        -isystem thirdparty/beman_inplace_vector/include \
        $audit_define design/audits/probes/cpp26_facade_probe.cpp \
        -o /tmp/cpp26-parent-probe && /tmp/cpp26-parent-probe || exit 1
    done
  done
done
```

Clang/native libstdc++ emits an upstream deprecated-trait warning from `inplace_vector`; this is
already suppressed by the normal Makefile. There was no facade compile/runtime failure.

Parent also staged the public headers and fallback trees under `/tmp/cpp26-install-audit-kbng6thg`,
generated native and fallback configs using the installation selector preprocessing step,
compiled/ran the probe using only the staged include roots with both compilers, and checked the
installed selections survive a consumer force-fallback define. All four staged combinations passed.
The initial harness compared macro listing order across compilers; sorting definitions corrected the
harness, with no product change. This checks headers/config, not library linkage, pkg-config
relocation, licenses, the whole `make install` recipe or an older consumer standard library.

No full application build, GoogleTest suite, GPU/backend comparison, benchmark, sanitizer run or
full install was performed. Production behavior did not change. Implementation phases must use the
normal GNU Make probes, `make -j$(nproc)` and the appropriate existing headless tests; public API
migrations require rebuilding every consumer and checking the library export boundary.
