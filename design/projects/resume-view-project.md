# Resume the View Project

The prompt that resumes the view project in a fresh session, on a new machine. It continues
[`start-view-project.md`](start-view-project.md), which still holds: read it for the orchestrator's
role, the decisions already made, subagents, gates, keeping the documents true, git and when to ask.
This file says where the project stands, what the owner decided since, and what the first session on
this machine learned the hard way.

You are the orchestrator. Develop on `ccr-4382af96-4tdph5`; push with
`git push -u origin ccr-4382af96-4tdph5`.

## 1. Read first

- [`view-project-status.md`](view-project-status.md) — where the project stands; continue from its
  **Next**. Its Done table names every landed package and commit, Findings names what is broken
  outside the project's reach, and Decisions taken records each ruling.
- [`start-view-project.md`](start-view-project.md) §1 to §10.
- [`../view-system-implementation-plan.md`](../view-system-implementation-plan.md) §3.1 (the spikes'
  results, which change many packages) and §4 (the packages).
- `AGENTS.md` and `.claude/rules/architectural_governance.md` — binding.

## 2. Where it stands

- **M0 and M1** are done. **M2** (the spine) has every package in, plus a closing round that fixed
  what its first gate found. Its second gate found no blocker but sent it to a second, short closing
  round; the status file's Next lists it.
- Landed ahead of their milestones: L1, L2, L5, L8, L9, R5's translucent list and the UI's move onto
  it, P1 to P3, E14, and R4's test (disabled until L7).
- Not started: M3 onward.

## 3. Decisions the owner made in the first session

These are not to be reopened.

- **One milestone at a time, completely done** (2026-10-14): "better to get a few things completely
  done than many things partially." Nothing from M3 starts until M2's gate passes. A gate that finds
  should-fix items gets a closing round, not a deferral list.
- **Workers run on Sonnet** (2026-10-14), not Opus. Reviews and frame inspection too.
- Everything else in `start-view-project.md` §2 stands.

## 4. Your first steps

1. Read the status file and check the branch builds and is green on this machine: `make -j$(nproc)`,
   `make -k test`, `./tools/compare-backends.sh`. Record the baseline in the status file if it
   differs from the one there (more cores change timing benchmarks; Vulkan may now run).
1. Do M2's second closing round, the list in the status file's Next: two Sonnet workers split so
   they touch different files (behaviour and persistence; drawing, chrome and guards), each fix with
   a test that fails without it.
1. Then a short re-check — the red team on those items, the frame inspector on the frames — and the
   go or no-go for M2, written in the status file. Both earlier gates' findings and fixes are in the
   closing-round commits (`66c3202`, `73b68a9`) and the status file.

## 5. Briefing workers

Give every worker a brief that says, beyond its package:

- Read `AGENTS.md`, `.claude/rules/architectural_governance.md`, `start-view-project.md` §2, the
  plan and the status file. Where code goes: `src/` and `include/gleditor/` generic only;
  `apps/common/xanadu/` (including `view/`) device-free; `apps/common/ui/` draws or takes input;
  `apps/xuzz/` wiring only; never `apps/xudu/` or `apps/zigzag/`.
- Owner's style: more cells and dimensions, not text that needs parsing or side tables; no sentinels
  in APIs; chaining setters; every tunable a `SettingSpec`; every xuzz chord in `system://keymap`
  (`SystemDocsTest.DefaultKeymapGivesEachChordOneActionPerScope` gates it); comments say why;
  design-document changes argued, with a change-history line.
- Start the worktree at the branch (`git merge --ff-only ccr-4382af96-4tdph5`, or reset to it if the
  tree is clean and it diverged), `git submodule update --init --recursive`, then
  `make build//src/config.h`.
- Tests that shell out (E2E, `KeyboardSlice`, `PublicationUi`, `VQueryE2E`) need
  `xuzz xudu zigzag xudu-dump vquery` built; "binary not found" is a missing build.
- A fix comes with a test that fails without it; say which were confirmed failing first.
- Never push; commit in the worktree; add paths by name.
- Report: commits, what was built, the gates run with pass counts, what contradicts the spec or
  plan, what is left.

## 6. What the first machine taught

- **Headless, always.** `make` exports the rendering and XDG variables; outside it, set them (see
  `AGENTS.md`). A Vulkan capture under SDL2 needs `xvfb-run -a -s "-screen 0 1024x768x24"`.
- **Share the compiler cache across worktrees.** The Makefile redirects `XDG_CACHE_HOME` into
  `build/xdg/cache`, so ccache lands inside each checkout: every worktree builds cold and keeps its
  own copy. Give workers one `CCACHE_DIR` (the first machine used `/root/.cache/ccache-shared`,
  capped with `ccache -M`). This filled the disk once.
- **Never wait with `until ! pgrep -f "<pattern>"`**: `pgrep -f` matches the loop's own command
  line, so it never ends; a worker waited on three of these for an hour. Never `pkill -f` by pattern
  either: it kills its own shell and other workers' compiles.
- **Never run two full gate runs in one tree at once.** `make test` and `compare-backends.sh` write
  fixed directories (`build/compare-backends/`, `build/integration_*`); two runs corrupt each
  other's results. Reviewers that run binaries point `XUDU_WORKSPACE_DIR`, `XUDU_SCREENSHOT_DIR` and
  the XDG directories at their own scratch.
- **A failing test is not a flake until proven.** Every "flake" the first session chased was a real
  race: a capture taken before the page finished arriving, a 48-pixel blank page, a notice drawn one
  frame late. Two timing benchmarks (`ArrayfiladeBenchmarkTest.VQLPredicatePushdownPruning`,
  `VortexBenchmarkTest.MemoizedVsUnmemoizedExecution`) do fail under load and pass alone.
- **`make test` uses a long-lived `build/xdg`; `compare-backends.sh` a fresh one.** A bug can hide
  in one and show in the other (the system-xanadoc link spans did).
- **Worker worktrees are local.** If the machine may be reclaimed, land work on the branch first.
- Toolchain on the first machine (a fresh Ubuntu 24.04 container): clang 19 with GCC 14's libstdc++,
  clang-format 19, and Poppler's pkg-config files changed to `-isystem`. See the status file's
  Environment section; a developer machine likely has these already.

## 7. Your first reply

Say what the status file says is next, the baseline you measured on this machine, and that you are
starting M2's gate.
