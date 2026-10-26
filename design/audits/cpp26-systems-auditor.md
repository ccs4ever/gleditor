# C++26 facade systems audit

Audited worktree: `origin/main` at `126be39e` (2026-10-26). This is a read-only systems audit; no
production source was changed. The only added file is this report.

## Findings

### Blocking issues

No blocking facade correctness or install-selection defect was found in the reviewed paths. This is
not a full downstream package/install test: install behavior was inspected in Makefile and exercised
through its exact selection preprocessor step, but `make install` itself was not run.

### Facade and ABI/configuration

- The facade selects each feature independently from its library feature-test macro in
  [cpp26_select.hpp](../../include/gleditor/cpp26_select.hpp:27). The relevant thresholds match the
  compatibility design: `function_ref` 202603L, optional plus optional ranges 202506L/202406L,
  `inplace_vector` 202603L, concat 202403L, and span 202311L. This correctly avoids assuming that a
  C++2c language mode implies library support.
- [cpp26.hpp](../../include/gleditor/cpp26.hpp:28) aliases only the selected implementations into
  `gleditor::cpp26`; the large inplace-vector header is kept separate in
  [cpp26_inplace_vector.hpp](../../include/gleditor/cpp26_inplace_vector.hpp:5). The headers add no
  new out-of-line facade symbols. Type layout and native symbol differences remain an ABI concern
  when these types cross the library boundary, so the consumer must honor the recorded selection.
- The installation path copies public headers, preprocesses `cpp26_select.hpp` with the build flags
  into `cpp26_config.hpp`, and installs fallback headers and licences at
  [Makefile](../../Makefile:1677). The pkg-config file adds that fallback directory as a system
  include path ([gleditor.pc.in](../../packaging/gleditor.pc.in:10)). The selection header reads the
  generated config before probing, so an installed library’s choice takes precedence over the
  consumer’s feature macros ([cpp26_select.hpp](../../include/gleditor/cpp26_select.hpp:23)).
- `GLEDITOR_CPP26_FORCE_FALLBACK=1` is applied to all compiled objects by the Makefile
  ([Makefile](../../Makefile:462)). Because the installed config defines the macros first and the
  selector only probes under `#ifndef`, this switch is a build-tree mode selector; it does not
  override a previously installed library config. That is safe for ABI agreement, although
  downstream users cannot use the define to override an installed library’s choice.
- The documented warning about borrowed optional references is material: `findRef` returns a
  reference into its lvalue range and states the lifetime requirement
  ([ranges.hpp](../../include/gleditor/ranges.hpp:49)); document APIs likewise return references
  into internal containers. I found no facade code that stores a callback past its synchronous
  caller lifetime.

### Functional parity and test coverage

- The existing compatibility test covers borrowed callable invocation, temporary callable use during
  a synchronous call, noexcept function pointers, contiguous fixed-capacity storage and overflow,
  and concat input-range reference/order/duplicate/empty cases
  ([cpp26_compat_test.cpp](../../tests/xuzz/cpp26_compat_test.cpp:29)).
- Existing application tests instantiate optional-reference return types in mesh tests
  ([mesh_host_test.cpp](../../tests/xuzz/mesh_host_test.cpp:44),
  [mesh_jobs_test.cpp](../../tests/xuzz/mesh_jobs_test.cpp:320)), but there is no direct
  compatibility assertion for optional-reference assignment or optional-as-a-range zero/one
  iteration. Dedicated library tests cover `span_at` reference preservation and in-/out-of-bounds
  behavior ([cpp26_span_test.cpp](../../tests/lib/cpp26_span_test.cpp:12)). The remaining
  optional-range coverage gap is not an observed functional failure; the standalone matrix below
  exercised it in both paths.
- The CI fallback job rebuilds the affected programs and test binaries with the force-fallback
  define ([c-cpp.yml](../../.github/workflows/c-cpp.yml:211)). This provides broad compile/runtime
  parity coverage when that job runs, but the standalone test body itself does not assert which
  selection branch is active. An explicit selection assertion or optional-range test would make the
  intended branch coverage more self-documenting.

### Performance claims and adoption

- The design’s performance statements are scoped allocation-count observations: the renderer
  callback closure measured one allocation through `std::function` and zero through `function_ref`,
  while bounded point storage measured one allocation with `std::vector` and zero with
  `inplace_vector` ([cpp26-compatibility.md](../cpp26-compatibility.md:114)). They are not
  throughput or frame-time speedup claims. I did not independently reproduce those counts, so they
  should be read as reported probe results, not as a general performance guarantee.
- A grounded next `function_ref` candidate is `MeshLensChannel::servePeerRequests`: its provider is
  a const `std::function` reference and is invoked synchronously while draining messages
  ([mesh_lens_channel.hpp](../../apps/common/xanadu/vortex/mesh_lens_channel.hpp:108),
  [mesh_lens_channel.cpp](../../apps/common/xanadu/vortex/mesh_lens_channel.cpp:139)). A
  function_ref would communicate the non-owning lifetime and permit direct lambdas without requiring
  an owning wrapper. This is an API/lifetime clarity opportunity; no speedup is claimed, and the
  network operation may dominate any wrapper cost.
- The bounded inline storage use in `layoutSegmentedPill` has a clear 32-element fast bound and
  dynamic overflow path ([segmented_pill.cpp](../../src/ui/segmented_pill.cpp:29)). Its two buffers
  use different APIs intentionally; retain the fallback/native common subset until the pinned
  implementation matures. Adoption elsewhere should require a real small upper bound and measured
  allocation or latency benefit; a random `std::vector` is not evidence of such a bound.
- Optional references already appear in public and engine lookup APIs, while optional-as-a-range
  currently has little direct use. The `std::optional<CellRef>` loop in SQL promotion mutates its
  optional on every iteration
  ([sql_promotion.cpp](../../apps/common/xanadu/zigzag/sql_promotion.cpp:91)); it is not an
  obviously cleaner optional-range conversion. No adoption is recommended solely to increase C++26
  usage.

### Additional range-helper findings (Phase 3)

- **P2 — `nthOf` rejects valid unsized random-access ranges.** Its constraint accepts any
  `input_range`, and the optimized branch checks only `random_access_range` before calling
  `ranges::size` ([ranges.hpp](../../include/gleditor/ranges.hpp:71)). Random access does not imply
  `sized_range`; a `subrange<const int*, UnsizedSentinel, subrange_kind::unsized>` models
  `random_access_range` and `input_range`, but has no size. Clang 23 compilation of `nthOf(r, 1)`
  fails at `ranges.hpp:75` because `std::ranges::size(range)` has no viable overload. The fallback
  loop already supports this range. Guard the size-based fast path with `sized_range<R>` (or use a
  sized sentinel condition); retain the iterator walk otherwise.
- **P2 — `stepped_path_view::iterator` advertises forward semantics it does not meet.** It declares
  `iterator_concept = forward_iterator_tag`
  ([stepped_view.hpp](../../include/gleditor/stepped_view.hpp:38)), but postfix increment returns
  `void` ([stepped_view.hpp](../../include/gleditor/stepped_view.hpp:69)). The range is an
  `input_range` but fails `forward_range`: the concept diagnostic traces the failure to
  `incrementable` requiring `i++` to return the iterator. Algorithms constrained on forward ranges
  therefore reject this view despite its advertised category. Either return the prior iterator from
  postfix increment, if the multipass/equality semantics are intended and valid, or advertise the
  weaker input category. Existing iteration tests exercise range for-loops but do not assert the
  concept.
- The compatibility design still describes `inplace_vector` adoption as a tether-render 17-point
  curve ([cpp26-compatibility.md](../cpp26-compatibility.md:114)). The only production consumers
  found now are the two 32-element staging buffers in segmented-pill layout
  ([segmented_pill.cpp](../../src/ui/segmented_pill.cpp:29)); tether samples use the separate fixed
  `NurbsSamples` type. This is a P2 documentation drift: update the rationale and measurement
  context before using that prose to justify broader container adoption.

The initial Phase 3 probes under `/tmp` reproduced the failures described above. The saved probe
under `design/audits/probes/` now asserts the repaired contract: both unsized random-access and
sized random-access ranges without range-level subscripting work with `nthOf`, and
`stepped_path_view` is an input range but not a forward range.

```sh
clang++ -std=c++2c -Iinclude design/audits/probes/cpp26_range_probe.cpp \
  -o /tmp/cpp26_range_probe
/tmp/cpp26_range_probe
# PASS
```

## Reproducible validation

Parent confirmed that submodules were initialized. Toolchains available were Clang 23.1.1 and GCC
16.2.1. A standalone probe was written to `/tmp/cpp26_audit_probe.cpp`; it checks optional reference
mutation and zero/one iteration, concat order, valid and out-of-range `span_at`, and contiguous
inplace-vector resize. Commands from the worktree root:

```sh
for cxx in clang++ g++; do
  for mode in native fallback; do
    flag=
    [ "$mode" = fallback ] && flag=-DGLEDITOR_CPP26_FORCE_FALLBACK=1
    "$cxx" -std=c++2c -Iinclude \
      -Ithirdparty/nontype_functional/include \
      -Ithirdparty/beman_optional/include \
      -Ithirdparty/beman_inplace_vector/include $flag \
      /tmp/cpp26_audit_probe.cpp -o "/tmp/cpp26_probe_${cxx}_${mode}" \
      && "/tmp/cpp26_probe_${cxx}_${mode}"
  done
done
```

All four compile-and-run combinations passed. Clang emitted one upstream libstdc++ 16 deprecation
warning while instantiating native `inplace_vector`; the Makefile suppresses that warning for normal
builds (`-Wno-deprecated-declarations`, at [Makefile](../../Makefile:459)).

Selection matrix, using the same `-std=c++2c` and include roots, with
`-dM -E -x c++ include/gleditor/cpp26_select.hpp`: both compilers selected all five native
facilities without the define and all five fallback facilities with
`-DGLEDITOR_CPP26_FORCE_FALLBACK=1`. In both compiler outputs the native set was `CONCAT`,
`FUNCTION_REF`, `INPLACE_VECTOR`, `OPTIONAL`, `SPAN_AT` = 1; the forced set was the same five macros
= 0.

No full application build or test suite was run, to keep this audit narrow and keep validation
scoped to header contracts; the parent also performed no full application build.
