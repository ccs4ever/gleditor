# M0 topology spike: evidence and gaps

Date: 2026-11-23

## Result

The standalone executable model passed its four checks: private request occurrences can reach a
shared endpoint/capability through posward clone views; a two-item request queue traverses in both
directions; a binary range slice preserves NUL and `0xff` bytes and rejects out-of-bounds ranges;
and transient bytes refuse serialization. The model also exercised dense-index reuse after mark
release and exception-style unwind: the numeric cell index repeats, the allocation incarnation
changes, stale handles fail validation, and cell-owned lease-root counts drop with the truncated
cells.

This is **modeled evidence only**, not an integration pass for M0. The design requires real
`ArenaManifold` release/reuse/exception tests and a resolver/lease owner integrated with the arena.
The inspected current interface exposes `mark()` and `release()` but no `incarnationOf()` or
`matches()` API. No `build/` directory or existing test binary is present, and M0 runtime APIs do
not exist yet. No worker/service behavior was modeled.

## Topology represented

The harness used two requests, each with a private endpoint occurrence and capability occurrence.
Each occurrence links to its own clone view; clone views form reciprocal `d.clone` chains rooted at
one canonical endpoint or capability. This permits `cloneMaster` traversal while avoiding multiple
incoming links on the canonical cell. A queue sentinel and two request occurrences formed a
reciprocal ordered chain. The graph model enforces one neighbor per cell/dimension/direction and
rejects collisions at link creation.

The model intentionally keeps field names abstract. It does not settle actual dimension ids, value
tags, ZigZag rank ordering, persistence encoding, or graph reconstruction from real `ArenaManifold`
cells.

## Reproduction

Harness and binary are isolated under `/tmp/vortex-io-m0-topology/`:

```sh
g++ -std=c++23 -Wall -Wextra -Werror -O2 \
  /tmp/vortex-io-m0-topology/spike.cpp \
  -o /tmp/vortex-io-m0-topology/spike
/tmp/vortex-io-m0-topology/spike
```

Observed output:

```text
PASS modeled reciprocal graph: two requests / endpoint / capability / queue
PASS binary range slice and overflow-safe bounds
PASS transient persistence refusal
PASS modeled release+exception reuse invalidates stale incarnation and drops cell-owned lease roots
```

Source contract checked: `design/vortex-io-implementation-plan.md` §§3.1 and 3.2, §§2 and 13;
`design/vortex-io-redesign.md` §§6.2 and 13. `apps/common/xanadu/zigzag/arena_manifold.hpp`
currently declares `mark()`, `release()`, dense `CellRef` allocation, and `contains()`, but not
incarnation validation hooks. As §2.1 of the redesign states, `contains(ref)` is insufficient
because a released dense index can be reused.

## M0 disposition

The occurrence/clone topology is internally coherent under the one-neighbor rule, including two
consumers of each shared canonical value. The spike does not justify a production implementation
without the following real-code evidence:

- Add owner-managed, non-reused allocation incarnations (or an equivalently exact stale-reference
  proof) to ephemeral arena mint/release; prove same-index remint fails old-handle matching.
- Integrate payload lease-root acquire/transfer/release with arena marks, slot overwrite, proxy/base
  ownership, exceptions, cut/discard, and arena destruction. The harness only models truncation and
  exception unwind counts.
- Exercise actual `PrimediaSpan`/scratch/transient resolution, arbitrary binary slicing, checked
  offset arithmetic, and a typed transient-persistence refusal through the resolver API.
- Build actual cells and traverse request → private occurrence → clone view → canonical value, plus
  the reciprocal queue, using the real ZigZag dimensions and `cloneMaster` behavior.

The model is useful as a topology counterexample check and as a reproducible baseline for the
implementation spike, but M0 remains open pending those integration tests.
