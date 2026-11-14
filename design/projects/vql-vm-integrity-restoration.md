# VQL runtime graph integrity restoration

The integration baseline at `6631bde9` restored several language cards but still performed topology
selection or mutation during compilation, quantified compiler result slots instead of runtime
members, and retained placeholder cursor results. This stage repairs those paths together with
mutation-index and arity validation. It does not complete §4.12 or the wider VM restoration plan.

## Resulting behavior

- Dimension morphisms emit runtime allocation, value and link instructions inside mutation groups.
  Compilation leaves user topology unchanged. Each execution creates fresh literal targets; empty
  computed targets leave the current chain tail intact so later targets can run. Existing cell
  targets retain their identity. Exported graphs execute after the AST and source VM are destroyed.
- Cursor selection traverses runtime ranks and reads `d.name` aliases. Active and pinned rank order
  is preserved. Read-only dimension perspectives traverse the existing qualified axis at runtime,
  apply target predicates and clamps, and use canonical fallback when that result is empty. This
  does not implement a general perspective clone-map ABI. Root metacell enumeration refuses until a
  graph-visible root registry exists.
- Quantifiers iterate runtime domain occurrences and short-circuit later predicate execution.
  Cardinality and identity checks inspect runtime streams. Predicate lambdas share the ordinary
  compiled-call contract/destructuring binder. Path results use presence; scalar results use value
  truthiness. Unsupported block, multi-formal and variadic predicate lambdas refuse explicitly.
- Invalid or out-of-range mutation indices refuse before writes. Positive range lengths clamp before
  endpoint addition, including `INT64_MAX`. Excess `value` and fixed-function arity refuses before
  argument effects. Valid explicit arguments still prepare in caller scope before parameter
  matching.

The four §4.12.2 definitions now run verbatim through the real CLI, producing `95`,
`[note] Example`, `av1 @ 60fps` and `archived` with the small test fixtures. The archive example
retains required fields with `*`; its original exclusive body remains an atomic-refusal regression.
The owner's `@now` decision is documented as a UTC wall-clock sample during execution.

## Validation boundaries

The baseline ran 4,346 engine tests: 4,272 passed, 46 failed and 28 skipped; 12 more were disabled.
The first complete post-repair gate ran 4,370: 4,304 passed, 38 failed and 28 skipped. Its failing
names were a strict subset of the baseline, with eight existing VQL failures fixed. Independently
reviewed missing-target, lambda-contract and maximal-length cases then extended the regression
matrix.

The final full build passed with all writers frozen. The engine gate ran 4,376 tests across 435
suites: 4,310 passed, 38 failed and 28 skipped, with the same 12 disabled profiling/benchmark
probes. No failing name was new relative to the baseline. All 30 added tests passed. The final
affected matrix ran 859 VQL/mutation-group tests, with 26 existing failures and no new failures. All
four real CLI examples passed. This remains a partial restoration, not a full-suite pass.

Validation logs are `/tmp/vql-vm-integrity-build.log`, `/tmp/vql-vm-integrity-final-full.{log,xml}`,
`/tmp/vql-vm-integrity-final-vql.{log,xml}` and `/tmp/vql-vm-integrity-cli.log`. Exact failing names
are retained in `/tmp/vql-vm-integrity-final-failed.txt`.

The unchanged library baseline is 841 passes and one failure from 842 tests. The slice/UI baseline
is 266 passes and two failures from 268 tests. Repository lint reports two existing UI typography
policy violations. These are recorded failures, not passing gates.

Existing tests changed only at restored boundaries: old perspective/morphism refusal assertions now
check actual results and unchanged canonical topology; slice cursor fixtures use `d.name` and assert
the documented rank stream. The argument-ordering test retains its check that later actuals run
before parameter matching, using a valid two-formal call. Excess-arity calls have separate
before-effects coverage. No test was disabled or deleted.

Independent Luna architecture, senior and resource reviews cover graph ordering, shared contracts,
rollback, reference lifetime and portability. The opcode-size probe compares runtime ranks of one
and 1,000 members. It establishes constant generated opcode count for that query, not constant
memory or a speedup: ranks materialize streams and traversal state grows with rank length. Fresh
morphism cells accumulate by design. C6 lifecycle work, C9 measurements and the remaining callable
and route packages stay open in [the completion gates](vql-412-completion-gates.md).
