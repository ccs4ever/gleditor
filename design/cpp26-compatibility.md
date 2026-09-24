# C++26 compatibility plan

Reviewed 2026-09-24. This is a plan for using selected C++26 library facilities while the project
continues to build with its Makefile-selected C++23 or C++26 draft mode. Compiler dialect and
standard-library coverage are separate: `-std=c++2c` alone does not establish that a library type
exists. Implemented rows below are in this worktree; the other rows are estimates, not commitments.

## GitHub survey

The useful comparison is implementation scope and fit, rather than star count. A large general
utility library does not necessarily track the final C++26 API. These are the upstream projects
worth watching or using for this repository:

| Project                                                                                   | C++26 coverage and fit                                                                                                                                                     | Decision                                                                                       |
| ----------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| [Beman optional](https://github.com/bemanproject/optional)                                | Reference-style implementation of `optional<T&>` and optional range support; header-only API can be consumed by GNU Make.                                                  | Pin for the first tranche.                                                                     |
| [zhihaoy/nontype_functional](https://github.com/zhihaoy/nontype_functional)               | Complete non-owning `function_ref` implementation with qualified signatures. Its `nontype` constructor reflects an older draft; current libstdc++ uses `constant_wrapper`. | Pin for ordinary callable use; keep draft-specific constructor tags outside the common facade. |
| [Beman inplace_vector](https://github.com/bemanproject/inplace_vector)                    | Standard-oriented fixed-capacity vector; headers can be used without adopting its CMake build. Upstream still labels it under development.                                 | Pin for a narrowly scoped tether-render staging buffer.                                        |
| [PLF hive](https://github.com/mattreecebentley/plf_hive)                                  | Mature colony/hive container lineage.                                                                                                                                      | Candidate only after an iterator-stability use case is identified.                             |
| [NVIDIA stdexec](https://github.com/NVIDIA/stdexec)                                       | Broad implementation of C++26 senders and receivers; upstream calls it experimental and tracks changing wording.                                                           | Evaluate for asynchronous orchestration, not as a blanket replacement for existing callbacks.  |
| [Kokkos stdBLAS](https://github.com/kokkos/stdBLAS)                                       | Reference implementation of `std::linalg` algorithms over `mdspan`.                                                                                                        | Revisit if renderer or array workloads need standard BLAS-shaped operations.                   |
| [GSI SIMD](https://github.com/GSI-HPC/simd)                                               | Portable SIMD implementation lineage.                                                                                                                                      | Benchmark against current compiler intrinsics before adoption.                                 |
| [{fmt}](https://github.com/fmtlib/fmt) and [Abseil](https://github.com/abseil/abseil-cpp) | Large, widely used compatibility libraries; neither is a C++26 polyfill suite. {fmt} already underlies spdlog here.                                                        | Use as design references; adding them wholesale would duplicate dependencies.                  |

The pinned sources are
[nontype_functional](https://github.com/zhihaoy/nontype_functional/tree/57854ce32506ec526347637e8fcc226624466bbf)
(`57854ce32506ec526347637e8fcc226624466bbf`, BSD-2-Clause) and
[Beman optional](https://github.com/bemanproject/optional/tree/b239e587c3ecd9ae9a3bd01f6318d825241afefe)
(`b239e587c3ecd9ae9a3bd01f6318d825241afefe`, Apache-2.0 WITH LLVM-exception), and
[Beman inplace_vector](https://github.com/bemanproject/inplace_vector/tree/c7fe76da307f0d6fda5023b2d2ebef7e3f7b18d1)
(`c7fe76da307f0d6fda5023b2d2ebef7e3f7b18d1`, Apache-2.0 WITH LLVM-exception). They are Git
submodules under `thirdparty/`; do not edit them locally. Their upstream build systems are not used.

## Feature-by-feature effort

Estimates are engineer-days to integrate, test and document in this repository, after choosing a
library. They exclude future migrations of unrelated call sites. Value depends on an actual use
case; "defer" means there is currently no demonstrated advantage over existing code.

| Feature                                      | Likely source                     |             Effort | Project use and decision                                                                        |
| -------------------------------------------- | --------------------------------- | -----------------: | ----------------------------------------------------------------------------------------------- |
| `std::function_ref`                          | nontype_functional                |                2-3 | **Implemented** for synchronous Vortex `map`, `filter`, `fold` callbacks.                       |
| `std::optional<T&>`                          | Beman optional                    |                2-3 | **Implemented** for borrowed ZigZag cell lookup.                                                |
| Optional range support                       | Beman optional                    |                1-2 | **Implemented** alongside optional references; test zero or one iteration.                      |
| `std::inplace_vector`                        | Beman inplace_vector              |                3-5 | **Implemented** for the default 17-point tether curve; larger configured curves remain dynamic. |
| Checked `std::span::at` access               | Native library or local helper    |                1-2 | **Implemented** for GIF byte reads through `gleditor::cpp26::span_at`.                          |
| `std::views::concat` for two const spans     | Native library or C++23 `join`    |                2-3 | **Implemented** for VQL `!both`; see [the scoped adapter](cpp26-views-concat-plan.md).          |
| `std::hive`                                  | PLF hive                          |                4-7 | Defer pending a stable-iterator workload and memory comparison with existing containers.        |
| `std::execution` senders/receivers           | NVIDIA stdexec                    |              10-20 | Defer pending a concrete threading and cancellation design for swarm or rendering work.         |
| `std::simd`                                  | GSI SIMD or native library        |               5-10 | Defer pending profiler evidence in glyph, layout or decode kernels.                             |
| `std::linalg`                                | Kokkos stdBLAS                    |               5-10 | Defer; no current BLAS-shaped hot path.                                                         |
| `std::copyable_function`                     | Beman copyable_function or native |                2-4 | Defer; existing ownership semantics already use `std::function`.                                |
| `std::text_encoding`                         | Native library or local adapter   |                3-5 | Candidate for import metadata; encoding identification does not transcode document text.        |
| `std::indirect`/`std::polymorphic`           | Beman indirect or native library  |                3-6 | Defer pending a value-semantic ownership use case.                                              |
| C++26 formatting additions                   | {fmt} or native library           |                2-4 | Defer; spdlog/{fmt} already serve diagnostics and formatting.                                   |
| Compile-time reflection                      | Compiler support                  |             15-30+ | Cannot be faithfully supplied as a library polyfill; wait for compiler coverage.                |
| Contracts                                    | Compiler support                  |             10-20+ | Cannot preserve language-level semantics with a macro; wait for compiler support.               |
| Pack indexing and other core-language syntax | Compiler support                  | 5-15 per migration | No library polyfill; use existing templates until supported toolchains converge.                |

The first three estimates overlap; the implemented features share the compatibility header,
submodule setup and CI work. Future features should be integrated one at a time only where the
project has a measured use case.

## Implemented boundary

`<gleditor/cpp26.hpp>` exposes `gleditor::cpp26::function_ref` and `gleditor::cpp26::optional`, plus
optional's associated tags and helper. `<gleditor/cpp26_inplace_vector.hpp>` exposes
`gleditor::cpp26::inplace_vector` separately so the large container header is parsed only where it
is used. They choose native facilities per feature-test macro and otherwise use the pinned headers.
Every choice is made once, in `<gleditor/cpp26_select.hpp>`, which defines a
`GLEDITOR_CPP26_NATIVE_*` macro per facility; the facades read only those. `function_ref` requires
`__cpp_lib_function_ref >= 202603L`; `inplace_vector` requires
`__cpp_lib_inplace_vector >= 202603L`; optional references and range support require both
`__cpp_lib_optional >= 202506L` and `__cpp_lib_optional_range_support >= 202406L`.
`GLEDITOR_CPP26_FORCE_FALLBACK=1` exercises all four fallback branches. The Makefile records this
define in its object build flags, so switching modes rebuilds affected objects.

`include/gleditor/cpp26_span.hpp` adds `gleditor::cpp26::span_at(span, index)` for checked access at
byte parsing boundaries. It calls native `span::at` when `__cpp_lib_span >= 202311L`, or checks the
index and throws `std::out_of_range` before indexing on older libraries. The same force-fallback
define selects the local branch. A free function is necessary because application code cannot add a
member to `std::span`. The GIF parser uses it for header and block bytes after its existing length
checks, preserving its `nullopt`/`false` results for truncated input. The helper is a function
template with no new shared-library symbol or persistent representation.

`<gleditor/cpp26_concat.hpp>` adds `gleditor::cpp26::views::concat` for two homogeneous const spans.
It returns a lazy input range of `const T&` values. Native `std::views::concat` is selected by
`__cpp_lib_ranges_concat >= 202403L`; the C++23 and forced-fallback branch joins an owned array of
span descriptors. The two VQL direct-engine `!both` paths consume the view while their source
vectors live, then materialize the result once. The adapter promises no concrete view type,
`size()`, random access, or borrowed-range status.

The facade intentionally has no shared `nontype`/`constant_wrapper` tag: the pinned polyfill
implements the former draft name, while the current native library implements the latter. Both
branches support the ordinary lvalue callable, temporary callable used within one synchronous call,
and function-pointer forms used here. Do not store `function_ref` beyond the caller's callable
lifetime. Updating the pin to a final-wording implementation is a prerequisite for exposing the
constant-wrapper constructor as a common API.

These facades live in the library and may appear in its public API (revised 2026-09-24; they were
first confined to `apps/`). A native and a fallback specialization can differ in layout and symbol
names, so everything compiled against one `libgleditor` must make the same choice. Inside this tree
that holds by construction: the library and every program are built together with one compiler,
standard library and flag set, and `GLEDITOR_CPP26_FORCE_FALLBACK` is part of the recorded build
flags. For an installed library, `make install` preprocesses `cpp26_select.hpp` with the build's own
flags and writes the resulting `GLEDITOR_CPP26_NATIVE_*` defines to `<gleditor/cpp26_config.hpp>`,
which `cpp26_select.hpp` reads before probing. A program built later therefore follows the library's
choices rather than its own standard library's: a fallback-built library stays fallback-compatible
under any compiler, and a native-built library refuses to compile against a standard library lacking
the facility rather than mismatching silently. The fallback headers (and their licences) install
under `gleditor/cpp26-fallback/`, which `gleditor.pc` adds as `-isystem`. Persistent formats still
must not contain these types. `findCell`'s optional reference borrows from its input map; callers
must keep the map alive and avoid invalidating the referenced cell.

The tether renderer normally samples 16 segments into 17 points once per frame. With a local
allocation-counting probe using `glm::vec3`, construction of the former `std::vector` used one
allocation; `inplace_vector<glm::vec3, 17>` used zero in both the native and forced-fallback paths.
The configured segment count is not capped: counts above 16 still use `std::vector`, and the same
point-generation loop feeds the renderer in either case. The Beman implementation is still under
development: its `try_push_back` currently returns a pointer, while this native library returns
`optional<T&>`. Only `resize`, `data`, and `size` are used across both branches here. Keep this use
local until the fallback's API and constexpr support settle.

## Verification and migration

The normal build chooses native facilities when its standard library reports the required macros;
otherwise it uses the pinned fallbacks. The SDL2 CI job rebuilds `xudu` and the affected test
binaries with `GLEDITOR_CPP26_FORCE_FALLBACK=1`, then runs the compatibility, VQL `!both`, checked
span, GIF, Vortex functional and ZigZag lookup tests headlessly. Native and forced-fallback paths
were tested locally with Clang 22 and libstdc++ 16; the fallback headers were also compiled in C++23
mode with Clang and GCC. CI runner libraries may only exercise the fallback path until a newer
toolchain is added. Run both local builds and the full `make -j$(nproc) test` suite before
broadening adoption. The existing `-std=c++2c` probe with `-std=c++2b` fallback remains
authoritative.

When every supported standard library provides an adopted facility, raise or remove the feature gate
only after native and fallback tests agree and a clean build across supported compilers succeeds.
Remove a submodule only when its last fallback consumer is gone. New ABI-bearing uses need a
separate review before moving into `include/gleditor/`.

Standards context:
[function_ref P0792R14](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p0792r14.html),
[constant-wrapper constructor update P3948R1](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/p3948r1.html),
[optional references P2988R9](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p2988r9.pdf),
[optional ranges P3168R2](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p3168r2.html),
[inplace_vector P0843R14](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p0843r14.html),
[span::at P2821R5](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p2821r5.html), and
[execution P2300R10](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p2300r10.html).
