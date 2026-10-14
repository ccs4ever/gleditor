# Strict capture renderer diagnosis

The seventh-increment source audit did not change production code or public headers. The
actual-device regression test in
[glyph_cache_mip_test.cpp](/workspace/gleditor/tests/lib/glyph_cache_mip_test.cpp) was run by the
owning agent against the baseline and candidate on OpenGL, OpenGLES, and Vulkan.

The sixth-increment failure is in document glyph pixels, not the notification overlay. The reported
OpenGL/OpenGLES delta occupies x=173–553, y=309–423, with maximum channel delta 23. The toast panels
are lower in the frame. Plain captures match. Candidate and old-library control captures are
byte-identical per backend, while a separate serial capture sometimes produces exact GL/GLES
equality. The seventh-reproduction report at
[evidence/reproduction/report.md](evidence/reproduction/report.md) records two fresh OpenGL runs and
two fresh OpenGLES runs, with separate Xvfb and XDG profiles for each; all four overlay frames match
exactly, while the historical GL variant differs. The historical comparison also used X11 under
Xvfb, so the available runs do not support an offscreen-versus-X11 explanation. This shows that the
mismatch predates the candidate and is not stable as a backend-only difference.

The strict capture path itself waits for recorded work: `Renderer::hasPendingWork()` includes the
animation timeline, `update()` ends the device frame before capture, and the ordinary screenshot
requires a settled frame with no pending work. The document transform is composed from the view and
the document model before `Page::collect()` supplies per-page MVP and opacity. Nothing in the
captured logs records those uniforms at capture time, so the artifacts cannot rule out every
timing-dependent scene difference, but they do not show a screenshot being taken before the
renderer’s settled condition.

The glyph atlas is the strongest source-level mechanism consistent with an output change confined to
glyph pixels and with identical output from the same backend varying across runs. The atlas has four
mip levels and an eight-texel zero border per glyph
([cache.cpp](/workspace/gleditor/src/glyphcache/cache.cpp:82)). The packer places the padded
rectangles at raw cumulative x and y offsets
([lane.hpp](/workspace/gleditor/include/gleditor/glyphcache/lane.hpp:92),
[palette.cpp](/workspace/gleditor/src/glyphcache/palette.cpp:47)); their widths and heights are the
raster bounds plus sixteen texels ([cache.cpp](/workspace/gleditor/src/glyphcache/cache.cpp:641)).
Those padded extents and origins are not rounded to the mip block size. The returned inner glyph
origin is the placement plus eight ([cache.cpp](/workspace/gleditor/src/glyphcache/cache.cpp:669)),
so an earlier, unrelated glyph can change the target glyph’s phase within the global mip grid. GL
and GLES use mipmapped linear filtering
([device_gl.cpp](/workspace/gleditor/src/render/gl/device_gl.cpp:415)); Vulkan also generates mip
levels by linear blit
([device_vk_resources.cpp](/workspace/gleditor/src/render/vulkan/device_vk_resources.cpp:393)). The
existing backend comparison comments explicitly recognize that the same glyph at an odd rather than
even atlas offset can acquire a different mip chain and produce a low-amplitude page nudge
([compare-backends.sh](/workspace/gleditor/tools/compare-backends.sh:301)).

The request-order mechanism is now confirmed in a controlled actual-device regression. The test
draws the same `A` glyph using two fresh `Serif 24` caches at 0.375 screen pixels per layout pixel,
with identical geometry and draw content; only an unused glyph request precedes `A` in one cache.
The initial `i` prefix was a negative control and passed on the baseline because it left the
relevant phase aligned. Replacing it with `m` produced a strict mismatch on all three backends: 80
differing pixels, maximum channel delta 45, and `A` origins `(8,8)` versus `(55,8)`. The fixed
candidate then passed exact pixel equality on all three backends (owner report: 53/53 owning tests
passed, including this regression). This validates that insertion order can change minified glyph
pixels through the atlas mip-grid phase and that the candidate alignment fix removes that controlled
difference. The historical capture logs do not record glyph request order or atlas coordinates, so
the experiment does not prove that this precise request order caused the sixth-increment captures.

A separate candidate Vulkan run also differed between its forced one-thread and four-thread captures
in the same document-glyph bounds, while the baseline-control Vulkan pair matched exactly. That
remains a separate repeatability observation; it does not establish that thread count caused the
sixth capture discrepancy or that it shares the confirmed atlas-phase mechanism. The same-backend
baseline and candidate captures also do not reveal the historical cache order.

The regression uses a real initialized device on each compiled backend, two fresh caches with the
same `Serif 24` font, and the same single-glyph Canvas draw. It holds text, colors, dimensions,
transform, and draw order fixed at 0.375 screen pixels per layout pixel, then compares captured RGBA
pixels exactly and reports both atlas origins on failure. The `i` control demonstrated that not
every unrelated request changes the relevant phase; `m` provided the deterministic red baseline. The
owning candidate fix aligns allocation extents and origins to the deepest mip block and zero-fills
added allocation space. The exact-pixel test passes after that change without relaxing comparison
criteria.

The evidence supports the implemented phase-independent allocation correction for this controlled
failure. It does not attribute the historical sixth capture mismatch to one specific cache insertion
sequence; preserving that distinction is important because the historical screenshots contain no
atlas diagnostics.
