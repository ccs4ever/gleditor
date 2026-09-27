---
name: xuzz-ux-validation
description: >-
  Validate, from the user's seat, that xuzz, vquery, vpl and vprolog can be used for real work
  through their interfaces: creating documents and slices, switching views, following links to
  distant cells, transcluding content, querying stores, calculating over numeric cells, making
  rule-based decisions, passing data between programs, and resuming work. Use to audit the user
  experience or to verify a UX fix; use the xudu, ZigZag or link-navigation skills to design or
  build those features.
---

# Real work UX validation

The contract is [`design/ux_workflow_real_work.md`](../../../design/ux_workflow_real_work.md):
fourteen journeys (J1–J14), what counts as reaching each step through the interface, and the
evidence a pass needs. Read it first. This skill says how to run it. A validation reports what the
build does; code that looks as if it should work is not a pass.

## Rules

- **Through the interface only.** Graphical steps go through a menu, an on-screen control, a
  `system://keymap` default binding or the pointer. Terminal steps use the program's visible REPL
  prompt. Launching a program with a store path is the only command line a journey may use. A step
  reachable only by a flag, a script or a source file prepared elsewhere is a *No affordance*
  finding, not a pass.

- **Headless always** (AGENTS.md). Use `make`'s environment or set it yourself, and give every run
  its **own** configuration and data directories: system xanadocs are read through the open
  document's permascroll, so a configuration shared across runs with different permascrolls silently
  loads no key bindings.

  ```sh
  run=$(mktemp -d)
  export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
    XDG_CONFIG_HOME=$run/config XDG_DATA_HOME=$run/data
  ```

- **The automation options are hands, not features.** They stand in for a person, and each goes
  through the path the matching real input takes. `--click X,Y`, `--select START,END`,
  `--type TEXT`, `--key NAME` (dialog keys), `--chord COMBO` (a key combination, through the key
  bindings), `--mouse-down X,Y[,BUTTON]`, `--mouse-move X,Y`, `--mouse-up X,Y[,BUTTON]`,
  `--drag X1,Y1:X2,Y2`, `--right-click X,Y`, `--capture FILE`, `--screenshot FILE` and
  `--dump-a11y`. Prefer `--chord` with the default binding to `--do NAME`; use `--do` only for an
  action whose binding is confirmed in `system://keymap` (run once with
  `SPDLOG_LEVEL=xudu.keymap=debug`, which logs each binding made and warns about any that do not
  parse), and record the chord. An action with no binding and no control is *No affordance* however
  well `--do` drives it.

- **Harness gaps are fixed first.** When a gesture a person can make has no automation, that is a
  *Harness gap*. Close it in the automation layer (`src/app.cpp`'s script options,
  `src/renderer.cpp`'s script steps), routing synthetic input through the same event path a real
  event takes, with a test, before judging the journey. Never judge a journey through a side door
  the user does not have.

- **Use a terminal session for REPL journeys.** Drive `vquery`, `vpl` and `vprolog` by typing at
  their prompts in a PTY and keep the full input and output transcript. Do not use `-e`, `--store`,
  `--output-store`, a pipe or a prepared source file to complete a journey step. These can be used
  separately to diagnose a finding, but their success is not a journey pass. Check that the answer
  uses the user's data and that a later prompt can continue the work.

- **Track every handoff.** For a journey crossing programs, retain the input and output artifacts,
  their source cell identities and record counts at each stage. A manually copied value is not
  evidence that the next program consumed the preceding program's result.

- **Evidence or it did not happen.** Keep, per graphical step, a captured frame and relevant
  accessibility dump; per terminal step, the REPL transcript including input, output and errors;
  and, after closing a store, `xudu-dump --section=ops --permascroll=<dir> <store>`. Inspect frames
  rather than trusting exit codes; a program that draws nothing still exits 0.

- **Validate, then fix.** A validation run changes no product code. Fixes are separate changes, each
  followed by rerunning the journeys it touches.

## Agentic workflow

For a full audit, assign independent roles when subagents are available. Run at most three subagents
concurrently, using `gpt-6-luna` for each delegated role. Queue remaining journeys and reviews until
a slot is free; the lead stays in the parent agent. Each subagent works in its own run directory and
edits no product code.

1. **Harness engineer (first, alone).** Inventory the automation (`xuzz --help-all`, the script step
   kinds in `src/renderer.cpp`) against every gesture J1–J14 needs, close the gaps, and prove each
   new graphical step with a test and a captured frame. Check that terminal sessions can capture
   REPL input and output. Hand the others the list of hands they now have.
1. **Journey runners (in parallel, one per journey or pair).** Before running, map each step to the
   affordance a user would find: a radial-menu entry, a command-bar command, a key binding, a
   pointer gesture or a REPL command. A step with nothing to map to is already a finding. Then run
   the journey headless, keeping evidence, and classify every step (Pass, Fail, No affordance,
   Missing, Harness gap).
1. **Persistence auditor.** Owns J5 and the save half of every journey: `xudu-dump` before closing
   and after reopening, the permascroll and system xanadocs, and whether caret, camera, slice focus,
   pouch items, selected link, calculated views and asserted rules come back.
1. **Keyboard and accessibility reviewer.** Repeats each passing graphical journey with the keyboard
   and the accessibility tree alone: every step reachable without the pointer, every control named,
   focus never lost. For REPL journeys, checks that commands, errors and solutions are readable in
   the terminal. Reports steps that pass only by mouse.
1. **Lead.** Merges the reports into one findings table, removes duplicates, ranks by how badly each
   blocks real work, and proposes fixes. A disagreement between roles is settled by rerunning the
   step and reading the evidence, not by argument.

For a single fix, run just the journeys it touches, directly, without subagents.

## Report

One table for the run, then the findings:

| Journey | Step | Affordance used (menu, key, pointer, prompt) | Outcome | Evidence |
| ------- | ---- | -------------------------------------------- | ------- | -------- |

For each finding that is not a Pass: what the user tried, what happened, the evidence path, and a
proposed fix with where in the code it lands. Say plainly what was not validated and why: a harness
gap left open, a journey skipped, a backend not tried.
