# Round 4 critique: Language Design & Ergonomics (performance-architecture-proposals.md)

Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R3 M#1 (unmigrated `std::vector` consumers): RESOLVED — P5 names the consumers and says to use
  `OperandList`, with no implicit vector conversion.
- R3 M#2 (opcode fast path could misclassify runtime slots): RESOLVED — general classification
  precedence is preserved; the separate fast path is deferred pending a maintained invariant.

## BLOCKING

None.

## MAJOR

### 1. (§2.2, line 174) Dense `byRef` table has no representation for absent entries

Full title: Zero-initialized dense entries can confuse a missing operation with dense slot zero

**Problem:** P2 proposes replacing `byRef` with a direct dense vector but does not specify how it
distinguishes an absent operation index from a valid mapping to dense ID zero. A zero-initialized
vector would return a valid slot for missing indices, breaking `denseOf`’s optional result.

**Evidence:** P2 proposes a `std::vector<std::uint32_t>` or `VirtualMemoryArena` table and prices it
at 4 bytes per operation (`design/performance-architecture-proposals.md:174-180, 190-191`) without
specifying an occupancy representation. The current API returns `std::optional<std::uint32_t>`
(`apps/common/xanadu/zigzag/manifold.cpp:126-134`), and dense ID zero is valid for the first slot
(`manifold.cpp:477-492`).

**Fix:** Specify explicit presence tracking, such as a separate occupancy bitmap, and include its
memory cost in P2’s pricing. Do not use a reserved value as the API’s representation of absence.

## MINOR

None.

VERDICT: NOT CONVERGED
