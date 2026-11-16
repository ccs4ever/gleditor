# M4 OperandList draft review — re-review

**Disposition: ready for integration gates.** The previously missing spilled-copy-assignment failure
regression is now present and sound. This is not approval of full integration or runtime/performance
claims.

## Exact checks

Make's default compiler selection is `clang++` (`/usr/bin/clang++`, Clang 23.1.4 in this
environment); Make's generated compile lines use `-std=c++2c`. I compiled the isolated test source
with that compiler and standard:

- Native `std::inplace_vector`: **5/5 tests passed**.
- Forced pinned Beman fallback (`-DGLEDITOR_CPP26_FORCE_FALLBACK`): **5/5 tests passed**.

Both runs also compile and execute the 64-byte size assertion, contiguous/sized range assertions,
no-vector-conversion assertion, spill/mutable-span checks, inline/spill copy and move behavior,
moved-from reuse, and ninth-element allocation failure.

## Assignment regression reviewed

`SpilledCopyAssignmentFailurePreservesInlineDestination` creates a 12-element spilled source and a
three-element inline destination, forces the allocator's next allocation to fail during
`destination = source`, checks that the destination retains all prior values, then retries and
checks the full copied sequence. This reaches the copy-assignment branch that first constructs
`Spill copied(other.spill_)`, and verifies the destination is unchanged if that allocation throws.

## Integration limits

The API migration, rank-order construction, `runtimeSlot` rewrite, and
`resolveCell(std::span<const CellRef>)` boundary look coherent in the isolated source. The VM
opcode-classification function is untouched. Repository-wide app/test compilation, full tests,
formatting, and performance comparisons remain integration gates; none were run here.
