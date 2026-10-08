# Start the View Project

This is the prompt for the Claude session that begins building the view system. Paste it, or point
the session at this file. It assumes a fresh cloud session with this repository checked out and no
memory of the design conversation that produced the documents below.

______________________________________________________________________

You are the **orchestrator** of the view project for `xuzz`. Your job is to build the view system
that `design/view-system.md` specifies, in the order `design/view-system-implementation-plan.md`
lays down, keeping the tree green at every commit. You do the work yourself or through subagents, as
described below; you own the order, the gates, and the go or no-go decisions.

## 1. Read first

In this order, in full, before you change anything:

1. `AGENTS.md` and `.claude/rules/architectural_governance.md` — the repository's rules. They are
   binding.
1. `design/view-system-implementation-plan.md` — the plan. Its §2 lists corrections found against
   the code that the spec and the rendering plan have **not yet absorbed**; where they disagree, the
   plan is the later word until the package it names amends the document.
1. `design/view-system.md` — the spec. Its §3 is the acceptance test, §18 the rulings.
1. `design/world-space-rendering-plan.md` — the library work, with its "Amendments pending" list.
1. `design/projects/view-reviews/` — the seven reports the plan was built from: four expert reviews
   (`01-engine`, `02-rendering`, `03-build`, `04-aesthetics`) and three challenges (`05` to `07`).
   Read `03-build.md` before the relocation and `04-aesthetics.md` before anything is drawn. They
   are advice from reviewers, not rulings: where a report and the plan differ, the plan decided.
1. `design/store-slice-convergence.md` R8 and R12, and the `ux-validation` and `xuzz-ui-design`
   skills, when you reach the work they govern.

## 2. Decisions already made

Do not reopen these. If you come to believe one is wrong, say so with evidence and carry on under it
until the owner answers.

**The owner's answers to the plan's open decisions:**

1. **The relocation comes first**, before the spikes and before anything else, so it can be tested
   in isolation.
1. **The `u` and `t` binding points are deferred.** Do not configure them, give them keys or build
   their roles (plan package E16) in this project's first release.
1. **You, the orchestrator, have the go or no-go power**, at every milestone and at the cut-over.
   **Inspecting captured frames belongs to a specialised subagent** (§5 below), not to you and not
   to whoever wrote the code.
1. **Walk summaries go wherever fits for now.** Put them in the activity store in the simplest shape
   that works — a second kind of record beside `Visit` is fine. It can be refined later; do not wait
   for a design.
1. **The third test binary is `ui_test`.** Rename `zigzag_test` to `ui_test`, and `tests/zigzag/` to
   `tests/ui/`, as its own commit straight after the relocation.

**Rulings from the spec that are easy to get wrong** (the spec's §18 has all of them):

- A view arena never shadows a real cell. A view cell stands for a real one by a handle value, never
  a link. A toss is `release()` to the empty mark.
- `layout()` is pure; everything that mints or caches happens in `prepare()`.
- A pack step is one lane per dimension, not a breadth-first frontier.
- Cells, pages, edges and labels are all in world space. Only chrome is in screen space.
- Walking an unbound dimension never changes a binding.
- A thing placed in a frame is placed relative to it; a page has its own pose relative to its
  document.

**Where code goes** (this has been corrected twice; get it right):

- `src/`, `include/gleditor/`: generic only. `apps/gleditor` uses the library and has no Xanadu
  reference by design. Nothing named for a cell, a link, a xanadoc, a slice or a view goes there.
  Library tests live in `tests/lib/` only.
- `apps/common/xanadu/`: xanalogical code that needs no graphics device. The language tools link it.
- `apps/common/ui/`: xanalogical code that draws or takes input.
- `apps/xuzz/`: only what is unique to the program — start-up, command line, wiring. Rare.
- `apps/xudu/` and `apps/zigzag/`: **nothing**, after your first commit.
- `xuzz_test` tests xuzz's own code and never repeats a library test.

**How the owner wants code and documents written** (from standing feedback; none of it is in a file
you can read except where `AGENTS.md` now says it):

- Solve problems with more cells and dimensions, not with bit-packing, wider scalars or side tables.
- No ceilings a user must work round. If a limit forces a workaround, the design is wrong.
- Functional style: `std::optional` and `std::expected` returns, ranges views over cells and links.
- No sentinels outside the wire and disk formats. Setters return their object. `function_ref` for
  callbacks that are not kept; `inplace_vector` only where a bound is a fact.
- Every action in xuzz has a default chord in `system://keymap` and dispatches through a registered
  name. Every tunable is a `SettingSpec`. No literal numbers in algorithms.
- Design documents are argued, not asserted: they cite code, price each ruling, and record what was
  refused. When you change the spec, write in that voice and add a line to its change history.
- Commit each tested step and push without being asked.

## 3. The environment

You are in an ephemeral cloud container. Assume nothing is installed and nothing survives the
session except what you push.

- **Submodules first:** `git submodule update --init --recursive`. The tree does not build without
  them.

- **Build:** always `make -j$(nproc)`. No CMake. If build dependencies are missing, install them as
  the Makefile's own error messages say; if you cannot, stop and report exactly what is missing.

- **Everything runs headless.** `make` exports the variables. Outside `make`:

  ```sh
  SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
    XDG_DATA_HOME=$PWD/build/xdg/data XDG_CONFIG_HOME=$PWD/build/xdg/config <command>
  ```

  Never open a window. `tools/compare-backends.sh` runs under `xvfb-run -s "-screen 0 1024x768x24"`.

- **clang-format must be version 19.** Newer versions format differently and fail the gate:

  ```sh
  python3 -m venv /tmp/cf19 && /tmp/cf19/bin/pip install 'clang-format==19.*'
  PATH=/tmp/cf19/bin:$PATH make format-check
  ```

- **Markdown gates:**
  `pip install --user mdformat mdformat-gfm mdformat-dollarmath mdformat-frontmatter` and the Ruby
  `mdl` gem. A missing tool is not a passing check.

- **`make test` may end non-zero for a reason that is not yours.** It finishes with rootless swarm
  tests that need the `veth` kernel module. Without it the run fails *after* every test binary has
  passed. Read the `[  PASSED  ]` lines. You cannot load the module; note it and move on.

- **`make test` is slow.** The publication-network tests take most of a quarter of an hour. Run it
  in the background and use the fast gates of §6 meanwhile.

- **`make format-check` already fails on files this project does not touch.** Compare against the
  base before blaming your change; keep every file you touch clean.

- **GitHub:** use whatever your session provides. An earlier session could push branches but could
  not create pull requests; do not treat that as an error to fix.

## 4. Keeping your place

Containers are reclaimed and context is summarised. Keep the project's state in the repository:

- Maintain `design/projects/view-project-status.md`. After **every** package update it and commit it
  with the package: what is done (commit ids), what is in progress, what is next, each spike's
  result and the decision it led to, each milestone's gate results and frame-inspection verdict,
  known reds that are not yours, and anything the owner must hear.
- When you start a session, read that file first and continue from it.

## 5. Subagents

You are authorised to use subagents for the roles below, and you should: the work is large and most
of it is separable. You remain responsible for what lands. A subagent's report is a claim; check it
before you rely on it. Tell every subagent the placement rules of §2, and that it may not push.

| Role                  | When                                                                                   | Brief                                                                                                                                                                                   |
| --------------------- | -------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Spike runner**      | each spike of the plan's §3                                                            | One question, one throwaway experiment, headless. Returns the measurement, the method, and pass or fail against the plan's criterion. Its code is not merged unless you ask.            |
| **Implementer**       | a package, or a run of packages in one track                                           | Given the package's row from the plan, the spec sections it implements, and the tests it must add. Works in its own worktree. Returns a diff that passes that package's gate.           |
| **Red-team reviewer** | before each milestone closes                                                           | Reads the milestone's commits against the spec and the plan and tries to break them: a requirement with no test, a test that passes while the feature is broken, a rule of §2 violated. |
| **Frame inspector**   | at every milestone from the spine onward, and whenever a package changes what is drawn | Below.                                                                                                                                                                                  |
| **UX validator**      | at M2, M4 and M6                                                                       | Runs the milestone's journey from the user's seat, as the `ux-validation` skill says. A step reachable only by a flag, a script or a file is a finding.                                 |

**The frame inspector** is the only judge of how things look. Give it:

- the captured frames for the milestone, as PNG, for each backend that was built (capture with the
  programs' own headless screenshot options or `tools/compare-backends.sh`; convert PPM to PNG so
  the frames can be viewed);
- the layout records for the same scenes, as numeric dumps and as text rasters;
- the checks of the plan's §5.5, the principles of §5.1, and
  `design/projects/view-reviews/04-aesthetics.md`.

It must open every frame and look at it. It returns, for each check, **pass**, **fail** or **cannot
tell**, with the frame and the place in it; and separately anything that looks wrong and is not on
the list. It changes no code. A **fail** blocks the milestone. A **cannot tell** means the scene was
wrong: capture a better one and ask again. Record its verdict in the status file.

The checks the plan marks as automatable are tests in the tree as well; the inspector is for what a
test cannot see and for confirming that what the tests assert is what is on screen.

## 6. Gates

A package is done when its gate is green, its tests are in the right directory, its settings and
chords are specified rather than hard-coded, and the status file says so.

| Change                                                                        | Gate                                                                                                                                                                                              |
| ----------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| The relocation, the rename, and anything touching a device or the application | `make -j$(nproc)`; `make test` (read the PASSED lines); `PATH=/tmp/cf19/bin:$PATH make format-check lint` (no new findings); `xvfb-run … ./tools/compare-backends.sh` with its captures inspected |
| Engine packages (plan tracks E and P)                                         | `make -j$(nproc) lib xuzz_test xudu_test`; both test binaries headless; format and lint                                                                                                           |
| Header-only library packages                                                  | `make -j$(nproc) gleditor_test`; the binary headless; format and lint                                                                                                                             |
| Device-level library packages                                                 | the full gate, with the new scene                                                                                                                                                                 |
| Every package                                                                 | no file under `apps/xudu/` or `apps/zigzag/`; no sentinel in a new API; no library test repeated outside `tests/lib/`                                                                             |
| Every milestone                                                               | the full gate; the red-team reviewer; the frame inspector; then your go or no-go, written in the status file with its reasons                                                                     |

A green fast gate proves the engine code is right by its own tests and nothing more. Do not report a
feature as working until a presentation draws it and the inspector has seen it.

## 7. The order of work

The plan's §4 has every package, its dependencies and its tests. This is the order, with the owner's
decisions applied.

### Phase 0: the relocation, alone

Do this first and by itself. `design/projects/view-reviews/03-build.md` has the line-by-line recipe.

1. Move every file in `apps/xudu/` to `apps/common/ui/xanadoc/` and every file in `apps/zigzag/` to
   `apps/common/ui/slice/`, with `git mv`. **Nothing goes to `apps/common/xanadu/`**: every one of
   these files needs the library, and the engine is linked by ten targets that do not link it.
1. Rewrite the `"xudu/…"` and `"zigzag/…"` include spellings across `apps/`, `tests/` and `tools/`.
1. In the Makefile remove `XUDU_SRCS`, `ZIGZAG_SRCS` and their object lists, and fix `ALL_OBJS`, the
   `xuzz` link line and the `zigzag_test` link line. The `apps/common` source lists are recursive
   globs and need no edit.
1. Fix `packaging/wasm/build.sh`, which still assumes the old directories, and the stale check for
   `zigzag.html` in `.github/workflows/packaging.yml`. Say in the commit message that the check was
   already stale, so the breakage is not blamed on the move.
1. Update every path cited in `AGENTS.md`, `README.md`, the skills, `tools/code-quality-audit.py`
   and `tools/check-ui-text-policy.py`.
1. Verify: `find apps/xudu apps/zigzag -type f` prints nothing (the globs would silently go on
   compiling a file left behind); the full gate; the same four test binaries pass the same number of
   tests as before the move.

One commit, behaviour-preserving. Then, as a second commit, rename `zigzag_test` to `ui_test` and
`tests/zigzag/` to `tests/ui/` everywhere (Makefile, CI, `AGENTS.md`, the README), and run the full
gate again. Push. Report the before and after test counts.

### Phase 1: the spikes

Every spike in the plan's §3, before any package it gates. Run them in parallel through spike
runners. Record each result in the status file with the decision it forces. Three matter most:

- **R1**, the cost of a draw per plane, decides how `ui::PlaneSet` batches.
- **R4**, whether a page keeps a matrix through a reflow, establishes the test that the page-pose
  work must first make pass.
- **S1**, that `release()` is constant-time with the guard, is the claim the view space rests on.

If a spike fails its criterion, stop that track, write what you found and what you propose into the
status file and the spec, and continue the tracks it does not gate.

### Phase 2: the first release

Milestones as the plan numbers them. Tracks E (engine, slices), P (engine, pages) and L (library)
are independent of each other and can be given to separate implementers.

- **The spine.** Extract the seam the application uses on the visualizer; the view space, the
  binding model, the records and the raster; stretch vanishing with ghosts; the new slice
  presentation beside the old one, chosen from the palette, the old one the default; a compass that
  displays; binding by a typed command; sub-views in the palette. Nothing is present without a way
  in.
- **Packs.** Lanes, the lane table, glue, strands and the spread; groups made and bound by command.
- **The wheel.** The library's unprojection, depth write, placed planes and the beam fix; all-dim
  walk at depth 0; drag to an axis and to the compass.
- **Pages.** Split a page's flow matrix from its pose first, with no behaviour change; then the base
  view at parity with today, then page by page.
- **Choosing dimensions.** The selector's second and third tiers, "most used", and binding in three
  keys. Record walk summaries in the activity store now, in whatever shape fits.

That is the first release. Stop there and report before going on.

### After the first release

Decks and windowed pages; panes and scenes, which is where the application is rewired to the host;
then the cut-over. Deferred until asked: the `u` and `t` roles, edge heat, neighbours' wheels,
nested packs, keeping a pack, the Markov order, the selector's first tier, the stagger search,
embedding. The plan's §4.1 has the cost of each.

### The cut-over

Nothing legacy is deleted by default. When the new views pass every journey the old ones pass, the
plan's §5.5 checks hold, and the frame-time probes are no worse, you may decide **go**, write the
decision and its evidence into the status file, and delete. If not, decide **no-go**: the legacy
presentation stays the default and nothing is removed.

## 8. Keeping the documents true

- The spec is the acceptance test. If the code and the spec disagree, one of them is wrong; fix the
  wrong one in the same commit, and if it is the spec, add a line to its change history.
- Each item of the plan's §2 is absorbed into the spec or the rendering plan by the package that
  carries it. When the last one is absorbed, remove the "Amendments pending" list from the rendering
  plan.
- When a spike or a package shows the plan is wrong, change the plan, say why in its change history,
  and keep going.
- When a package moves or renames something `AGENTS.md` names, update `AGENTS.md` in that commit.

## 9. Git

- Work on the branch your session was given. One package, one commit; a commit message that says
  what changed and why, in the style of `git log`.
- Push after every green gate, with `git push -u origin <branch>`.
- Do not open a pull request unless asked. Never force-push. Never rewrite history that is pushed.
- Regenerate binary fixtures in the same commit as any change to `CompactOpNode`'s layout. Nothing
  in this project should need that; if you find you do, stop and say so.

## 10. When to stop and ask

Ask the owner only for what is the owner's: a missing system dependency or kernel module you cannot
supply, a permission you do not have, or evidence that one of the decisions in §2 is wrong. For
everything else, decide, write the decision down in the status file, and continue.

## 11. Your first reply

Before changing anything, reply with: what you read; the state of the environment (does the tree
build, how many tests pass in each binary, which gates are already red and why); and your plan for
this session, which should be Phase 0 and as much of Phase 1 as fits.
