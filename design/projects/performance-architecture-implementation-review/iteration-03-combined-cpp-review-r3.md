# M3 C applied-test review (r3)

Reviewed the three applied test files statically after the parent integrated the portability patch.
No production source was edited, and I did not compile or run tests.

## Findings

- The unordered helper tests no longer assert a fixed bucket threshold, `bucket_count >= size`, a
  fixed number of allocation calls per insertion, or a hard-coded three-call failure schedule.
  Entry-preservation tests exercise 512 inserts and lookups without encoding standard-library
  geometry. The reservation and staged-commit tests retain semantic checks: a previously reserved
  destination bucket count stays stable, and commit performs no PMR allocation after preparation.
- Failure-injection loops now derive their ordinals from a successful preparation or copy assignment
  using the same `FailingResource`. Each ordinal is then failed and checks that the operation
  refuses or throws while preserving the old contents. The two Manifold copy fixtures commit their
  dense and sparse tokens sequentially, so they do not accidentally invalidate the first prepared
  token before measuring.
- The `DenseGrowthFailurePreservesExistingLookupEntries` test exercises allocation failures while
  adding a second cell after a first mapping is live. It checks folded position, cell count, both
  old mapping/birth, absence of the new mapping, then successful retry and equivalence to a full
  fold.
- Removing `fillBirthTableToLimit` fixes the focused-test nontermination: geometric growth raises
  the bucket capacity before it can be filled by that loop. A fresh birth still enters the lookup
  preflight path; this test only needs to prove typed resource-failure propagation, while helper
  tests cover individual node/bucket allocation failures.

One cleanup: `manifold_test.cpp` repeats the same two-line comment immediately before
`resource.failAfter(0)` at lines 277–280. It is harmless but should appear once.

The prior r2 memory report has been corrected. The plan's 48-byte figure is per flat slot before
capacity slack, not a total-byte-per-live unordered-map cap; the staged birth map's 64.45 requested
bytes/live at one million entries compares favorably with the archived flat-birth measurements of
about 101.19 current and 151.78 peak. PMR allocator accounting is still not RSS or a universal
standard-library cost.

Runtime status remains with the parent pipeline. This review makes no integrated correctness or
performance-selection claim.
