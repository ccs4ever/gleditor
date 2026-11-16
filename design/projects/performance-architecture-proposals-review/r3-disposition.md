# Round 3 disposition (performance-architecture-proposals.md)

Reviewed v3; revised the target to v4. The two outstanding reviewers both confirmed that the round-2
operand ceiling and nested-savepoint findings were resolved. They found four further MAJOR issues.
Each was checked against the live source before accepting its actionable premise.

| Finding                                                         | Ruling                              | Resolution in v4                                                                                                                                                                                                                                                                                                                                                                                    |
| --------------------------------------------------------------- | ----------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| vortex M1: undo payload representation and no-throw restoration | Accepted                            | P1 now maps scalar, membership, shared-handle and activation-stack inverses explicitly. Frame pushes/pops retain a single frame handle; erased buckets and stack capacity remain available while rollback needs them. Preparation precedes mutation; unwind allocates nothing. The 24-byte entry is an ABI target, with full retained-capacity pricing and optional caller-supplied `maxUndoBytes`. |
| vortex M2: generation reset retains owning payloads             | Accepted                            | Generation-only reset is limited to trivially destructible values. Owning tables destroy live payloads in O(N); generation wrap requires an occupancy sweep. Retained capacity is priced separately.                                                                                                                                                                                                |
| ergo M1: vector consumers omitted                               | Accepted                            | P5 names existing explicit-vector consumers in `vortex_vm.cpp` and `vortex_stdlib.cpp`, adds the latter to the register, specifies mutable/range/span access, and forbids implicit allocating vector conversions. Implementation must audit all consumers and compile all programs/tests.                                                                                                           |
| ergo M2: instruction classification precondition missing        | Accepted; shortcut deferred as U-P4 | Preserve register exclusions, registered opcode lookup and numeric fallback in their current order. A separate direct path requires a demonstrated, maintained instruction-cell invariant and measurements.                                                                                                                                                                                         |

## Source verification and scope

- `vortex_vm.hpp:451,470-472,518`: references are optional scalar handles, streams and iterators
  already use shared handles, and activation stacks are vectors of frame handles. The vortex M1
  suggestion that iterator internals necessarily require larger inline undo payloads is too broad:
  retaining the existing typed shared handle preserves snapshot ownership. The genuine gap was the
  absent contract for frame-stack inverses and allocation-free restoration.
- `vortex_frames.cpp:477-478,675-686`: frame push, pop and removal need explicit inverses.
- `vortex_vm.cpp:468-509,1548-1579`: scoped/mutation-group abort methods are `noexcept`.
- `vortex_stdlib.cpp:918,927` and `vortex_vm.cpp:1745,1807,1892` confirm vector consumers.
- `vortex_vm.cpp:576-591` confirms register classification precedence and numeric fallback.

The register remains P1-P5. P1 adds stream/contract mutator files; P5 adds the stdlib consumer.
Section 6 records implementation acceptance gates, not tests performed in this documentation turn.
Round 4 reruns Vortex and ergonomics, plus architecture because ownership and retention changed. The
other three reviewers' round-2 convergence carries forward unless those checks reveal a material
change in their domains. No runtime implementation is authorized by this review alone.
