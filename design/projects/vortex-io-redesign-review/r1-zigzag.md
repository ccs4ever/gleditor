# Round 1 critique: Xanadu / ZigZag Purist (vortex-io-redesign.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## BLOCKING

None.

## MAJOR

### 1. (§6.2, lines 273–275) Occurrence ranks lack a concrete reciprocal graph

**Problem:** The draft selects private capability occurrences and endpoint occurrence ranks as the
solution to many-to-one request relationships, but it does not define how those occurrences are
linked, cloned, chained, or scoped to their owning request. It explicitly defers the dimensions,
tags, and resolver representation. That leaves the central topology open: a direct edge from each
request's field occurrence to one shared endpoint/capability cell would violate the 2-degree rule;
just naming an "occurrence rank" does not say which cells form the rank or how field-to-value lookup
avoids that fan-in. This is a specification gap in the selected mechanism, not a claim that the
native request table is impermissible.

**Evidence:** The document says, “Requests own private field/capability occurrences. Endpoint queues
use occurrence ranks, respecting one neighbor per direction per dimension. The exact dimensions,
tags, and resolver representation must be pinned by the transient-integration spike”
(`design/vortex-io-redesign.md:273-275`). The earlier statement only says “Use private capability
occurrences and ordered occurrence ranks” (`design/vortex-io-redesign.md:221-225`); neither passage
provides the graph shape. In the implementation, `ArenaManifold::link` writes reciprocal ends and
clears displaced occupants before writing a new edge
(`apps/common/xanadu/zigzag/arena_manifold.cpp:2201-2214`), so multiple request cells cannot point
directly to the same target on one dimension/direction. The primary ZigZag reference describes the
same constraint: each cell has “at most two neighbors in every dimension”
[Nelson, *A Cosmology for a Different Computer Universe*, §1][nelson-zz], lines 164–170.

**Fix:** Add a small cell-and-link topology example for two requests sharing one endpoint and
capability. Name the request-field occurrence cells and their dimensions, show the endpoint queue as
a reciprocal ordered rank, and show how shared targets are reached without fan-in (for example, one
occurrence/clone view per owning request on the posward `d.clone` rank, with each request linking to
its own view). State how an occurrence maps back to its canonical endpoint/capability and how the
chain is traversed to recover a field. The spike can still choose ABI and resolver details, but its
acceptance gate should prove this topology with at least two requests sharing both a queue and a
managed capability.

## MINOR

None.

VERDICT: NOT CONVERGED

[nelson-zz]: https://xanadu.com/zigzag/ZZdnld/zzRefDef/
