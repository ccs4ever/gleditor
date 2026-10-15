# Local seventh-checkpoint validation

Validated from `fbd4169c89dc04e5227754fd90148b7e813a196c` in the new
`.worktrees/spatial-ui-overhaul` checkout. Submodules are initialized. The canonical GNU Make build
passes with the host defaults, `-O3 -g`, SDL3 and all three backends. These are local results,
separate from the cloud O0 evidence. No optimized performance budget is accepted.

## Checks

| Check                                          | Local result                                                                      |
| ---------------------------------------------- | --------------------------------------------------------------------------------- |
| Full library, X11/Xvfb initial run             | 792/799; six Vulkan presentation setup failures and one cache capacity assumption |
| Full library, offscreen before test correction | 798/799; only cache capacity assumption fails                                     |
| Full library, offscreen final                  | 799/799                                                                           |
| Cache/palette/actual-device mip checks         | 53/53, including GL/GLES/Vulkan                                                   |
| UI                                             | 233/233                                                                           |
| Xuzz                                           | 99/99                                                                             |
| Unchanged strict core backend comparison       | Pass on GL/GLES/offscreen Vulkan; broad E2E excluded                              |
| Whole-page ink E2E, OpenGL                     | Pass with the unchanged 100-pixel threshold                                       |
| C++ formatting                                 | Pass with clang-format 19.1.7                                                     |
| Repository format-check                        | Stops on shell formatting in seven archived seventh-checkpoint scripts            |
| Repository lint                                | Stops on six ShellCheck findings in four archived seventh-checkpoint scripts      |
| Separate Markdown checks                       | Format passes; lint reports one MD013 in the existing reproduction report         |
| Separate YAML checks                           | Format and lint pass                                                              |
| Configuration harmony and UI text policy       | Pass, including seven policy tests                                                |

The host clang-format is version 23; its initial differences were not used to reformat the tree.
Version 19 was installed in an isolated temporary environment. Full format/lint gates remain open;
the cloud's 37-diagnostic count is not the observed local count.

The final library contains 799 cases, rather than the cloud's 790. Local optional dependencies
include ThorVG and FLAC support. Test counts are reported as executed; they are not normalized. The
initial sequential optimized-library resize measurement was 70.7 ns/update; the later pre-correction
offscreen run measured 140 ns/update. Neither establishes optimized budget acceptance or
retroactively clears the cloud O0 finding.

## Cache growth test correction

The local generic `Serif 150` resolves to Noto Serif Regular. The aligned atlas exhausts this mocked
512-square, eight-layer cache before all 16 requested glyphs fit. Interposing only the old cache
source from `67435ff` into the same local test executable passes the old capacity assertion;
`cache-control-before.log` and its exact compiler arguments retain this diagnostic control. This is
a controlled source interposition, not a full baseline-library build.

The test now stops once the second layer exists. Every attempted insertion must succeed, atlas width
must remain 512, and layers must increase while respecting the eight-layer limit. This tests the
intended layer transition without promising a host-font-dependent total capacity. Independent review
confirmed the transition remains meaningfully tested. No product code, mip regression, latency
threshold, ink threshold or backend comparison criterion changed in this local increment.

## Native preservation evidence

`gl/`, `gles/` and `vulkan/` contain local native frames, session transcripts, arguments and exact
source/activity dumps. All three runs use 800 by 600, the forced 7,438-byte source selection, chrome
scale 1.5 and Ctrl+Return at byte 3,719. Each run records one visit before Enter, two at Enter and
the same two after continuation, Back and Origin. Both source-operation dumps remain byte-identical
across all three snapshots. Twelve captured frames per successful backend were converted losslessly
to PNG and compared to their raw PPM pixels.

The original GL/GLES click-label attempt fails because no visible actionable control named Enter is
found. Those attempts are retained separately. Successful fresh-profile retries use the confirmed
default `Alt+Shift+Return` chord. This is keyboard evidence, not a pointer-affordance pass. The
unavailable click target remains a finding.

GL/GLES run through Xvfb with software GL. Vulkan succeeds with SDL offscreen on the installed AMD
Radeon RX Vega RADV driver. X11/Xvfb Vulkan cannot find a graphics/present queue here. Lavapipe is
not installed; no software-Vulkan coverage is claimed. The Vulkan outer reporting shell also hit
zsh's read-only `status` variable after the app completed; its outer exit is 1 while the recorded
inner app exit is 0. That distinction is retained in its evidence.

Independent frame review finds the mini-map obscures selected source lines and the expanded Link
panel covers part of the document. Back differs visibly: GL retains the detail panel while Vulkan
shows no panel. Both select the default tab. Static frames do not identify the cause, and this
remains open rather than a claim of complete restoration parity. Source selection, Enter,
continuation and Origin otherwise show closely matching layouts. Independent final review also finds
GLES matches GL pixel-for-pixel for Enter, continuation, Back and Origin; the source-selection
frames look alike but have pixel differences. The Back discrepancy is between Vulkan and both GL
backends. These bounded invariants do not complete J24/J25, J1–J16 preservation or J17–J32
acceptance.

## Renderer and ink limits

Strict GL/GLES ordinary and overlay captures match exactly. Vulkan differs by 0.5104 percent for the
ordinary frame and 0.5096 percent for the overlay, within the original one-percent allowance and
channel tolerance 1. Configured one/four-thread frames match; this small sample does not exercise
actual split recording. Actual frames and logs are retained under `comparison/`.

The unchanged whole-page ink test passes on OpenGL locally. This establishes that one local
backend/font configuration; it does not erase the historical 89-pixel failure or establish all
backend/font configurations. Its frame is retained under `ink/`.

Native AT-SPI, actual enlarged document text, pending/unavailable cancellation, stale targets,
repeated occurrences, cell/history endpoints and branching reopen remain open. The next task is to
resolve the visible Enter/Back findings through the shared navigation boundary and rerun the same
captures, then complete actual enlarged document text and J24/J25 variants. Publication and
pin-lifecycle audit still precedes D2 history enumeration. No second camera/view architecture, main
merge or deployment was introduced.

Raw PPMs, isolated profiles, temporary runners and local tool packages remain under
`/tmp/spatial-local-validation.W4mSRr` and `/tmp/spatial-xvfb`. `provenance.json` records local
binary hashes; `SHA256SUMS` records the durable files in this directory. Historical cloud evidence
is unchanged. Full-library final results apply to the test correction in this increment.
