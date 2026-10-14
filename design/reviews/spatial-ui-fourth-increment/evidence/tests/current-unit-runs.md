# Fourth increment native unit runs

All commands ran in `/workspace/gleditor` through `/workspace/spatial-build/run.sh`, which executes
in the existing `spatial-native-session` container with a fresh XDG data, config, and cache
directory per invocation. The unit test commands were:

```sh
./build/zigzag_test
./build/xuzz_test
SDL_VIDEODRIVER=x11 xvfb-run -a -s "-noreset -screen 0 1440x1000x24" ./build/gleditor_test
```

| Binary          | Result                 | Log                                                 |
| --------------- | ---------------------- | --------------------------------------------------- |
| `zigzag_test`   | exit 0; 233/233 passed | `/workspace/spatial-build/fourth-zigzag-full.log`   |
| `xuzz_test`     | exit 0; 99/99 passed   | `/workspace/spatial-build/fourth-xuzz-full.log`     |
| `gleditor_test` | exit 0; 785/785 passed | `/workspace/spatial-build/fourth-gleditor-xvfb.log` |

The library run passed the retained-geometry and text-range checks, including
`DocGapTest.TextBoundsClipToAnInteriorBuiltPage`,
`DocGapTest.TextBoundsKeepPartialLigatureEndpointFractions`,
`DocGapTest.RetainedCaretsMatchNativeLigatureAndBidiPlacement`,
`DocGapTest.RetainedBlankLineAndMediaGeometrySurvivesByteRenumbering`, and
`DocGapTest.ReflowReplacesRetainedCaretGeometry`. Its incremental resize benchmark measured 854.6
ns/update against the 1000 ns test threshold and passed. The initial offscreen attempt skipped three
Vulkan resource lifetime tests; the successful Xvfb/x11 rerun had no skips.

The first library attempt used the wrapper's default SDL offscreen driver. It ran 785 tests but
could not create Vulkan windows for two backend parameter cases. The successful Xvfb/x11 run above
reran the full suite with Vulkan available; all three backends passed.

The stable application repair build also relinked `zigzag_test`; its rerun passed 233/233 (exit 0)
at `/workspace/spatial-build/fourth-zigzag-repair.log`.

The focused link travel E2E passed 1/1 on each backend under Xvfb/x11:

| Backend   | Result             | Log                                                 | Raw captures                                         |
| --------- | ------------------ | --------------------------------------------------- | ---------------------------------------------------- |
| OpenGL    | exit 0; 1/1 passed | `/workspace/spatial-build/fourth-link-opengl.log`   | `/workspace/spatial-build/fourth-raw/link-opengl/`   |
| OpenGL ES | exit 0; 1/1 passed | `/workspace/spatial-build/fourth-link-opengles.log` | `/workspace/spatial-build/fourth-raw/link-opengles/` |
| Vulkan    | exit 0; 1/1 passed | `/workspace/spatial-build/fourth-link-vulkan.log`   | `/workspace/spatial-build/fourth-raw/link-vulkan/`   |

`E2EBinaryOrchestrationTest.aDraggedSelectionLandsWhereItIsDropped` passed 1/1 on OpenGL (exit 0,
69.3 s). Its log is `/workspace/spatial-build/fourth-drag-selection.log`; raw PPMs and transcripts
are in `/workspace/spatial-build/fourth-raw/drag-selection/`.
`ForeignRunDragCreatesAReaderOwnedStore` remains unrun because a separate 800×600 manual framing
capture exposed an application layout regression and the root agent paused further application E2Es
pending another repair build. After the root agent resumed core renderer checks on this same stable
build, `tools/compare-backends.sh` passed with
`XUDU_TEST_BIN=/workspace/spatial-build/no-xudu-test-binary`, which explicitly excluded its broader
orchestration extension. Its log is `/workspace/spatial-build/fourth-core-compare.log`; raw PPMs are
in `/workspace/spatial-build/fourth-core-compare/`. The three-backend renderer frames, including the
captured page text, were opened and visually inspected.

The renderer comparison reported exact OpenGL/OpenGL ES frame equality (0/480,000 differing pixels),
Vulkan within 0.2471% (1,186/480,000 pixels, maximum channel delta 4), and passing picking,
notification overlay, culling, atlas-growth, coarse-text, minified-text, and threaded-recording
checks. The sample has too few page draws to exercise threaded splitting; the single- versus
four-thread frames still matched.

Lossless PNG views of the core and link captures are in [`core-frames`](core-frames/). The source
inputs remain as raw PPM files outside the repository in
`/workspace/spatial-build/fourth-core-compare/` and
`/workspace/spatial-build/fourth-raw/link-{opengl,opengles,vulkan}/`.

| PNG                                     | SHA-256                                                            |
| --------------------------------------- | ------------------------------------------------------------------ |
| `core-frames/core-opengl.png`           | `bbc7433eaa7855fc947bd89b54e8820cf19d27ef95f767d5e795ac70750e8d9b` |
| `core-frames/core-opengles.png`         | `7a6b43c22b6350ddf453f0166a08160ca10009d3a056b16f55ab27618c0d7481` |
| `core-frames/core-vulkan.png`           | `f3d7c98b3b623d9887186a18559567388ed74061492c5db08d18a43e96223d61` |
| `core-frames/link-opengl-entered.png`   | `fb3afcd08be7dae1d44dc14ae3d32b61e19820a62a5c8432dffdbb61cd259d7f` |
| `core-frames/link-opengles-entered.png` | `a93ececfb3e4e0061a3d84edd54e9f1bbbe16ca440a73d06adf1ce1869871f06` |
| `core-frames/link-vulkan-entered.png`   | `22e7db703018f85658d0e7945f40c88366b41185ebbd339dc2a4837cec5c13cd` |

The core raw PPM SHA-256 values are
`6fc78ad16e7e2a11a74bc0175f316ac3652ef51b37c9e8a06d8d5560fe5d4489` (OpenGL and OpenGL ES; identical)
and `99e720517e3338fbe13ff5e3ff6a2a7522053e521962925935ae5f3eae32e827` (Vulkan).

The application E2Es and renderer checks used source revision
`c1ab2314e7bf31db67545aefd8a59f795a0d4756` with working-tree diff SHA-256
`a03b7d5d3bac86c8904e0d9b58754a48bedffd1ce5d8a12763f8033da6a9b5aa`, `build/xudu` SHA-256
`466f0d281e86369029e537c9463992b44fb8618a7d7c00ac8255627354d5a1b5`, and `build/xudu_test` SHA-256
`6009f8ddb005e59c8ad89ad1aca847a89166580bb30b043ec81fd5771d0f44ed`; the repaired-build
`build/zigzag_test` SHA-256 was `e90707c043e74887eb23e4f76d9ff054aef732c99cda6318616a203d42e07b39`.

## Pre-margin controlled-isolation build

After restoring the generic pending-camera behavior, the final isolated build passed. Its
application E2Es passed on the latest binaries:

| Case                                                | Backend   | Result                       | Log                                                        | Raw captures                                                |
| --------------------------------------------------- | --------- | ---------------------------- | ---------------------------------------------------------- | ----------------------------------------------------------- |
| Link travel keeps exact document and sibling visits | OpenGL    | exit 0; 1/1 passed           | `/workspace/spatial-build/fourth-final-link-opengl.log`    | `/workspace/spatial-build/fourth-raw/final-link-opengl/`    |
| Link travel keeps exact document and sibling visits | OpenGL ES | exit 0; 1/1 passed           | `/workspace/spatial-build/fourth-final-link-opengles.log`  | `/workspace/spatial-build/fourth-raw/final-link-opengles/`  |
| Link travel keeps exact document and sibling visits | Vulkan    | exit 0; 1/1 passed           | `/workspace/spatial-build/fourth-final-link-vulkan.log`    | `/workspace/spatial-build/fourth-raw/final-link-vulkan/`    |
| Selection drag lands where dropped                  | OpenGL    | exit 0; 1/1 passed (70.6 s)  | `/workspace/spatial-build/fourth-drag-selection-final.log` | `/workspace/spatial-build/fourth-raw/drag-selection-final/` |
| Foreign-run drag creates reader-owned store         | OpenGL    | exit 0; 1/1 passed (131.9 s) | `/workspace/spatial-build/fourth-foreign-drag-final.log`   | `/workspace/spatial-build/fourth-raw/foreign-drag-final/`   |

The relinked final `zigzag_test` passed 233/233 at
`/workspace/spatial-build/fourth-zigzag-isolated.log`. The 785-test library, 99-test Xuzz suite, and
renderer binary were unchanged by this application-only build; the core comparison above remains
valid.

The link E2Es verified activation, accessibility state, and activity history, but visual review
found a temporary renderer-facing discrepancy in this build: the entered-frame PPMs showed both
document bodies on Vulkan, while OpenGL and OpenGL ES showed only chrome and the link panel over a
black body area. Those superseded images are `core-frames/final-link-opengl-entered.png`,
`core-frames/final-link-opengles-entered.png`, and `core-frames/final-link-vulkan-entered.png` (raw
PPMs are in the table above). Their PNG SHA-256 values are
`7c9b8b49d904f5ddb960adddefed5d44ec5fc3b5fedd0b8a0b8434f6ca069002`,
`3defdf87333947418853e1d0ed7db8fc4773f7615632002db99b3dc329f3a256`, and
`8c0406ed7be5aff14772d2caa4b15d9cfddab37455645763d75b31dface879f2`, in that order. The focused core
renderer comparison still drew text on all three backends.

This final binary set used source revision `c1ab2314e7bf31db67545aefd8a59f795a0d4756`, with
working-tree diff SHA-256 `a8e1e81fd353df32754b55f5306df1dd8910aaf9b3bc7bca7e3e49afc2b86124`:

| Binary                              | SHA-256                                                            |
| ----------------------------------- | ------------------------------------------------------------------ |
| `build/xuzz` (`build/xudu` symlink) | `49e1a068f69d73aed7fbe0a498db338d2cf02d7a5554e1f70bc405c928ac9935` |
| `build/xudu_test`                   | `6009f8ddb005e59c8ad89ad1aca847a89166580bb30b043ec81fd5771d0f44ed` |
| `build/zigzag_test`                 | `ad9dd3a0cf79415e75e5ba6d2d0d54e3fab8c797bab8f2b64e11e258af522564` |
| `build/gleditor`                    | `4b435e37947f72f8ea3433d0ed1ba67d74f13436fa4bb0500bd308675c5bcf52` |

The next settled UI build relinked `zigzag_test` again; it passed 233/233 at
`/workspace/spatial-build/fourth-zigzag-final-a8df8fd.log` (binary SHA-256
`cf2c1f48fa4f0053806bdea56e168bfcff104fbf5ae1c638ae53b3b1bea898f4`). Its application binary was
`build/xuzz` SHA-256 `a8df8fdac6b4124159b0a63aedd258e1dfdb15b0680ff0a259e7313e894b9d7d`, with
working-tree diff SHA-256 `45877ea20beca36dbf59e5f92dbaf1dc1930d53af5229074bf5d4bfc19555d28`. The
root agent paused application E2Es after a visual review found the first letters of the selected
source range clipped at the left edge. The margin repair results below supersede those app E2E
results.

## Final safe-margin build

The safe-margin repair passed the 800×600 OpenGL multiline landing gate. Its entered frame shows all
eight selected lines beginning at x=30, with the full source range above the chrome; the
[wide evidence report](../wide/report.md) records OpenGL, OpenGL ES, Vulkan, 640×480, and 1440×1000
results.

The final safe-margin build passed these application checks:

| Case                                                | Backend   | Result                       | Log                                                         | Raw captures                                                      |
| --------------------------------------------------- | --------- | ---------------------------- | ----------------------------------------------------------- | ----------------------------------------------------------------- |
| Link travel keeps exact document and sibling visits | OpenGL    | exit 0; 1/1 passed           | `/workspace/spatial-build/fourth-margin-link-opengl.log`    | `/workspace/spatial-build/fourth-raw/finalmargin-link-opengl/`    |
| Link travel keeps exact document and sibling visits | OpenGL ES | exit 0; 1/1 passed           | `/workspace/spatial-build/fourth-margin-link-opengles.log`  | `/workspace/spatial-build/fourth-raw/finalmargin-link-opengles/`  |
| Link travel keeps exact document and sibling visits | Vulkan    | exit 0; 1/1 passed           | `/workspace/spatial-build/fourth-margin-link-vulkan.log`    | `/workspace/spatial-build/fourth-raw/finalmargin-link-vulkan/`    |
| Selection drag lands where dropped                  | OpenGL    | exit 0; 1/1 passed (93.7 s)  | `/workspace/spatial-build/fourth-margin-drag-selection.log` | `/workspace/spatial-build/fourth-raw/finalmargin-drag-selection/` |
| Foreign-run drag creates reader-owned store         | OpenGL    | exit 0; 1/1 passed (136.7 s) | `/workspace/spatial-build/fourth-margin-foreign-drag.log`   | `/workspace/spatial-build/fourth-raw/finalmargin-foreign-drag/`   |

The relinked UI suite passed 233/233 at `/workspace/spatial-build/fourth-zigzag-margin-final.log`.
The link entered frames for all three backends were visually opened; OpenGL, OpenGL ES, and Vulkan
show both page surfaces and selected text. Lossless PNGs are
`core-frames/finalmargin-link-opengl-entered.png`,
`core-frames/finalmargin-link-opengles-entered.png`, and
`core-frames/finalmargin-link-vulkan-entered.png`; their SHA-256 values are
`666f5201ec51ccb9764cd864bc4ffd7c863a14067a00e02bb8a733afa54f878a`,
`00c99fe6f11a6bcce2f1dee12e6a00e0a6de9dee941424d661b6832498406c72`, and
`d4f5dd18c081b1ec49ca37b4b1ea0fa1a75c2037e4c22faad72395e8c7de4a54`.

The 785-test library suite, 99-test Xuzz suite, and core renderer comparison were reused because
their binaries were unchanged. The final binaries used source revision
`c1ab2314e7bf31db67545aefd8a59f795a0d4756`, with working-tree diff SHA-256
`17cfb9755b64a65a0829a38e813565303073da3a1b535161345e56950fdb5e06`:

| Binary              | SHA-256                                                            |
| ------------------- | ------------------------------------------------------------------ |
| `build/xuzz`        | `52dadabe6cfee1e5994a5e2e297ea35b6e9e6eef150f7f68012e9ff3a949cdf7` |
| `build/xudu_test`   | `6009f8ddb005e59c8ad89ad1aca847a89166580bb30b043ec81fd5771d0f44ed` |
| `build/zigzag_test` | `e1a3e5a1f4ba9dfab62f84509241df39fb89881cb75d6a8217f868ad5ebee20d` |
| `build/gleditor`    | `4b435e37947f72f8ea3433d0ed1ba67d74f13436fa4bb0500bd308675c5bcf52` |
