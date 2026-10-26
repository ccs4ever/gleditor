# Microversion segment branch absence plan

`MicroversionId::Segment::branch` currently uses ordinal zero to represent the first segment's
missing branch. This is a true in-memory absence sentinel: the public value type exposes it, and
native comparisons and adapters interpret it. The branch field should become optional without
increasing `MicroversionId` beyond its current 24-byte representation.

## Measured sizes

Using the active compile database compiler and flags on this target:

- `sizeof(std::optional<std::uint32_t>)` is 8 bytes.
- The public `Segment { std::optional<std::uint32_t> branch; std::uint32_t number; }` is 12 bytes.
- `sizeof(MicroversionId)` is 24 bytes today.
- A candidate storage record with `firstNumber`, a packed 31-bit branch count plus one-bit
  `hasMainline` word, and a union of two 8-byte branch records or a pointer measures 24 bytes.

These are ABI size measurements only; they make no performance claim. The public `Segment` can be
larger because it is a value yielded by the API, not stored inline in each `MicroversionId`.

## Preserve leading-branch names

The first segment is not always a main-line segment. `MicroversionId{}.branch(1)` is `a1`, and
`parse("a1")` is valid. A representation that assumes the first segment has no branch would change
valid identity names.

Use structurally distinct state in a 24-byte native representation:

- `firstNumber` stores the first segment number when `hasMainline` is set.
- A packed 32-bit word stores `hasMainline` plus a 31-bit count of required branch records.
- The union stores two inline `{ordinal, number}` branch records or a heap pointer.
- Root has neither a main-line segment nor branch records.
- `a1` has no main-line segment and one branch record.
- `2` has a main-line segment and no branch records.
- `2a4` has a main-line segment and one branch record.

Keep the current two-total-segment inline threshold: with a main-line segment, one branch record is
inline; without one, two branch records are inline. The 31-bit count has an explicit maximum; refuse
construction past that limit rather than wrapping. No zero ordinal should be stored as an absent
branch. Real branch ordinals remain positive.

## Public segment access

Keep the public API type as
`Segment { std::optional<std::uint32_t> branch; std::uint32_t number; }`. Replace the current
`segments(): std::span<const Segment>` with a lightweight range over the packed representation. Its
iterator/index operation yields a `Segment` value: the first value has `nullopt` only for a
main-line first segment, and all branch records have engaged positive ordinals. It should support
the operations used by callers (`size`, `empty`, `operator[]`, `front`, `back`, and iteration)
without materializing a vector per comparison or hash. The public constructor that accepts
`std::span<const Segment>` can remain, validating that an absent branch only occurs at the first
segment and that engaged branch ordinals are positive. Keep its input copy and the segment range's
lifetime rules explicit. Add size assertions for both public `MicroversionId` and the private
storage record.

## Consumers and wire boundaries

Native consumers that need migration:

- `apps/common/xanadu/microversion.cpp`: parse, copy, formatting, parent/next/branch, path,
  ancestry, equality, and ordering. `branch()` should reject ordinal zero.
- `apps/common/xanadu/segmented_ops_spool.cpp`: ID hash should hash optional presence distinctly; do
  not use the wire zero as the native absence representation.
- `apps/common/xanadu/compact_op.hpp`: `branchOrdinalFor` is a compact operation encoding boundary;
  translate an absent segment branch to wire ordinal zero there.
- `apps/common/xanadu/binary_ops.cpp`: encode absent branch as the existing zero wire ordinal and
  decode wire zero to `nullopt`; keep all on-disk bytes and format versions unchanged.
- `apps/common/ui/hypertime_graph.cpp` and `apps/common/ui/xanadoc/session.cpp::branchOf`: inspect
  optional presence and render letters only for engaged branches.
- `tools/store-dump.cpp`: reconstruction must preserve the first main-line segment versus a leading
  named branch.
- `tests/xuzz/microversion.cpp` and `tests/xuzz/binary_ops.cpp`: cover root, main-line, leading
  branch (`a1`), nested branches, and wire round trips.

`CompactOpNode::branchOrdinal` and compact binary operation branch fields are wire/native adapter
values with their existing zero convention; this plan does not change those formats or the
`CompactOpNode` layout. It changes only the in-memory `MicroversionId::Segment` API and storage.

## Review and gates

Before implementation, confirm the range iterator works with the used ranges algorithms and does not
return references to temporary segment values. Compile every enumerated consumer and measure the
resulting `sizeof(MicroversionId)` and storage record with the project's compiler flags. Then run
the microversion and binary-op focused tests, the full native build, and the engine/runtime suites.
Check that root, `a1`, ordinary main-line IDs, nested branches, equality/order/hash consistency, and
binary round trips retain their current meanings. Do not modify a wire format or fixture unless the
binary representation itself changes.
