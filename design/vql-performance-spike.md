# VQL performance spike: measured scope

This executable addresses item 7 of §1.2 in
[vql-workflow-extensions.md](vql-workflow-extensions.md). It measures the existing direct evaluator
and its vector-based path/functor implementation. It does not implement a new descriptor ABI, lazy
stream engine, SIMD strategy, source adapter, or fixed-size lowering.

## Reproduce

Build through Make so its compiler, language-standard probes and headless environment apply:

```sh
make -j$(nproc) vql-spike vql-spike-abi
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  XDG_DATA_HOME=$PWD/build/xdg/data XDG_CONFIG_HOME=$PWD/build/xdg/config \
  XDG_CACHE_HOME=$PWD/build/xdg/cache \
  ./build/vql-spike --cells 1000 --dimensions 2 --warmups 3 --repetitions 21
```

`--cells` sets the result cardinality and length of the `d.items` rank. `--dimensions` sets the
number of additional named one-link hops per item. Each item has its own hop targets, so the fixture
has `cells * (dimensions + 1)` data cells plus runtime and dimension-directory cells. `--warmups 0`
is supported. Cells, dimensions and repetitions must be positive. The CLI refuses more than 64 hop
dimensions, 10 million data cells, or a million repetitions/warmups rather than silently overflowing
its fixture bounds.

Vary cardinality and hop depth separately; for example, run cells 10, 1000 and 10000 at depth 2, and
depths 1, 4 and 16 at cardinality 1000. Record commands, machine and build flags beside every
result. CPU scheduling, thermal state and other workloads remain uncontrolled. There is no timing
pass/fail threshold.

## What is timed

The benchmark uses `std::chrono::steady_clock` and prints minimum, median and nearest-rank p95
nanoseconds per complete operation. Each evaluation returns the complete result vector. Result
count, ordered reference checksum and unchanged arena cell count are checked outside the timed
interval; a disagreement exits unsuccessfully. The native visitor performs its reference checksum
inside the interval, so it is a lower-level diagnostic rather than an equal-work speedup claim.

| Output row                       | Included work                                                        |
| -------------------------------- | -------------------------------------------------------------------- |
| `parse_eval_first_use`           | First observed parse plus direct evaluation on the prepared fixture  |
| `parse_first_observed`           | First separately observed parse and AST assignment                   |
| `parse_only_warm`                | Repeated parse and replacement of the previous AST                   |
| `parse_eval_fresh_engine`        | Parse and evaluate with a newly constructed evaluator                |
| `eval_parsed_fresh_engine`       | Evaluate a retained AST with a newly constructed evaluator           |
| `eval_parsed_warm_engine`        | Reuse evaluator and retained AST                                     |
| `eval_bound_functor_warm`        | Apply a composed, previously bound path functor using a retained AST |
| `native_link_visitor`            | Follow the same real arena links without VQL vectors or name lookup  |
| `compile_identity_fresh_runtime` | Compile retained `##` AST, including graph emission and disassembly  |

Fixture construction, engine construction, functor binding, checks, reporting and sample storage are
outside the timed intervals. Fresh evaluators share the same fixture/core; this resets their
variable/functor environments, not the arena or dimension directory. Compilation uses a fresh
arena/core/VM/compiler each repetition, all created outside the interval and destroyed before the
next repetition. Emitted instructions therefore cannot accumulate over the run. Read-only direct
queries must add no cells at all. Results and ASTs are ordinary owning values: replacing them can
free the previous iteration's storage inside an interval; their final destruction is outside it.

“First observed” and “fresh engine” describe software state. They do not mean a flushed CPU cache, a
cold allocator, process startup latency or a cold filesystem. The first standalone parse follows the
first parse/evaluate call, so it is explicitly not an independent cold-parser measurement. Warm rows
run the configured number of discarded warmups before collecting samples.

## Allocations

The executable replaces scalar and array C++ `new`/`delete`, including aligned, sized-delete and
nothrow variants. A startup check exercises all eight allocation forms and validates their counts
and requested bytes. Thread-local counters are enabled only during each timed interval. Output
reports mean successful allocation calls, requested allocation bytes and non-null delete calls. The
hooks themselves add overhead; all reported rows use the same instrumentation.

These are **C++ allocation-call counts on the measurement thread**, not total memory consumption.
They exclude direct `malloc`/`realloc`, memory mappings, allocator metadata, stack storage, other
threads and any dynamically linked code that binds allocation internally. Requested bytes are not
peak or retained bytes. Delete counts can include objects allocated before the interval and exclude
objects whose destruction occurs after it. A zero count establishes only the absence of observed C++
allocation calls within this boundary. It is not evidence of a universally allocation-free VQL
engine.

## Compiler semantics and ABI

Whole-rank traversal is refused by the compiler's preflight check: its old one-`Link` lowering
cannot stand for the direct evaluator's whole rank. Functor lowering is also refused. The spike
requires both refusals and fails on unexpected acceptance so a future compiler extension must
revisit equivalence before publishing a speed comparison. It reports those diagnostics, not compiled
traversal latency. The actual common subset used here is bare `##`: the spike verifies that the
terminal instruction's input is home, runs its spawned cursor, and requires VM success. Compilation
timing covers that identity subset alone; its size does not scale with fixture size.

Sizes and alignments are printed for real `CellRef`, `CellSlot`, `DimLink`, `SignedDimensionStep`,
`PathStep`, `PathExpression`, `QueryExpression` and `VQLEngine` types. Their `sizeof` values exclude
owned heap storage. The 16-byte `PrototypeHyperHandle`, 24-byte `PrototypeCurriedMorphism` and
64-byte `PrototypeDynamicLensMatrix` are explicitly inert layout prototypes. Their successful static
assertions establish possible layouts, not implemented runtime descriptors or lowering. A 64-byte
size does not establish 64-byte alignment or a single cache-line access.

`make -j$(nproc) vql-spike-abi` emits `build/vql-spike-abi.s` using the same Make-selected compiler
and flags. Its two exported, non-inlined functions consume plain 16-byte and 24-byte integer
aggregates. Inspect the actual assembly and target before describing register or memory passage; no
such rule is portable to all ABIs or all 24-byte type classifications. This experiment does not
measure argument-passing latency.

The recorded x86-64 build with Clang 23.1.1 passes the 16-byte prototype in `%rdi` and `%rsi`,
whereas the 24-byte integer aggregate is read from the caller's stack. The generated function bodies
provide direct evidence for this build's System V ABI classification:

```asm
vql_probe_handle:
    leaq (%rdi,%rsi), %rax
    retq
vql_probe_morphism:
    movq 16(%rsp), %rax
    addq 8(%rsp), %rax
    addq 24(%rsp), %rax
    retq
```

## Recorded run

These measurements precede the subsequent iterative critique fixes. Their fixture and measured
operations are unchanged; they are not a performance certification of later lexical/error-handling
repairs. The critique ledger records final correctness checks separately. A
[post-critique run](measurements/vql-spike-post-critique-2026-10-21.txt) of the rebuilt spike with
1000 cells, two hop dimensions, three warmups and 21 repetitions passed correctness and zero
arena-growth checks; its raw timings are retained separately from the earlier tables.

Host: AMD Ryzen Threadripper 1950X, 16 cores / 32 threads, x86-64 Linux `7.2.9-1-cachyos`. Frequency
boost is enabled; the process is not pinned to a CPU. Clang 23.1.1 uses libstdc++ headers from GCC
16\. The recorded non-debug Make build selects `-O3 -g -std=c++2c -fPIC`, SDL3 and Vulkan, with the
repository's warning flags and pkg-config flags. Compiler selection and language standard come from
Make probes, not a hardcoded probe command. The full flag signature is written by Make to
`build/.buildflags`.

The following observations used `--warmups 3 --repetitions 21`, with the cells/dimensions shown.
Each configuration ran as a separate process. Other workspace activity, CPU frequency changes and
scheduling were not controlled; these are illustrative measurements, not stable latency targets. The
main 1000-cell, depth-2 run returned 1000 cells with ordered checksum `10415434106586018603`; the
arena remained at 3022 cells. Every configuration passed the output count/checksum, zero
arena-growth and allocation-hook checks.

| Main run phase                           | Median ns | p95 ns | Mean new calls | Mean requested bytes |
| ---------------------------------------- | --------: | -----: | -------------: | -------------------: |
| First observed parse + eval (one sample) |    565380 | 565380 |           7050 |               333125 |
| First standalone parse (one sample)      |      4558 |   4558 |              5 |                 3368 |
| Warm parse                               |      3126 |   3166 |              5 |                 3368 |
| Parse + eval, fresh engine               |    464828 | 474756 |           7050 |               333125 |
| Parsed eval, fresh engine                |    469296 | 525764 |           7045 |               329757 |
| Parsed eval, warm engine                 |    364176 | 464166 |           7045 |               329757 |
| Bound composed functor, warm engine      |    272320 | 276618 |           7046 |               333757 |
| Native link visitor                      |     23785 |  24277 |              0 |                    0 |
| Compile identity, fresh runtime          |      1393 |   1583 |             13 |                  954 |

The first standalone parse is not parser-cold: it follows the parse/evaluate observation. Fresh/warm
differences in these short uncontrolled measurements do not establish a caching benefit or
regression.

| Cells | Hop dimensions | Parsed warm median ns | Functor warm median ns | Native median ns | Parsed new calls | Parsed requested bytes |
| ----: | -------------: | --------------------: | ---------------------: | ---------------: | ---------------: | ---------------------: |
|   100 |              2 |                 54615 |                  54594 |             4619 |              733 |                  33797 |
|  1000 |              2 |                364176 |                 272320 |            23785 |             7045 |                 329757 |
| 10000 |              2 |               2619993 |                2443406 |           240720 |            70060 |                3432133 |
|  1000 |              1 |                161518 |                 161348 |            15199 |             4033 |                 185538 |
|  1000 |              4 |                481019 |                 445581 |            51228 |            13069 |                 618195 |

The allocation counts give direct evidence that current VQL whole-rank execution materializes and
allocates as workload grows. Bound functors still use that implementation; their descriptors do not
eliminate the per-query vector/hash work. The native visitor is an existence proof for
allocation-free link reading on this finite in-memory fixture, not a replacement evaluator with VQL
semantics. Separate configurations have uncontrolled scheduling/cache/frequency conditions; do not
infer a latency law from these samples. Sorts, joins, computed dimensions, source I/O, retained
history and network behavior were not measured.

Actual sizes/alignments from the main run:

| Current type          | Bytes | Alignment |
| --------------------- | ----: | --------: |
| `CellRef`             |     4 |         4 |
| `CellSlot`            |    32 |         8 |
| `DimLink`             |    12 |         4 |
| `SignedDimensionStep` |    64 |         8 |
| `PathStep`            |   264 |         8 |
| `PathExpression`      |   480 |         8 |
| `QueryExpression`     |   560 |         8 |
| `VQLEngine`           |   344 |         8 |

All three inert descriptor prototypes had alignment 8, including the 64-byte lens prototype. The
compiler's supported identity returned home and executed successfully. Rank traversal was refused
with `whole-rank traversal is not compiled to Vortex yet; run the query with --engine direct`. No
compiled rank throughput is reported. No fixed nanosecond, throughput, network or SIMD claim is
inferred from these sizes or measurements.

Raw output for this final-binary sweep is retained in
[the measurement record](measurements/vql-spike-2026-10-21.txt). The final functor-specific
rejection probe also passed. All five configurations reported correctness=pass and arena_growth=0.
