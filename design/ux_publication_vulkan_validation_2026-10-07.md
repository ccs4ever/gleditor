# Publication UI Vulkan prerequisites: validation (2026-10-07)

This batch repairs the Vulkan failures recorded in
[the reader layer report](ux_publication_layers_validation_2026-10-07.md). It changes generic
rendering and picking behavior. Publication identities, store formats and author edition decisions
are unchanged. Full P1–P7 acceptance remains a separate outstanding run.

## Repairs

Vulkan descriptor pools grow in batches of 64 pipelines. Earlier pools stay owned until device
shutdown, preserving their descriptor sets and independent canvas bindings. Pipelines retain the
existing device lifetime ownership. A regression creates 130 pipelines and checks exact red, green
and blue pixels from canvases spanning three pools.

Buffers replaced or destroyed during recording stay alive until that frame completes. Textures use
the same per-frame retirement mechanism. Reusing a frame slot waits on its existing fence, resets
command storage, then reclaims its retired resources. This bounds retention to outstanding frames
without adding a device-wide wait at each frame. Replacement storage reserves retirement metadata
before allocating driver resources, avoiding an unowned replacement on allocation failure.

Callback picks queue until a frame is open and its scene has been drawn. Rejected valid requests
remain queued for retry, and accepted replies match the exact request ID. A coordinate outside the
render target completes with a no-hit result. Previously the drag-drop callback requested its pick
before `beginFrame()`, Vulkan refused it, and the callback kept the session busy forever.

## Evidence

All runs are headless with offscreen video, dummy audio and isolated XDG directories. OpenGL uses
software rendering. Vulkan runs on the available RADV Vega device; `LIBGL_ALWAYS_SOFTWARE` does not
select a software Vulkan driver. Strict diagnostics are enabled in the resource regressions and
renderer comparison. Logs and backend captures are under `build/publication-vulkan/`; the namespace
run refreshes package UI captures under `build/publication-layers/network-ui/`.

The three resource regressions passed in 320 ms with no diagnostics. Besides pool growth, they check
destroying and resizing buffers after recorded draws, including an active-frame `waitIdle()`. Eight
consecutive frames replace and destroy their buffers without intervening captures or explicit
outside-frame waits. Trace output records six slot-reuse reclamations followed by two final drains,
three buffers per frame. The final captured pixel retains the original red draw.

The drag orchestration case passed in 13.5 seconds, checking drops onto text, into empty space and
outside the render target. The onto-text assertion verifies transclusion of the original addresses.
Architecture and ownership reviews found no remaining material issue after the invalid-coordinate
completion fix.

The full Make test run passed the four native suites: 751 library cases, 1,300 engine cases, 61 Xuzz
cases and 192 slice cases. The engine skipped its existing video playback capture test. The 12
namespace transport cases passed. All four publication namespace cases passed: author-key/topic
discovery, independent-package UI review and reader layers, live/missed update subscriptions, and
remote DHT acknowledgement with pointer retention. They took 149.8, 584.0, 162.7 and 77.1 seconds,
respectively. `make -j$(nproc) test` completed with exit status zero. The intermittent seed-review
failure was not reproduced; its underlying cause remains open.

`tools/compare-backends.sh` passed across OpenGL, OpenGL ES and Vulkan, including framebuffer,
picking, atlas, minification and text checks and all 33 orchestration cases on each backend. All
four previously failing Vulkan cases passed. Screenshot comparisons retained the established
thresholds: no tolerance changes were made. Scene orchestration allows 3% differing pixels for
OpenGL ES and 18% for Vulkan; glyph-only checks use tighter limits. Passing does not imply identical
rasterization. Maximum scene divergence measured 1.5756% for OpenGL ES and 2.5962% for Vulkan
against OpenGL. The small default sample did not exercise parallel page recording.

Repository format-check and lint passed with clang-format 19 against the final tree.

## Remaining publication work

Validate Bob and Carl creating and publishing commentary, Alice citing both in her reply, and the
complete seven journeys through the UI. Investigate the intermittent seed-review failure, move
initial signing/sealing off the rendering command path, and extend package preparation controls as
tracked in [implementation progress](publication-implementation-progress.md). Mock enrollment
remains explicit; production verification, Oracle election and identity lifecycle remain deferred.
