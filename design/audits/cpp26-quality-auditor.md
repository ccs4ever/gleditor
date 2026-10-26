# C++26 and functional quality audit

Scope: read-only review of `src/`, `include/`, and `apps/` in the `cpp26-functional-audit` worktree
at `origin/main` `126be39e`. No production code was changed. The compatibility facade and its
documented constraints were reviewed in `design/cpp26-compatibility.md` and `AGENTS.md`.

## Summary

The existing C++26 facade is already applied where the repository has documented use cases. The
clearest additional opportunities are three `std::function` callback parameters whose
implementations invoke them inline and do not retain them. They can be evaluated as
`gleditor::cpp26::function_ref` candidates, subject to checking all call sites and measuring
allocation benefit before changing APIs.

No additional `inplace_vector`, optional-reference, optional-range, span-concat, or checked-span
migration is strong enough to recommend from this pass. The visible dynamic collections either have
no enforced maximum or represent genuinely owned results. Avoid turning an observed size into a
fabricated bound.

## Facade inventory

- `function_ref` is used in the render worker pool, Vulkan GL interop callback, Vortex array
  functions, store/view callbacks, ZigZag projection lenses, traversal visitors, and other
  synchronous interfaces. The compatibility note records the zero-allocation worker-pool case and
  the API rule not to retain borrowed callbacks.
- `optional<T&>` already appears across page, cell, node, value, scroll, settings, link, and store
  lookups. The compatibility note explicitly documents borrowed lifetime requirements for the
  `findCell` reference.
- Optional range support is available through the shared optional facade;
  `apps/common/xanadu/multi_store.cpp:309` uses `.or_else` on an optional reference.
- `inplace_vector` is currently limited to the two 32-slot staging buffers in
  `src/ui/segmented_pill.cpp:29-38,81-86`. It has a dynamic vector path above the inline threshold,
  so it does not impose a product limit.
- `span_at` is used for GIF byte reads in `src/decode_index.cpp:1369-1439`, following the decoder's
  explicit size checks.
- Two-span concat is used in `apps/common/xanadu/ops.cpp:157` and
  `apps/common/xanadu/vql/vql_engine.cpp:31`; each consumer has the described two-const-span
  lifetime and materialization behavior.

## Prioritized candidates

### P1 — `CommandMailbox::submitTransaction` callback

Evidence: the declaration takes `const std::function<CommandResult(std::string_view)>&` at
`apps/common/xanadu/zigzag/command_transaction.hpp:46-48`. Its implementation only checks and
invokes the handler at `apps/common/xanadu/zigzag/command_transaction.cpp:34-38`; the API comment
says it invokes the handler synchronously at lines 41-42. No callback is stored in `CommandMailbox`
or the promise cell.

Candidate: replace this parameter with
`gleditor::cpp26::function_ref<CommandResult(std::string_view)>` if the call-site audit confirms all
callers invoke `submitTransaction` with live lvalue or temporary callables for the duration of the
call. This makes the non-owning lifetime intent explicit and can avoid constructing an owning
type-erasure wrapper. Preserve an empty-callback behavior only if the callers currently depend on
passing an empty `std::function`; the present implementation does.

Risk: low to medium. The synchronous lifetime is clear, but an empty `function_ref` may not have the
same contract as an empty `std::function`, and this is a public header in the app engine. Check the
facade's callable and nullability semantics before migration. No callback is stored.

Validation: enumerate every call site; compile the relevant target in native and
`GLEDITOR_CPP26_FORCE_FALLBACK=1` modes; add/adjust focused tests for a stateful lvalue callback,
temporary lambda, and empty callback if emptiness remains supported. Compare allocations only if
this call path is frequent enough to justify a performance claim.

### P1 — Transductive view predicate/comparator

Evidence: `materializeFilter` takes `const std::function<bool(CellRef)>&` at
`apps/common/xanadu/view/transductive_view.hpp:69-73`; implementation invokes it during the rank
walk at `apps/common/xanadu/view/transductive_view.cpp:59-65`. `materializeSorted` takes
`const std::function<bool(CellRef, CellRef)>&` at header lines 77-81 and passes it to
`std::stable_sort` at implementation line 122 (the comparator is used before the function returns).
Neither callback is retained.

Candidate: use `function_ref` for both if call-site review finds no empty `std::function` values and
the stable-sort comparator satisfies the callable contract. The filter's synchronous walk is a
particularly straightforward borrowed callback. This aligns the API with the actual non-owning
lifetime.

Risk: medium. `stable_sort` can copy its comparator internally; this is acceptable only if the
`function_ref` wrapper is copyable as expected and stays within the caller lifetime. Comparator
strict-weak-ordering is already required independently. Do not blindly convert a callback that
callers intentionally leave empty.

Validation: inspect all callers; cover filtering and sorting with captured state and empty-result
paths; run the view tests and both native/fallback builds. Verify no callback escapes into
`ViewManifold` or a deferred job.

### P2 — Magnet parameter visitor

Evidence: `forEachMagnetParameter` takes `const std::function<void(std::string_view, std::string)>&`
at `apps/common/xanadu/torrent.hpp:303-305`. The implementation loops over URI fields and calls
`visit(...)` directly at `apps/common/xanadu/torrent.cpp:468-486`; it stores no callback. The
decoded value is passed by value, so it owns its temporary string across the synchronous invocation.

Candidate: change to `function_ref<void(std::string_view, std::string)>` if call sites confirm a
callback does not need ownership after invocation. This is an explicit, easy-to-understand borrowed
visitor API.

Risk: low to medium. Verify zero-argument/no-op use cases; the current function does not test
callback emptiness and would throw `std::bad_function_call` for an empty `std::function`, so the
present API does not promise optionality. Public API compatibility still needs consideration within
the app.

Validation: compile all call sites, test repeated parameters and captured lvalue/temporary
callbacks, and run existing magnet/BEP46 parsing tests in native and forced-fallback builds.

## Rejected or deferred candidates

- **Vortex logic solver callback:** `VortexStdLib::solve` accepts `std::function` by value and its
  single-goal overload explicitly moves it into the span overload
  (`apps/common/xanadu/vortex/vortex_stdlib.cpp:4288-4295`; declarations at
  `vortex_stdlib.hpp:287-294`). That signals a consuming/owning boundary and differs from the safe
  borrowed-parameter cases above. Do not convert without tracing the complete downstream call chain
  and confirming no storage/copy contract.
- **Other `std::function` members and setters:** state, UI handlers, jobs, deferred dispatch, worker
  payloads, and transport factories are stored, moved, or invoked later. `function_ref` would create
  dangling-lifetime hazards. In particular, `mesh_host.cpp`'s execute/cancel closures and the
  callbacks in `include/gleditor/state.hpp` are persistent state, not borrowed call parameters.
- **More `inplace_vector` substitutions:** candidate vectors in parsers, query results, decoded
  media, cell traversal, and UI models are driven by input size or lack a proven hard bound.
  `inplace_vector` is appropriate only with a justified bound and overflow strategy. The current
  segmented-pill hybrid is a good model; do not cap the user-visible segment count.
- **More `optional<T&>` conversions:** the existing 79 matches in `include/`, `src/`, and
  `apps/common/` already cover many borrowed lookup APIs. Return types like `ImageResource`,
  `CommandResult`, `CellRef`, and page query results intentionally own values or cross mutation
  boundaries; changing them to references could couple callers to cache/container invalidation.
- **More two-span concat:** the existing occurrences use exactly two same-type spans. Most other
  collections are heterogeneous or owned vectors; adding a view solely to combine them risks
  obscuring lifetime and allocation behavior.
- **More `span_at`:** current accesses are at binary parsing boundaries. Direct indexes elsewhere
  are generally guarded by loop bounds or use containers whose access contract differs from span;
  audit each parsing boundary before broadening, rather than replacing safe indexing mechanically.

## Functional style and readability

The most useful functional-style improvement is the callback ownership correction above: a borrowed
synchronous callback should have a borrowed type. This documents lifetime in the signature and
avoids making callers pay for ownership that the callee never uses.

Do not convert ordinary loops to ranges for style alone. Existing uses such as `std::ranges::any_of`
in `apps/common/xanadu/ops.cpp` already express a simple query clearly. Likewise, do not introduce
generic higher-order helpers for one-off traversals. For any future optional-range adoption, use it
where a zero-or-one reference is already being iterated and it removes branching without hiding the
absence case; the existing optional-reference APIs should not be mechanically rewritten.

## Work and validation status

This report is an audit artifact only. No code was changed, and no build or tests were run. The
validation steps above are the meaningful checks to perform if any candidate is approved for
implementation; this audit does not claim runtime or allocation improvements without those checks.
