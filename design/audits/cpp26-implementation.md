# Applying the C++26 and functional audit

Implementation branch: `audit/cpp26-functional-chainability`, based on fetched main
`126be39e8e4936133ade433ddcc4c0f40e2678ec`. The original checkout's changes remain separate. The
[audit](cpp26-functional-audit.md) and [research](cpp26-library-research.md) record the baseline
findings and dependency decisions. Three Luna agents implemented separate tracks, then
cross-reviewed the callback and helper contracts; the parent integrated and validated the result.

## Applied changes

- `nthOf` now accepts unsized random-access ranges through its iterator walk. Its sized fast path
  uses iterator arithmetic, since the range object need not supply `operator[]`.
- `stepped_path_view` advertises input iteration, matching its iterator-local value and reference
  lifetime. It preserves the existing traversal and cycle/limit behavior. Tests assert the honest
  concept instead of manufacturing forward semantics.
- Generic facade tests moved from `tests/xuzz` to `tests/lib`, without duplication. Direct tests
  cover optional-reference identity, mutation, rebinding, reset and zero/one range iteration.
- Transductive filter/sort, magnet parameter traversal and mesh request serving take synchronous
  borrowed `function_ref` callbacks. Required-callback and lifetime comments make the contract
  explicit. Captured-state and repeated-key integration tests cover the migrated call paths.
- `Doc`, `ScreenOverlay`, `VPLEngine`, `VplView`, `LinkForgeWidget`, `LinkPanelOverlay` and
  `VortexHost` now return their receiver from the audited property/configuration setters. Existing
  noexcept, locking, dirty/revision and retained-callback ownership behavior is preserved. Two VPL
  setup blocks use the resulting chains.
- The compatibility note describes the current two 32-slot segmented-pill buffers and their heap
  fallback, separating historical tether allocation measurements from current implementation.
- Full fallback compilation uncovered mixed empty tags in GIF frame accessors and a deduced ZigZag
  clone-master lookup. Those branches now use the facade's `nullopt`, so Beman optional references
  compile as well as their native aliases. Existing GIF and core lookup tests cover absence and
  borrowed lookup behavior.
- Fallback CI now supplies `XDG_CACHE_HOME` alongside the other headless isolation variables. Its
  whole-library suite automatically includes the relocated tests.

The full build also exposed an existing `store-dump` compile failure: `scrollKey` returns an
optional string and the tool streamed it directly. The narrow repair unwraps a valid key and refuses
a keyless deployment with a contextual `StoreTablesUnreadable` error. An existing tool suite gained
a regression for the refusal, with no fabricated key or partial deployment row.

## Deliberate boundaries

`CommandMailbox` retains its nullable owning callback contract and “Unhandled action” behavior.
`Drawable::setModel` is unchanged because a base pointer would lose derived-type chaining. Stored
callbacks remain owning, and Vortex solver ownership was not redesigned. The generic stepped view's
pre-existing exception specifications and element-type requirements were not broadened.

Fluent setters remain unqualified, consistent with established project APIs and existing source
usage. Their result is a borrowed pointer: immediate chains on a temporary last through the full
expression, but a retained pointer does not extend the receiver's lifetime. Public setter signature
changes require rebuilding consumers; matching symbol names do not establish binary compatibility.

No additional library, arbitrary inline-capacity limit, persistent-format change or speculative
performance claim was introduced. Hive, richer ranges, numerical libraries and senders/receivers
remain conditional/deferred for the reasons documented in the research.

## Validation

Both native and forced-fallback full builds passed, using GNU Make's compiler/dialect probes.
Fallback outputs are isolated with `OBJDIR=build/cpp26-fallback`, so native validation binaries
remain intact. The fallback build uses `GLEDITOR_CPP26_FORCE_FALLBACK=1`; its library-linked suite
resolves the shared library from that output directory, confirmed with `ldd`.

Native validation:

- All 830 library tests and all 256 ZigZag tests passed.
- Xuzz: 2,240 passed, 10 environment-dependent skips, and one scaled-screenshot assertion failed in
  the first full run. That single reader-drag case passed with a fresh isolated profile and in each
  of the three backend orchestration runs; no speculative rendering change was made.
- After the tag repairs, all 18 focused facade/range/GIF tests and all six core lookup tests passed.
- The complete backend comparison passed on OpenGL, OpenGL ES and Vulkan, including all 37
  orchestration scenarios per backend. An initial five-minute wrapper timed out; the complete run
  used a sufficient limit and passed.
- The first rootless network phase passed all 12 swarm/mutable-name tests.

Fallback validation:

- All 830 library tests and all 256 ZigZag tests passed.
- The Xuzz run excluded the 37 binary-orchestration scenarios already covered by native/backend
  validation: 2,203 passed, 10 skipped, and one existing microbenchmark timing threshold failed. Its
  lookup correctness assertions passed; the measured speedup was 4.06x against a 5x threshold.
- Three fixed alternating native/fallback reruns preserved all results. Native speedups were 4.38x,
  4.67x and 6.05x (two threshold failures); fallback was 5.54x, 7.17x and 6.99x (all passed). This
  demonstrates threshold variability in both modes, not a demonstrated fallback regression. The
  unrelated algorithm and benchmark threshold were left unchanged.

Extended native network validation passed all five tests: four publication-outbox scenarios and
checkpoint recovery across publisher restart, partition and reactivation. Together with the first
phase, all 17 network tests passed.

Quality gates:

- Full `make format-check` passed with clang-format 19; changed Markdown lint, shellcheck, yamllint
  and all seven policy-checker unit tests passed.
- Full `make lint` stops on two typography-policy findings in unchanged slice-presenter headers.
  Independent repository-wide Markdown lint also finds issues in 14 unchanged design files;
  comparison with `HEAD` confirmed those files are unchanged. No new lint finding was introduced.

No additional performance claim, production dependency or persistent-format change was made.
