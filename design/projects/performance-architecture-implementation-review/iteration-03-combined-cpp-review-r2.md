# M3 C combined static and memory review

## Findings

- **No current logical or failure-atomicity blocker found.** The latest dense-growth failure test
  seeds an existing cell, measures every PMR request made while adding a later operation that forces
  dense reallocation, fails each request ordinal, checks the prior birth/byRef mapping, cell count
  and fold position, then retries and compares against a full fold. This exercises the two-array
  dense replacement path after live state exists.
- **The growth arithmetic tests are now meaningful.** `GeometricTargetClampsWithoutOverflow` covers
  initial growth, near-`size_t` saturation and ordinary doubling through the production helper.
  `IncrementalGrowthStartsSmallAndIsGeometric` checks a small initial reservation plus bounded
  allocator calls over 512 inserts. Together they cover the prior min-128 and doubling-overflow
  defects. The helper GTest logs report 12 passes in both native and ASan+UBSan builds. The new
  Manifold integration test is present, but helper-only logs do not establish that it has run;
  root's integrated test run remains the runtime gate.
- **The experiment report matches the CSV and source policy.** `Manifold::setOpCountHint` only
  updates `byRef`; neither store fold path reserves the births table for total path length. Birth
  entries reserve incrementally, using `geometricCapacityTarget`, and the map retains a two-bucket
  staging table. At 1M sequential births the reported actual destination bucket count is 1,056,323,
  mapped value size is 40 bytes, and current/peak PMR requested bytes are 64,450,600. The
  pre-reserved control is 64,450,584 bytes; the 16-byte difference is exactly the retained staging
  buckets. These are allocator-requested bytes, not RSS, virtual reservation, or allocator metadata.
- **Interpret the birth-table bytes against the right target.** At 1M live entries, 64,450,600
  requested bytes is about 64.45 bytes/live entry for the staged unordered map. The plan's 48-byte
  figure is the flat-map slot cost before capacity slack, with a separate 1.5–2x live-capacity
  target; it is not a 48-byte total/live limit for `std::unordered_map`. The archived flat-birth
  comparison is about 101.19 requested bytes/live current and 151.78 peak, so C's staged unordered
  birth map is lower in those allocator-requested measurements. The stage table itself accounts for
  just 16 bytes over the pre-reserved unordered control. Keep this as scoped PMR evidence, not a
  universal memory result.
- **Dense map figures also reconcile.** Sequential 1M entries use 1,048,576 values plus 131,072
  presence bytes: 4,325,376 current requested bytes. Peak 6,488,064 is consistent with the old and
  new value/presence arrays coexisting during a doubling. The clustered pattern's 2,099,200 dense
  values, 262,400 presence bytes, 2,097,152 overflow capacity, 25,960,704 current and 34,611,456
  peak are consistent with the input's 63,998,992 maximum key, local 1M hint, and separate overflow
  allocation. These figures show a material sparse-cluster cost, but are synthetic allocator
  accounting rather than Store memory/RSS.
- **The branch fixtures are accurately labeled as probes.** With only keys 1, 5,000, 9,999 and a
  local hint of 148, the 4,096 floor yields an 8,192 dense bound: 5,000 lands in dense storage
  (capacity 5,000, 625 presence bytes) and 9,999 spills to the flat map (capacity 8). The
  “global-hint-10k” proxy makes all three dense and requests 41,250 current / 61,875 peak bytes. The
  report correctly calls this a selected synthetic fixture, not an observed Store branch; the
  candidate's store hint is path-local.

## Portability and scope

The helper tests assert implementation-specific `std::unordered_map` geometry (`capacity() < 64`
after a one-entry insert and a fixed allocator-call bound). The C++ standard does not specify bucket
counts, node layout, or the number/timing of rehash allocations. Those assertions passed for the
reported Clang/libstdc++ environment, but they should either be clearly scoped to that supported
release toolchain or loosened before relying on them across libc++/other standard libraries.
Likewise, the PMR byte/capacity CSV is specific to the recorded standard-library implementation;
record its version with compiler identity when publishing the comparison.

The report has no Store integration runtime or C timing, as stated. The root's frozen-source
build/tests and the three-way performance/PGO comparisons remain necessary. No builds, tests, timing
runs, or source edits were performed for this review.
