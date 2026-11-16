# Independent M3 FlatU32Map review (r2)

## Verdict

**The helper is ready to enter M3 integration as an isolated data-structure component, with the
integration proof obligations below kept open.** The updated spike fixes the concrete iterator
defect from the prior review: both iterator types now expose real `pair<const uint32_t, Value>`
references, mutable access reaches only `.second`, and the compile-time forward-iterator checks
pass. This verdict does not approve or claim Manifold wiring, transaction correctness, or a
performance win.

I reran the existing isolated executable (no repository build): `flat_cell_map_test --gtest_brief=1`
reports 10/10 passed. The test translation unit statically asserts
`sizeof(pair<const uint32_t, StructureBirthInfo>) == 48` and checks both iterator concepts. No new
compile or benchmark was run in this review.

## API and lifetime review

- The map owns two `std::pmr::vector`s and retains a raw `memory_resource*`. This is valid provided
  the resource outlives every map retaining it. Copy construction intentionally retains the source
  resource; copy assignment retains the target resource; move construction/assignment transfer the
  source resource pointer. The requirement is documented in `TRADEOFFS.md` but not exercised with
  separate resource lifetimes.
- Both replacement arrays in `rehash` are constructed with exactly `resource_`, so their allocators
  compare equal to the live arrays before `vector::swap`. Copy assignment also creates its
  replacement using the target resource. This avoids unequal-PMR-allocator swap behavior.
- Move construction swaps vectors only between vectors initialized from the same resource. Move
  assignment destroys and reconstructs `*this` from the source, then restores a destination-derived
  generation. It is `noexcept` under the class's allocator/value constraints. The moved-from source
  retains its resource and has empty vectors and zero counts; tests confirm reuse with a subsequent
  insert.
- `PreparedInsert` is an owner/generation/slot/key capability. Every logical mutation and rehash
  advances the generation; committing a copied token once invalidates the other copy.
  `prepareInsert` itself can rehash and therefore invalidates earlier capabilities, as documented.
  It cannot reserve multiple pending insertions on one map. M3 must prepare/commit synchronously or
  provide a bundled reservation. Preparing across two different maps is compatible: failure in the
  second leaves the first map's logical contents unchanged, though its capacity may have grown.
- `commitInsert` does no allocation and its only potential object lifetime operations are statically
  constrained to nonthrowing. Invalid/stale tokens return `false`; the Manifold caller must treat
  this as an internal invariant failure, never as a capacity refusal.
- Copy construction and assignment may throw. Copy assignment constructs both arrays before swapping
  and thus preserves target logical contents if either allocation fails. The tests fail each
  allocation of rehash and copy construction; copy assignment test fails its second allocation.
  Failure tests validate existing key/value behavior and capacity for insertion rehash.

## Storage and rehash

The exact payload assertion is 48 bytes per bucket for this target's `StructureBirthInfo` pair.
Occupancy uses 2 bits per bucket, i.e. 0.25 bytes/bucket (amortized), so table payload is **48.25
bytes per bucket** before allocator/vector rounding. At the 2/3 live-entry ceiling this is **72.375
bytes per live entry** at full configured occupancy, excluding allocator metadata, vector capacity
slack, and the caller's dense index/bitmap. Rehash temporarily retains old and new pair arrays plus
both state arrays; peak rehash payload is approximately the sum of their bucket capacities times
48.25 bytes, again excluding allocator overhead. This is a significant cost to compare against the
old map and reserved unordered maps; the helper itself has no evidence of a win.

The two replacement arrays are both resized before any live state is moved/copied, and the value
constraints make subsequent rehash relocation nonthrowing. If either allocation fails, replacements
destruct and the old table remains intact. Tests explicitly fail allocation 0 and 1 for both growth
and tombstone cleanup. One minor missing edge check is `capacity + statesPerByte - 1` in
`stateBytes`; actual entry-vector `max_size()` is far below `SIZE_MAX` for this 48-byte pair, making
overflow unreachable on ordinary implementations, but a division/remainder formulation would make
the helper's arithmetic robust independent of that relationship.

## Remaining M3 integration proof obligations

1. Keep the planned `byRef` dense `opIndex - 1` plus occupancy bitmap separate from this sparse
   helper; reject operation index zero before dense indexing.
1. `applyStructure` must prepare every needed insertion/allocation before changing semantic state.
   On multi-map insertion failure, no partial birth/link fold may remain. Capacity refusal must
   propagate distinctly through `advance`/`Store`; `verifyAgainstFullRebuild` should validate
   recovery/retry.
1. Verify the actual PMR resource used by Manifold outlives all copied/moved maps. If a scoped test
   resource is used, owning Store/Manifold destruction order must be covered.
1. Add failure injection at the integration boundary for every preparation/allocation step and
   compare failure/retry results against full rebuild. The helper tests do not establish this
   sequencing.
1. Decide how integration treats `commitInsert == false` (internal bug) and whether generation wrap
   is defensively handled. `uint64_t` generation wrap is only a theoretical long-run concern, but
   stale-token validity relies on generations never repeating.
1. Measure payload/RSS, initialization and rehash peak costs, and the actual before/after workload
   before asserting a performance benefit. None is established here.

## Scope

The patch is a helper/test spike only. It has not been applied to the repository, built in the
repository, connected to Manifold, or measured. Missing Manifold wiring is not a helper-spike
failure, but it remains the main gate before M3 can be considered complete.
