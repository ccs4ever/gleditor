# I2B-2 independent ML gate: typed structural scope reads

Reviewed commit: `7a2dd07d6139b761ff687f0c4b0de5bcf5e20356`
(`Add typed structural reads to scoped evaluation (P19 I2B-2)`), based on I2B-1
`d4334ac39d1180e340ab323904bb8bada5c3d41f`. The I2B-2 source files in the current tree match that
commit; unrelated I3-1 changes are outside this review.

Scope: scoped-only typed `Point`, `Link` and `Deref`, cursor bindings, caller-pin reach provenance,
and result-reference export. This review does not approve compiler-emitted query lowering, candidate
eligibility/policy, analytic scoring, calibration, learning, migration, or native completion. Those
remain I3/I4/I5/I6 gates.

Gate result: **FAIL** — 0 BLOCKING, 1 MAJOR. A byte-budget bypass remains in the pin-reach native
representation.

## Findings

### M1: variable-length pin names are copied outside the byte grant

`ScopedFrame::reach` stores `std::string` root names for every reached `(CellRef, root)` pair
(`apps/common/xanadu/vortex/vortex_vm.hpp:238`). `stepScopedStructure` copies those names into
`rootsOf()`'s temporary vector and into new reach entries
(`apps/common/xanadu/vortex/vortex_scoped_evaluation.cpp:890–922`). Each pair is charged against
traversal work, but the copies do not debit `ScopedEvaluationBudget::bytesRemaining`. Parsing a
`pinned_root` string charges its bytes once; that does not bound repeated copies of that name across
many reached cells.

A descriptor can use a root label near the admitted byte limit, then walk many cells while staying
within the traversal-work grant. The reach multimap can consequently allocate many multiples of that
byte limit in native strings before work is exhausted. This defeats the shared byte budget for
native carriers and can turn a bounded scoped query into excessive memory allocation. The current
tests cover reach-count bounds and multi-root attribution, but not amplification by a long root
label.

Concrete fix: intern each declared root once in bounded frame storage and key reach entries by a
small root index (and use indices in temporary `rootsOf` results). Alternatively, debit the full
string length before every stored or temporary copy, including repeated traversal records. Add a
long-root, many-reached-cells test that proves aggregate allocation remains within the grant or
refuses before exceeding it. A traversal-count bound alone does not close this byte-allocation path.

## Verified I2B-2 behavior

- Structural opcodes are accepted in the scoped verifier only with the declared operand arities and
  binding types; their ordinary manifold-writing bodies are bypassed by `stepScopedStructure`
  (`vortex_scoped_evaluation.cpp:218–250, 564–568, 863–1048`). Cursor is binding-only and is refused
  as a schema field (`scoped_evaluation.cpp:330–337, 494–496`).
- `first`, `next`, `target`, and `.name` have distinct navigation semantics. Unknown unprefixed
  selectors refuse; missing neighbors produce a false found flag. Scalar `Deref` enforces the
  declared kind and rejects non-finite Float values. String reads charge content bytes, and string
  copies are debited before output allocation (`vortex_scoped_evaluation.cpp:948–1044`).
- Reach provenance records all roots and allows a result reference only under its declared
  `pinned_root`; duplicate-root traversal is handled as a multimap. Reach entries are charged as
  traversal work (`vortex_scoped_evaluation.cpp:890–922`). The root-copy byte gap above remains.
- Destination aliasing is covered through the whole old destination field graph.
  `publicationProtected` is built from the recursive destination snapshot and checked at
  cursor-reference publication and final payload capture
  (`vortex_scoped_evaluation.cpp:414–421, 954–958, 802–805, 1062–1069`). Tests cover old
  marker/name/value cells and positive unrelated caller cells
  (`tests/xuzz/vortex_scoped_evaluation_test.cpp:1970–2040`).
- Earlier I2A assignment, cleanup, shared fuel/byte accounting, nested scope and caller-state
  restoration checks remain in the same reviewed source/test set. The I2B addition does not add
  scoring or learner state.
- No scope opcode entry was added, preserving the P19.8 opcode budget. The owner’s I2B validation
  records approval of the scope-only meanings for existing `Point`, `Link`, and `Deref`, `Cursor`
  bindings, and pin-reach grant.

## Deferred ML gates

These are explicit prerequisites for later iterations, not part of this I2B-2 verdict:

- I3 must define stable typed storage for Beta parameters, variance, times and counts; authenticated
  observations and source-specific support remain ingress responsibilities.
- I4 must filter retired, wrong-source, stale-epoch and unhealthy observations before export; define
  missing-profile behavior without implicit priors; validate policy at the argument boundary; map
  low-level refusal codes to epistemic policy/budget errors; and test deterministic tie order across
  generator resumes. Reach provenance establishes ownership only; it does not establish freshness or
  eligibility.
- I4 must show that a resumed candidate generator carries prior selection/order state through
  caller-owned pinned data and invalidates it on belief or epistemic-state version changes.
- I4/I5/I6 each require their own full ML review for scoring, calibration, correction replay and
  model completion. I2B-2 does not establish any of those behaviors.
- Compiler emission and composite term export remain deferred to I4 as recorded in
  `i2b-validation.md`.

## Source/test manifest

The reviewed commit and complete changed-path manifest are pinned by commit ID plus the manifest
digest below. The checksummed manifest contains all 17 paths in `git diff d4334ac3..7a2dd07d`; its
digest is `6518858c70cf701019e9a2e78e16099233712785063ac922d598fb0748972e2a`.

Reviewed implementation and test file hashes:

- `apps/common/xanadu/vortex/scoped_evaluation.cpp`:
  `f6993325ba8f271abcec81f3b1b280f08101e61ea2e15f03c5b96308b8f8aa37`
- `apps/common/xanadu/vortex/scoped_evaluation.hpp`:
  `5cabf9786a95addf5c5f7dc584e75053006f59e1d2884449bac73071eacfafc2`
- `apps/common/xanadu/vortex/vortex_scoped_evaluation.cpp`:
  `0d6cc23ccd53883159694f77a70503d746686da8a48f39ee65823a2e446ae073`
- `apps/common/xanadu/vortex/vortex_vm.hpp`:
  `0f7a13a0b6ab9a7ee97aee615bc46c67090c49a12e4bddd0041027530098348c`
- `tests/xuzz/vortex_scoped_evaluation_test.cpp`:
  `17a7b683335f2444f1d566f8108376363a976053e1ae74c387673d94d915343b`
- `design/vortex-scoped-evaluation-abi.md`:
  `166baa4c1faa98a7aebc5c4d589f048024edeed09617c14f5df0870e66b5d4d9`

The full 17-path manifest is at `/tmp/p19-i2b2-review.manifest`; its file checksums were computed
from the reviewed commit contents and the I2B-2 paths have no post-commit worktree edits.

## Verification evidence

The lead’s headless I2B-2 validation logs are `/tmp/p19-i2b2-build.log`,
`/tmp/p19-i2b2-vagent_runtime_test.log`, `/tmp/p19-i2b2-vagent_actor_test.log`, and
`/tmp/p19-i2b2-xuzz_test.log`. They show a completed build; runtime: 183/183 passed; actor: 610/610
passed; xuzz: 2,680 passed from 2,707 run (27 skipped, one disabled test). I inspected the logs and
test/source correspondence; I did not rerun them.

Tally: 0 BLOCKING, 1 MAJOR, 4 MINOR carried to I3/I4.

VERDICT: FAIL
