# Real-work reliability pass, 12 October 2026

This pass follows the accepted J1–J16 journeys in
[the completed audit](audit-2026-10-12-real-work-green.md). It preserves that acceptance and turns
retained interaction plans into reusable regression tools. Stage execution alone is not a new
journey verdict: source addresses, author-store hashes, handoffs and resumed state still need
inspection against [the journey contract](ux_workflow_real_work.md).

## Durable replay

The catalog under `tools/ux-journeys/` records each stage's originating evidence, prerequisites,
required variables and driver. It includes reconstructed plans where an original plan file was
overwritten; those stages are identified in their notes. Examples describe earlier runs and are
never executable defaults. Authoring stays in the ordinary interface or a real REPL.

```sh
python3 tools/ux-journey.py --list
python3 tools/ux-journey.py --check
python3 tools/ux-journey.py --journey J1 --stage STAGE --run build/my-reliability-run
```

Choose an actual stage from `--list`, and supply each declared variable with `--var NAME=VALUE`.
Each stage gets fresh evidence and the group's shared private profile. Reopen stages must retain
that exact profile: copying settings with embedded absolute paths to another directory can reopen
the earlier authority instead. A closed consumer profile may be copied for a handoff, with source
paths preserved and inspected; a copy does not substitute for authoring through the interface.

The graphical driver uses the headless rendering path, the native driver records accessibility
Actions, and the PTY driver waits for actual prompts and checks complete output strings. All three
isolate data, configuration and cache. A failed run retains its launch, output and result.

## Video observation

The old skipped video test used a settled capture after pressing Play. Ordinary capture waits for
all contributors to stop reporting work, and a playing media widget reports work throughout
playback. The reproduction therefore captured Ended after the three-second clip. Changed pixels
alone could falsely imply that Playing was captured.

The media group's accessible value now exposes its playback state. `--await-a11y` waits for a unique
label and optional exact value with a bounded deadline, while allowing ongoing animation. It still
respects queued focus and document effects. The video test waits for Playing and uses an immediate
live capture, verifying both readiness and changed pixels. Long fixed pauses should be replaced only
when a visible state or process exit identifies the required readiness; intentional motion samples
remain timed.

## Intermittent drag calibration

The original selection calibration passed eight isolated repeats, each with 521–527 detected
selection pixels. Evidence: `build/ux-reliability-20261012/drag-repeat/result.json`. The earlier
blank capture has not been reproduced, so no cause or fix is claimed.

`tools/ux-drag-repeat.py` preserves the original interaction and saves private profiles, PPMs,
accessibility output and subsystem diagnostics. It bounds repetitions and fails when selection
pixels are missing. The existing drag regression now isolates its cache as well as data and
configuration, and includes the accessibility snapshot and retained capture path on failure. Cache
isolation prevents cross-test contamination; it is not evidence that cache sharing caused the
earlier blank frame.

```sh
python3 tools/ux-drag-repeat.py --runs 8 --evidence build/drag-reliability-run
```

## Validation

- Full `make -j$(nproc)` passed with AccessKit enabled. The final build includes pointer-pick
  diagnostics for request completion, modal dispatch, observer handling and stale focus rejection.
- All 52 focused library tests passed: automation parsing, renderer script ordering/readiness, and
  media widget behavior/accessibility.
- All five focused binary regressions passed: four animation/media cases, including the formerly
  skipped video playback test, and the complete selection-drag/drop regression.
- `make test/ux-journey-plans` validates the catalog and runs the tooling regressions; it is also a
  dependency of `make test`. All nine tooling tests passed. They cover variable substitution without
  shell evaluation, missing-variable refusal, complete PTY assertions, fresh evidence, shared
  profile isolation, process-group and separate PTY-session timeout cleanup, and refusal to pass a
  valid blank selection frame.
- `format-check`, `lint` and `git diff --check` passed.
- The catalog validates 51 stages across J1–J16. Representative durable replays passed: combined
  J1/J2 menu/key authoring and close; J7 fresh authoring, PTY calculation/error recovery/save, and
  native checks of source cells 6/15/20; J8 reopened result and recomputed 27. Consumer J8 used the
  exact J7 profile via `--profile-root`, retaining its permascroll binding.
- The first J1 replay found an archived-label mismatch, not a missing menu: current fresh defaults
  expose `Documents and session` where old evidence named `New and Open`. The J1/J2 selectors now
  match fresh defaults; the corrected replay passed. Old customized profiles need inspection.
- Eight runs of the durable drag probe also passed with valid selection pixels. Its retained
  evidence is `build/ux-reliability-20261012/retained-drag-probe/summary.json`. Two final probe runs
  additionally exercise the explicit selection-pixel failure gate and pointer diagnostics.

All reliability evidence is under `build/ux-reliability-20261012/`. The plans, runner, repeat probe
and tooling tests are tracked; screenshots and private profiles remain generated evidence. The
original blank selection frame remains unexplained after these bounded reproductions. This pass does
not claim that cache isolation fixed it or that the entire catalog has been rerun.
