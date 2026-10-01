# C++26 `views::concat` implementation plan

Implemented 2026-09-24. This plan extends the [C++26 compatibility plan](cpp26-compatibility.md)
with a first use of lazy range concatenation in the Xanadu application code. The implementation
target is a small common API whose supported operations work in both the current C++23 build and a
C++26 standard library. It does not change a persistent format or a public `gleditor` library ABI.

## Use and scope

VQL's `!both` yield mode returns the context cells followed by the newly created cells. That order
is specified in [the VQL design](vql-query-language.md) and implemented by copying two
`std::vector<CellRef>` sequences in `VQLEngine::evaluateStep()` and its clone branch. A concat view
can present the two sequences as one input range while the final result is materialized once.
Preserve order, duplicates, and the existing `CellRef` values. The view itself must not own or copy
cells.

The first API accepts **two homogeneous `std::span<const T>` inputs** and returns a view usable for
iteration and `std::ranges` input algorithms. Its namespace and spelling are
`gleditor::cpp26::views::concat(first, second)` (first shipped as `common::cpp26`; since moved into
the library, see the compatibility plan). Callers construct spans from live vectors and consume the
view before those vectors are mutated or destroyed. No code may depend on the returned view's
concrete type, `size()`, random access, or a borrowed-range guarantee. This contract keeps the first
use valid on both implementations.

The complete C++26 facility accepts heterogeneous input ranges and specifies common-reference
constraints, iterator category propagation, `iter_move`, `iter_swap`, and conditional `size()`.
Those capabilities are outside this first API. Add a call form only after a real consumer needs it
and both build paths pass the same concept and behavior tests.

## Source evaluation and selection

| Source                                                                                                        | Finding                                                                                                                                                                                        | Decision                                                                                                           |
| ------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| [WG21 P2542R8](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p2542r8.html)                         | Adopted design for `std::views::concat`; its iterator and common-reference rules are substantial.                                                                                              | Use its feature-test macro and behavior as the native reference.                                                   |
| [libc++ implementation](https://github.com/llvm/llvm-project/blob/main/libcxx/include/__ranges/concat_view.h) | Provides the standard implementation in C++26 mode. The local libstdc++ also reports `__cpp_lib_ranges_concat == 202403L` in `-std=c++2c`.                                                     | Select native `std::views::concat` when the macro is at least `202403L`.                                           |
| [range-v3 concat](https://github.com/ericniebler/range-v3/blob/master/include/range/v3/view/concat.hpp)       | Established predecessor, but it uses range-v3's own view machinery. P2542R8 notes that the standard adds stronger constraints.                                                                 | Keep as a candidate for later heterogeneous-range requirements; do not add the dependency for two `CellRef` spans. |
| C++23 `std::views::join` over an owned `std::array` of spans                                                  | A local compile-and-run spike passed with Clang and GCC in C++23 mode and Clang in C++26 mode for ordered iteration, `std::ranges::input_range`, `const T&` elements, and `std::ranges::copy`. | Use for the fallback. The array owns span descriptors; the vectors still own the cells.                            |

## Implementation steps

1. Add `apps/common/cpp26_concat.hpp`. Include `<array>`, `<ranges>`, `<span>`, and `<version>`.
   Keep the implementation separate from `apps/common/cpp26.hpp` so unrelated translation units do
   not parse range adaptor machinery. For two `std::span<const T>` inputs, return
   `std::views::concat(first, second)` when `__cpp_lib_ranges_concat >= 202403L` and
   `GLEDITOR_CPP26_FORCE_FALLBACK` is unset. Otherwise return
   `std::views::join(std::array{first, second})`. Return the view by value so its span descriptors
   remain alive.
1. Use the facade in the two VQL direct-engine `!both` branches in
   `apps/common/xanadu/vql/vql_engine.cpp`. Reserve the final vector for the sum of the two input
   sizes, then copy from the view in one pass. Keep the sources alive through that copy and preserve
   the existing result order. Leave the VQL compiler's separate stream assembly unchanged until
   direct engine parity is confirmed.
1. Add focused tests for the view in `tests/xudu/`: both empty, either side empty, duplicate cells,
   source order, and `std::ranges::input_range` with `const CellRef&` elements. Extend
   `VQLEngineTest.CreationSugarAndYieldModes` with `!both` and a clone case; compare direct-engine
   output to the existing compiler/VM behavior where the test fixture supports it.
1. Let CI's existing normal `make -j$(nproc)` and `test/all` steps cover the native branch where the
   runner provides it. Extend the SDL2 forced-fallback step to run the new view and VQL cases after
   rebuilding `xudu_test` with `GLEDITOR_CPP26_FORCE_FALLBACK=1`. Locally run the filtered tests
   headlessly after both a normal `make -j$(nproc)` and a forced-fallback build. Run
   `make format-check` and update the C++26 compatibility plan with the final contract and evidence.

## Acceptance and effort

The native and forced-fallback paths must return identical `CellRef` sequences for the cases above.
Both returned types must model `std::ranges::input_range` and yield `const CellRef&`. Creating the
view must not allocate or copy cells; the final VQL result remains a vector because callers own its
values. The fallback must compile in C++23 mode. No CMake build step or new submodule is needed for
this scope.

Estimate **2–3 engineer-days**: about half a day for the adapter and concept checks, one day for VQL
adoption and behavior tests, and half to one day for native/fallback builds, CI, and documentation.
A general replacement for the whole standard `concat_view` interface would need a separate design
and a larger estimate, roughly **5–10 days**, because the iterator and heterogeneous-reference
behavior must be proven across supported toolchains.

## Implemented boundary

`<gleditor/cpp26_concat.hpp>` (originally `apps/common/cpp26_concat.hpp`) provides the two-span
adapter. It selects native `std::views::concat` when the standard library advertises
`__cpp_lib_ranges_concat >= 202403L`, and selects `std::views::join` over an owned array of spans in
C++23 or when `GLEDITOR_CPP26_FORCE_FALLBACK=1` is set. Its result borrows the source elements, so
callers must consume it before changing or destroying either source.

The VQL direct engine materializes both `!both` branches through one `concatCellStreams` helper. The
focused tests cover creation, cloned link targets, order, empty inputs, duplicates, and the
input-range reference type. The SDL2 CI fallback step includes these cases. The VQL compiler's
separate stream assembly has no shared `CellRef` vector interface, so this change does not alter it.

Locally, Clang in `-std=c++2c` mode reported `__cpp_lib_ranges_concat == 202403L`; the native
focused tests passed. The forced-fallback build passed all 14 compatibility and VQL direct-engine
tests, and a standalone C++23 syntax check accepted the fallback. `make format-check` passed.
