# Structure-hyperop pre-implementation measurements

Measured on 2026-09-23 on an AMD Ryzen Threadripper 1950X (16 cores, 32 threads), using the
repository's optimized `xudu_test` build. These numbers are local baselines and substrate models,
not performance claims for unimplemented federation or quotation code. The benchmark is a disabled
GoogleTest in `tests/xudu/structure_hyperop_benchmark_test.cpp`, so normal CI does not turn timing
noise into a pass/fail gate.

## Reproduce

```sh
make -j$(nproc) xudu_test
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  XDG_DATA_HOME=$PWD/build/xdg/data XDG_CONFIG_HOME=$PWD/build/xdg/config \
  ./build/xudu_test \
  --gtest_filter=StructureHyperopBenchmark.DISABLED_ColdFoldAndArenaSubstrate \
  --gtest_also_run_disabled_tests --gtest_repeat=5 --gtest_brief=1
```

The test authors 10,000 linked cells on one dimension in an in-memory store (20,004 operations,
10,003 folded cells), then constructs a **fresh `Manifold`** from that state 20 times per sample.
"Cold fold" means cold *replay product*, not cold OS page cache or network fetch. It next times a
dependent 10,000-hop rank walk through the built manifold, the current `ArenaManifold` base
read-through path, and a **model** that adds two `unordered_map` lookups around a manifold hop. It
then mints and files 100, 1,000 and 10,000 proxy-like bare arena cells with a per-foreign-cell hash
map and one membership-rank link per cell. Finally it mints 100 bare arena cells per quotation and
links them into a rank for 1, 2, 4 and 8 repeated presentations. These loops measure proposed
storage substrates; they do not perform selectors, `GlobalOpRef` lookup or owning-space reader
dispatch.

## Results

Five samples; each fold figure is the mean of 20 fresh folds within that sample. Ranges show the
minimum and maximum sample, and the middle value is the five-sample median.

| Measurement                            | Median    | Range            | What was actually timed                          |
| -------------------------------------- | --------- | ---------------- | ------------------------------------------------ |
| Fresh Structure fold, 20,004 ops       | 1.253 ms  | 1.214–1.271 ms   | `Store::rebuildManifold()` into a new view       |
| Direct local dependent hop             | 7.40 ns   | 7.35–7.44 ns     | `Manifold::linked()`                             |
| Current arena base read-through hop    | 7.55 ns   | 7.51–8.17 ns     | `ArenaManifold::linked()` over one base          |
| Two-map federation **model** hop       | 13.87 ns  | 13.72–16.41 ns   | map → local hop → map; **not** a federated proxy |
| 100 proxy-like cells plus map/rank     | 0.0083 ms | 0.0081–0.0090 ms | 100 cells, map entries and 99 links              |
| 1,000 proxy-like cells plus map/rank   | 0.0559 ms | 0.0548–0.0651 ms | 1,000 cells, entries and 999 links               |
| 10,000 proxy-like cells plus map/rank  | 0.6963 ms | 0.5553–1.0035 ms | 10,000 cells, entries and 9,999 links            |
| 100 occurrence cells, one quotation    | 0.0040 ms | 0.0038–0.0045 ms | bare arena cells plus 99 local rank links        |
| 100 occurrence cells, two quotations   | 0.0070 ms | 0.0069–0.0077 ms | 200 cells plus 198 links                         |
| 100 occurrence cells, four quotations  | 0.0132 ms | 0.0130–0.0147 ms | 400 cells plus 396 links                         |
| 100 occurrence cells, eight quotations | 0.0253 ms | 0.0251–0.0282 ms | 800 cells plus 792 links                         |

`sizeof(CellSlot) == 32` in this build. The proxy substrate's own-cell and map-entry counts were
exactly the touched-cell count: 100, 1,000 and 10,000. Its 10,000-slot payload floor is 312.5 KiB,
before CSR link runs and hash buckets/entries; the 10,000-entry map had 10,273 buckets in this
build. The occurrence test establishes only a slot payload floor of 3.125 KiB per 100 selected cells
per quotation, or 25 KiB for eight quotations, before CSR link runs, vector capacity, origin maps,
canonical proxies and allocator overhead. The observed own-cell count was exactly
`100 × quotation count`; no foreign content bytes were copied in this surrogate. Neither figure is a
measured resident-memory delta.

The existing `ManifoldTest.aHopCostsWhatR12SaysItCosts` was also run headlessly five times over
10,000 cells × five dimensions: sequential CSR hops ranged 8.35–10.83 ns and scattered CSR hops
8.15–9.17 ns on this machine. It is a useful multi-dimension local baseline, not a federated
measurement.

## What the data supports, and what it does not

- Fresh in-memory folds of this 20k-op synthetic path are around 1.2 ms. This says nothing about
  publication verification, disk-cold reads, torrent latency, descriptor fetches or much larger
  branching histories. Fetch and fold budgets in §5.5/§5.10 remain required.
- Two hash lookups roughly doubled this particular hot dependent-hop loop. The actual federation
  path must still be measured after §5.7 phase 2; this model omits dimension binding, shadow checks,
  proxy minting, cache misses across stores and owning-space content dispatch. No accept/reject
  decision should be made from the model alone.
- Proxy-like arena storage grew linearly with touched cells in this range. This does not validate
  its eventual identity, lifecycle or resident-memory behavior; the map and rank are surrogates.
- Repeated quotation's bare slot/link cost is low at 100 selected cells, but scales with
  `selected cells × appearances`. This validates the growth *shape*, not the finished materializer
  latency or peak memory. A selector, identity map and provenance links will add work.

Before enabling production federation, measure actual p50/p95/p99 dependent hops for local,
canonical-proxy and quote-occurrence paths at 10k and 1m foreign cells, with dense and scattered
walks. Before production quotations, measure fresh and cached materialization for 100, 10k and 100k
selected cells repeated 1, 2, 4 and 8 times, reporting wall time, peak RSS, own cells and link-run
bytes. Before table migration, compare cold folds and store-open time on a representative large real
store before and after link/registry cells. All three gates must include correctness checks for
scoped edges, repeated appearances, stale completion and the unchanged base manifold.
