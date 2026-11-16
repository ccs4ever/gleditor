# M3 flat map draft review

Reviewed `/tmp/perf-m3-flat-map.patch`, the isolated header/tests under
`/tmp/perf-m3-flat-map/repo`, `TRADEOFFS.md`, and §7 of
`design/performance-architecture-implementation-plan.md` (M3). No repository files were changed.

## Findings

1. **Mutable iteration is broken; the declared iterator category is also false.** In
   `flat_u32_map.hpp:117-120`, `operator*()` binds `const auto& slot`, so even
   `BasicIterator<false>` reads `const Value&` from `Slot::valuePointer() const`. The `EntryRef` for
   a mutable iterator requires `Value&`. An isolated compile with the Makefile-reported
   `/usr/bin/clang++ -std=c++2c` fails on ordinary `for (auto &[k, v] : map)` with “binding
   reference ... drops const qualifier.” A `static_assert(std::forward_iterator<Map::iterator>)`
   also fails: `EntryRef` and the advertised `pair<const uint32_t, Value>` have no required
   `common_reference`, so `indirectly_readable` is false. Fix mutable/const dereference separately
   and either implement the iterator concepts/proxy common-reference contract or advertise/test the
   weaker iterator category actually supported. The current tests do not exercise mutable structured
   bindings or concepts.

1. **M3 transaction boundary is still only a helper-level primitive.** The draft provides one-key
   `prepareInsert`/`commitInsert`; §7 requires `applyStructure` to prepare every required lookup
   allocation before any semantic mutation, then commit without allocation. The isolated patch does
   not include a dense/presence byRef adapter, typed `LookupCapacityUnavailable`, `advance`/Store
   propagation, or the `applyStructure` preflight/wiring. This is a valid isolated spike, but is not
   an M3 implementation or a pass of §7's failure-atomicity gate. In particular, the proposed
   tradeoff statement that two maps can be prepared separately needs a caller-level proof that no
   second preparation on the same map can rehash and invalidate an earlier token.

1. **The tests do not establish the promised failure guarantees.** Existing failure injection covers
   only the 8→16 growth allocation in `prepareInsert`. It does not cover failed same-capacity
   tombstone cleanup, copy construction/assignment failure, copy assignment preserving the target,
   allocation-free commit after both maps are prepared, failed multi-map preflight/retry, or actual
   Manifold logical state/fold counters. §7 explicitly calls for failure at each new lookup
   preparation allocation and retry equivalence against full rebuild. Add these at the
   adapter/Manifold layer before claiming the gate.

1. **Iteration/API assumptions need consumer proof before substitution.** `TRADEOFFS.md` correctly
   flags the proxy pair, but also describes range-for structured bindings as already suitable; the
   mutable form does not compile as written (finding 1). `operator==` exercises const iteration
   only. Audit actual `byRef` consumers and add compile coverage for each required pattern,
   including mutable and const traversal, `it->first/second`, and any algorithms/concepts used. The
   plan also calls for preserving public `byRef` access and auditing consumers before changing its
   type.

1. **Cost evidence is not present.** With the actual `StructureBirthInfo` (40 bytes on the probed
   target), the slot assertion caps `sizeof(Slot)` at 48 bytes; that is consistent with the plan's
   48-byte target slot, but it is only a compile-time upper bound, not a recorded ABI measurement.
   At the map's 2/3 load ceiling this is up to 72 bytes of allocated slots per live entry, before
   old-plus-new rehash peak. Record actual `sizeof(Slot)`, capacity/live ratio, allocation/growth
   peak, and initialization/page-fault/RSS data against the §7 pricing contract before choosing it
   over reserved unordered maps. No timings or integration/performance claims can be made from this
   draft.

## Checks run

- Asked the repository Makefile for its own toolchain probe:
  `/usr/bin/ccache /usr/bin/clang++ -std=c++2c`.
- Compiled a temporary syntax/concepts harness against the isolated header and actual `manifold.hpp`
  using that compiler/standard. It exposed the mutable iterator compile error and the failed
  `std::forward_iterator` assertion. No project build or test binary was run.

## Gate before integration

Fix and compile the iterator API; add growth/tombstone/copy failure tests; measure actual
slot/capacity/growth costs; then wire the helper only as part of §7's full M3 work: dense mapping
with bitmap and overflow-checked U-P2 threshold, all `applyStructure` insertions preflighted before
mutation, typed resource refusal propagated without semantic-refusal accounting, Store rebuild
refusing partial folds, and incremental/full-rebuild plus injected-failure/retry tests. Keep the
reserved unordered-map fallback until those correctness and measurement gates pass.
