# UX validation 2026-10-04: Resume and navigation follow-up

Follow-up to [the remaining-gap audit](audit-2026-10-03-ux-validation-gaps.md), after `a2bb77e`, in
`.worktrees/xuzz-link-context` on `feature/xuzz-link-context`. This is a focused revalidation of the
proposed next steps, not a new pass of all seventeen journeys. The xuzz-ux-validation and
xuzz-link-navigation workflows apply. Runs use SDL offscreen, dummy audio, software OpenGL and
isolated XDG directories; no live window was opened. Vulkan driver coverage and its failure are
recorded below.

## Results and fixes

| Check                                                    | Outcome                                                                                                                                              | Evidence                                                                                                                |
| -------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| J5: Close, Dismiss, Open the same document               | Pass: saved caret, anchor, selected link identity, both endsets, independent cursors and active side return                                          | `/tmp/ux-next-resume/fixed-link/fixed-close-open.log`                                                                   |
| J5: close all documents, quit, restart, Open             | Pass: the session stays empty until Open; the closed document restores caret 10 from 2 and the same link context without duplicating the document    | `fixed-link/fixed-restart.log`; permanent binary regression                                                             |
| J5: camera and selected passage across restart           | Pass: page bounds, text position and selected bytes are restored; frames differ in the overview and caret blink, so this is not whole-frame identity | `camera-visible/before.ppm`, `reopened.ppm`, `close.log`, `reopen.log`                                                  |
| J4/J16: enter an endpoint whose document was closed      | Fixed: refusal is visible, caret and selected context remain, Dismiss works, and no completed endpoint visit is recorded                             | `unavailable-final/check.log`, `before.ppm`, `refused.ppm`, `dismissed.ppm`; permanent binary and navigator regressions |
| J4: pointer selection of two overlapping links           | Pass for individually visible ribbons: clicks select link 20 with 2 left/3 right members and link 35 with 1/1, preserving caret 2                    | `pointer-edge/click-overlap.log`, `picked-many.ppm`, `picked-single.ppm`                                                |
| J16: two futures from one visit, including after restart | Pass: Activity Forward offers both saved visits; selecting each reaches its original cell or document target                                         | `fixed-link/branches.log`, `forward-second-right.log` and corresponding captures                                        |
| J16: Walks panel, visit annotation and visit reference   | No affordance: no production control or default binding was found; Back/Forward alone does not satisfy these steps                                   | Search of `apps/xudu`, `apps/xuzz` and system keymap defaults; contract in `ux_workflow_real_work.md`                   |
| J17: formatting authority is not opened                  | Fail: the quoted alpha is plain with only its destination opened; opening the source makes its bold/italic formatting appear                         | `/tmp/ux-next-unopened/session.log`, `source-unopened.ppm`, `source-opened.ppm`, `comparison.png`                       |
| J17: remote formatting authority                         | Unvalidated: no remote authority was exercised; loaded-authority propagation must not be described as remote discovery                               | Same lookup limitation as the unopened case                                                                             |
| Native accessibility                                     | Blocked: AccessKit headers/pkg-config dependency are absent and the binary reports accessibility not built                                           | Harness trees validate labels/status only, not native AT-SPI, UIA or macOS delivery                                     |

Except where an absolute path is given, artifacts are under `/tmp/ux-next-resume/`. Each UI run
retains an argument transcript and log beside its captures. Pointer picks reported kind 4 at 520,293
and 460,290 in the same authored process before the clicks. An earlier attempt reused coordinates
after restarting; those coordinates hit page background and are excluded from acceptance. This does
not validate an ambiguity chooser for ribbons that are completely coincident.

Closed-document checkpoints now append to a separate `d.closed-places` rank in the private activity
store, using the existing reading-place structure. They do not replace the latest session snapshot
or append operations to visited documents. Open uses the saved version and refuses a missing saved
version. Empty saved sessions stay empty.

Entry and saved-visit traversal now check whether the host can focus the target before changing the
walk. A refused entry retains the selected link and shows the recovery status in the panel and
harness accessibility tree. Restoring selected context alone does not require entering the target.
This guard does not fetch or automatically reopen targets.

The previous formatting refresh could replace a newly opened document's pages while its asynchronous
initial page builder was still running. Refresh is now deferred until those pages are fully loaded;
the reopened document renders its text and selection correctly.

## Initial verification

- Full `make -j$(nproc)` passed.
- All 57 Xuzz tests passed, including refusal, unchanged walk/selection, retry and unavailable
  Back/Forward/Origin coverage.
- All 231 selected Xudu tests passed with
  `*Format*:*Link*:*ReadingPlace*:*Structure*:*E2EBinaryOrchestration*`. One pre-existing disabled
  test remained disabled.
- All 120 ZigZag tests passed.
- Full `format-check` with clang-format 19 and `lint` passed. The earlier baseline formatting
  failures in 45 unrelated files were normalized separately from the functional changes.
- Backend comparison **failed** during Vulkan binary orchestration. OpenGL and OpenGL ES completed
  all 30 orchestration cases. Static image, picking, culling, atlas growth, minified-text and
  notification-overlay comparisons passed for all three backends before orchestration. The threaded
  sample had too few draws to exercise parallel recording.
- Vulkan orchestration passed 25/30 cases on RADV; five failed, including the new closed-endpoint
  case, with recorded secondary command buffers invalidated by destroyed buffers and
  `VK_ERROR_DEVICE_LOST`. An isolated software-driver reproduction using
  `VK_DRIVER_FILES=/usr/lib/claude-desktop/vk_swiftshader_icd.json` failed the same endpoint case
  with the same invalid-buffer diagnostic and exit 139; the tab-name case passed. This is a renderer
  lifetime defect, not successful navigation acceptance on Vulkan. Evidence:
  `/tmp/ux-next-backends.log`, `/tmp/ux-next-backends/xudu_vulkan.log`, and
  `/tmp/ux-next-swiftshader.log`. No claim is made that it predates this branch.

This verification does not constitute native accessibility, remote-authority discovery,
full-coincidence pointer disambiguation, or a complete J16 pass. The renderer follow-up below
addresses the Vulkan defect. The remaining implementation work is authority discovery for unopened
quotations and usable visit annotation/reference controls, followed by native accessibility
validation in an AccessKit-enabled build.

## Renderer follow-up

The Vulkan lifetime defect is fixed after `d8560b1`. Both `resizeBuffer()` and `destroyBuffer()` now
retain old allocations while a frame is being recorded. Submitted work is still idled before
mutation; the old allocation then belongs to the current frame's retirement list until its fence
confirms completion. Device-idle and shutdown paths also drain these lists. This avoids invalidating
recorded secondary command buffers and bounds retention by frame completion rather than by a later
document or atlas allocation.

Software-driver validation exposed a second defect: SwiftShader's offscreen presentation surface
forces 1280×720 while SDL's drawable is 800×600. Using the swapchain extent for rendering made
pointer coordinates and captures disagree with the window. Vulkan now keeps an independent
drawable-sized render target, viewport, picking attachment and capture; only the final presentation
blit scales to the surface extent. The readable-view regression also checks capture width and height
against the application's default view. Reading framing uses the resting document transform so the
arrival animation cannot determine the final camera position.

The backend comparison's background and glyph-boundary probes were moved to measured interior
pixels. Identity comparisons remain exact and fractional-position tolerance remains 0.005.
SwiftShader's default one-channel-level color allowance measured 1.2115% differing overlay pixels,
above the existing 1% limit. The supported `VK_CHANNEL_TOLERANCE=2` override measured 0.5858%; the
1% pixel limit and all other comparison limits remain unchanged. The default allowance in the tool
remains one. This configured software-driver result must not be presented as a pass with the default
color allowance.

The orchestration runner now gives each backend its own XDG configuration, data and cache folders.
Reusing system xanadocs after rebuilding the addressed permascroll changed the chrome and caused
about 6.3% OpenGL/OpenGL ES frame differences. The isolated core replay measured 0–0.17% for the
five affected frames (`/tmp/ux-vk-isolated-core/`).

The compiler metadata producer now removes its preprocessing-only action from the emitted command,
allowing clang-tidy to choose its syntax/analysis action. Make regenerates all 368 entries; none
retain `-E`. The database is generated through Make, not edited by hand. Strict focused
`clang-analyzer-*` checks pass on the three Vulkan translation units and `apps/xudu/views.cpp`.

The final isolated comparison passes: all 30 orchestration cases complete on each of OpenGL, OpenGL
ES and SwiftShader Vulkan, and the resulting frames pass their existing parity limits with the
explicit two-level SwiftShader color allowance. Full build, formatting, lint and focused analyzer
checks pass. The small threaded comparison sample has too few page draws to exercise parallel
recording; matching those frames is not evidence of that path.

Verification artifacts:

- `/tmp/ux-vk-lifetime-build.log`: full build.
- `/tmp/ux-vk-lifetime-lib-tests.log`: 44 buffer, canvas, glyph-batch and device-capability tests.
- `/tmp/ux-vk-lifetime-final-e2e.log`: all 30 orchestration cases pass on SwiftShader with
  `VK_DRIVER_FILES=/usr/lib/claude-desktop/vk_swiftshader_icd.json`, including drag transclusion,
  closed endpoint refusal, independent restart and extreme multipage framing.
- `/tmp/ux-vk-lifetime-focused.log`: the three focused lifetime, drag and drawable-size cases pass.
- `/tmp/ux-vk-drag-probe/`: captured evidence distinguishing the drawable/surface mismatch.
- `/tmp/ux-vk-pick-probe/` and `/tmp/ux-vk-pick-background/`: the measured picking probes.
- `/tmp/ux-vk-lifetime-verified.log`: final complete comparison with backend isolation, the explicit
  software driver and `VK_CHANNEL_TOLERANCE=2`; frames under the corresponding directory.
- `/tmp/ux-vk-lifetime-analysis.log`: focused analyzer pass.
- `/tmp/ux-vk-lifetime-format.log` and `/tmp/ux-vk-lifetime-lint.log`: full formatting and lint
  gates.

The unopened-authority formatting, visit annotation/reference and native accessibility findings
above remain open. This follow-up changes renderer resource ownership and coordinate handling; it
does not establish remote authority discovery or native assistive-technology delivery.
