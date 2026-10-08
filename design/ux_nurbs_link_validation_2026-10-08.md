# NURBS link and tether validation, 2026-10-08

The build now draws authored many-to-many links as exact branches, two gathering points and a common
trunk. The existing context panel retains one link identity, both complete endsets and independent
member and occurrence cursors. NURBS geometry is shared with tapered drag feedback; authored links
use stationary filaments while shared primedia keeps its glass beam surface.

Design decisions and live settings are in [the design note](ui_nurbs_link_gathering.md). This is a
scoped J16 browsing and quotation-feedback validation, not a complete J16 acceptance or publication
swarm certification. Native stores and scale settings were prepared by the test fixture; their
creation does not count as user authoring through the interface.

## Run and evidence

All runs used SDL offscreen, dummy audio, software rendering and isolated XDG directories. Evidence
is under `build/nurbs-evidence/`: backend directories contain diagnostic fan images, native
navigation captures, store files and logs; `*-scaled/navigation/` repeats browsing at UI scale 1.5.
Vulkan quotation captures are under `vulkan/quotation/`. Build artifacts are not committed.

| Journey             | Step                                                              | Affordance used                                   | Outcome         | Evidence                                          |
| ------------------- | ----------------------------------------------------------------- | ------------------------------------------------- | --------------- | ------------------------------------------------- |
| J16 subset          | Open the companion store                                          | Ctrl+O, dialog, Return                            | Pass            | `*/navigation/both.png`                           |
| J16 subset          | Select one link without entering                                  | Alt+Shift+N                                       | Pass            | `*/navigation/selected.png` and `transcript.log`  |
| J16 subset          | Choose Left 2/2, cross, choose Right 3/3 and occurrence           | Alt+Shift+J, X, L                                 | Pass            | `*/navigation/independent.png`                    |
| J16 subset          | Change one side while preserving the other                        | Named pointer clicks on Cross and member controls | Pass            | `*/navigation/pointer.png`                        |
| J16 subset          | Enter the explicit occurrence and return                          | Drawn Enter and Origin buttons                    | Pass            | `*/navigation/entered.png`, `origin.png`          |
| J16 subset          | Select the overlapping disagreement link, then return             | Alt+Shift+N and P                                 | Pass            | `*/navigation/other-link.png`, `reselected.png`   |
| J16 subset          | Browse without editing visited stores                             | Same controls, close and inspect native stores    | Pass            | Test compares both stores' operation counts       |
| Quotation feedback  | Pick up, move, release into space and pouch, cancel               | Native pointer events, F2 and Escape              | Pass            | Foreign-run drag test and quotation captures      |
| Diagnostic geometry | Inspect 4×4 and fan routes                                        | Prepared stores and whole-page camera             | Diagnostic only | `full_page_many_to_many_hypermesh.png`, fan image |
| J16 complete        | Create links through UI, distant-cell branching, restart the walk | Not run                                           | Not validated   | Outside this scoped run                           |

The native browsing journey passes on OpenGL, GLES and Vulkan at scales 1 and 1.5. The five-case
rendering and interaction batch passes on OpenGL and Vulkan. GLES passes four cases initially; the
ordinary selection-drag case initially captured an empty document instead of its calibration
selection, then passes in isolation. This intermittent calibration result is retained in
`opengles/validation.log` and `drag-retry.log`; a retry is not evidence that the original failure
never occurred.

`compare-backends.sh` passes generic document, overlay, picking, coarse-text and zoom comparisons
for all three backends. Its full E2E extension was disabled in favour of the scoped matrix above.
`xvfb-run` is unavailable; the script ran with its SDL offscreen fallback. The threaded parity
sample had too few pages to exercise command-recording splitting. These are software-driver results,
not hardware GPU certification.

## Fixes verified during validation

- The panel previously covered the entered passage. It now protects the active document occurrence
  when an available strip fits it, and wraps controls into fewer columns when height permits.
- At scale 1.5 the overview intercepted a member-button click. Drawing the panel after the overview
  restores pointer selection; menus and dialogs remain above it.
- An unresolved representative endpoint could suppress other visible members. A device-backed
  regression with two visible left cells and a missing first right cell verifies that both left
  branches and the remaining right branch draw without mutating the store.

The library suite passes 758 tests, Xuzz 61, focused engine checks 53, and the final slice/overlay
suite 196. Geometry tests cover rational quarter circles, interior knots, analytic tangents, bounded
sampling, shared joins, distance-based intervals and rejected malformed inputs. A 1,000-by-1,000
endpoint test stages 1,999 representative descriptors rather than a million pairs. Endpoint
resolution retains its existing costs; this does not prove asynchronous discovery or an
allocation-free renderer. Shaders, formatting, lint and UI text policy checks pass.

## UX and aesthetics review

Independent UX and aesthetics agents were requested as required by the UI skill, but both stopped at
a service usage limit. The lead performed separate reviews of captured frames; no independent
approval is claimed. The UX pass checked visible side/member counts, explicit occurrence choice,
separate Enter, Origin and link switching. Native accessibility dumps contain the same named
controls and link identity; live assistive-technology operation was not tested because this build
has no platform accessibility backend.

The aesthetics pass selected stationary filaments over the initial brighter glass treatment for
authored links. The common trunk reduces crossing clutter, exact branches keep disjoint ranges
separate, and the selected cyan link remains prominent while the red disagreement recedes without
changing hue. Captured normal and enlarged frames retain the treatment on all three backends.
Tethers remain narrow, textured and progressively thinner toward the actual pointer. Shared segment
normals remove angular joins; sampling uses cumulative chord distance, not analytic arc length or
adaptive tessellation.

## Remaining findings

| Finding                    | What happened                                                                                                                              | Proposed follow-up                                                                        |
| -------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------- |
| Hidden slice overlays      | Registry-cell cards can obscure page text even in xanadoc-only mode, notably scale 1.5 entered frames                                      | Gate bridge world overlays with the active view mode in ViewCoordinator/BridgeCoordinator |
| Limited reading protection | The panel protects projected document endpoints when room permits; cell content and insufficient-space cases are not universally protected | Extend shared content exclusion to cells and dense scenes                                 |
| Dense quotation card       | The floating card obscures the terminal tether; its heading is clipped at scale 1.5                                                        | Shared world-card sizing and placement, as recorded in the prior tendril report           |
| Camera composition         | The default seam framing does not guarantee that both companion passages fit                                                               | Validate co-reading camera composition separately with J16                                |
| Intermittent calibration   | GLES initially rendered an empty selection-drag calibration frame; the isolated repeat passed                                              | Reproduce and trace startup/input readiness before treating it as resolved                |

Neither the full link authoring journey, persisted branching activity walks, distant-cell source
return nor complete publication verification is certified by this report.
