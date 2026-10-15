# Local whole-page ink follow-up

The unchanged `E2EBinaryOrchestrationTest.textSurvivesAtWholePageDistance` passed on GLES and
Vulkan, one case each. Logs and the actual frames are retained here. The candidate app and library
hashes are in the parent candidate provenance; `build/xudu_test` SHA256 is
`913a4d112c47b19ba1facbaa6e7e74d5cc85a4952d547965f249a932a2f43169`.

The runs used separate `/tmp/spatial-eighth-ink/{opengles,vulkan}` profiles,
`SDL_VIDEODRIVER=offscreen`, `SDL_AUDIODRIVER=dummy`, `LIBGL_ALWAYS_SOFTWARE=1`, and XDG
data/config/cache roots under each backend profile. `XUDU_BACKEND` selected each backend and
`XUDU_SCREENSHOT_DIR` selected its `frames` directory. This description records the executed setup;
no full environment dump was captured. The test drives the real app with `--whole-pages`. The fresh
test fixture is copied by the existing test.

`frame-audit.json` records lossless PPM-to-PNG verification and a post-run recalculation of the
existing dark-ink/paper predicate. Its threshold remains 100. This bounded local pass complements
the previous checkpoint's GL pass; no broad E2E or expanded font coverage is claimed.
