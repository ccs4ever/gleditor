# Text fitting and shared shaping cache: batch 1

Implemented in `feature/ui-text-fit-baseline`, following batch 0's instrumentation and workload.

## Implementation

`text::fit()` supports End, Middle and Start ellipsis, single-line Clip, and wrapping with line and
height limits. Zero dimensions mean unbounded. The fitter shapes each directional/script/font run
once and selects complete HarfBuzz clusters at grapheme boundaries; FriBidi orders the selected
pieces. Older libunibreak tables need a conservative same-script virama-conjunct boundary repair.
Pagination and fitting initialize the shared Unicode break tables through one thread-safe guard.

Displayed clusters own their UTF-8 strings and retain original source offsets. Synthetic ellipsis
clusters consume zero source bytes. `visibleBytes` counts the retained logical prefix; Start and
Middle may additionally display a suffix. Widths measure typographic advances, including fractional
constraints, rather than claiming to contain glyph ink overhangs.

`LayoutOptions::ellipsize` now works. Single-line labels no longer wrap. Multiline labels ellipsize
their last visible line. Decorated labels retain decoration ranges and named page metadata. Styled
and media layouts keep their existing flow and shorten the last visible text line; a media anchor on
that line ends the retained text prefix rather than becoming a glyph. Document pages explicitly
disable ellipsis so pagination continues normally. Legacy fallback shaping now passes bounded UTF-8
buffer lengths instead of assuming a null-terminated `string_view`.

`text::ShapingCache` promotes ZigZag's application cache into the library. It is a caller-owned,
bounded LRU with one capacity budget for ordinary pages and fitted labels. Keys cover text, retained
font identity and every relevant constraint, decoration, box, paragraph style and page field. Hits
perform no shaping. References survive until eviction or clear. The cache is intended for one
thread; clearing drops entries while preserving cumulative hit/miss/eviction counters. ZigZag now
uses this implementation and preserves its existing statistics interface.

Clip currently retains only complete clusters. Batch 2 must add Canvas clipping and define partial
edge-cluster drawing together with the boxed draw API. Existing per-cluster atlas rasterization
still loses Arabic joining context and cannot render some emoji fallback sequences correctly; the
new fitter preserves source clusters but does not replace that rendering pipeline.

## Verification

The full build and test target pass: 583 library, 1,249 Xanadu, 57 Xuzz, 119 ZigZag tests, plus both
network suites (12 and 1 tests). The final library rerun passes 586 tests, adding styled alignment,
media preservation and refusal of non-finite cache dimensions. Coverage includes fractional and
integer width sweeps, combining marks, ZWJ emoji, Devanagari conjuncts, Arabic RTL, mixed bidi,
start/middle ellipsis, height/line limits, source-byte mapping, bounded views, decorations, ordinary
pagination, cache key changes, eviction, font ownership and zero shaping on cache hits.

Repository lint and changed-file clang-format 19 checks pass. Repository-wide format-check retains
the untouched-file failures recorded in the batch 0 report.

`compare-backends.sh` passes document and overlay pixels, picking, culling, atlas growth, coarse
text, zoom and Vulkan recording comparisons on an isolated Xvfb display. Application E2E tests ran
in the full test target; the parity script's additional per-backend E2E section was disabled rather
than repeating the unresolved Vulkan drag timeout from batch 0. Document and label captures were
inspected. All three new GL/Vulkan label-workload capture pairs match exactly.

## Workload measurements

Same 36-label fixture, Sans 10, 1280x800, 10 warmup and 100 measured frames as batch 0. Software GL
and Vulkan runs were serialized. An initial GL run had high variance; the table uses its settled
repeat. Times include device completion and are specific to this host.

| Backend | Scene                | Frame p50 (ms) | Frame p95 (ms) | Width/height overflow rows |
| ------- | -------------------- | -------------: | -------------: | -------------------------- |
| OpenGL  | Unbounded rebuild    |         40.040 |         41.630 | 36/0                       |
| OpenGL  | Width-limit rebuild  |         10.292 |         11.051 | 0/0                        |
| OpenGL  | Width-limit retained |          5.171 |          5.632 | 0/0                        |
| Vulkan  | Unbounded rebuild    |         48.532 |         49.929 | 36/0                       |
| Vulkan  | Width-limit rebuild  |         19.211 |         21.913 | 0/0                        |
| Vulkan  | Width-limit retained |         11.017 |         12.165 | 0/0                        |

The width-limited rebuild performs 36 layout requests, 1,512 HarfBuzz calls (1,216 fallback calls),
and 8,144 input bytes per frame. The retained scene performs zero shaping. This does not establish
that the immediate Canvas path is cached: substantial atlas fallback work remains. Batch 0's
width-limited rows all overflowed vertically; their p95 was 43.396 ms for GL and 53.015 ms for
Vulkan.

Raw TSV and captures are under `build/ui-text-fit-batch1-results/`, with parity captures under
`build/ui-text-fit-batch1-parity/` in the worktree.
