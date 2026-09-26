---
name: xuzz-ux-validation
description: >-
  Validate, from the user's seat, that xuzz and vquery can be used for real work entirely
  through their UI: creating xanadocs and slices, selecting and dragging text into new pages and
  pouches, moving between the home cell and documents, and closing and resuming a store. Use to
  audit the user experience or to verify a UX fix; use the xudu, ZigZag or link-navigation skills
  to design or build the features themselves.
---

# Xuzz UX validation

The contract is [`design/ux_workflow_real_work.md`](../../../design/ux_workflow_real_work.md): six
journeys (J1–J6), what counts as reaching each step through the UI, and the evidence a pass needs.
Read it first. This skill says how to run it. A validation reports what the build does; code that
looks as if it should work is not a pass.

## Rules

- **Through the UI only.** Every product step goes through a menu, an on-screen control, a
  `system://keymap` default binding or the pointer. Launching the program with a store path is the
  only command line a journey may use. A step reachable only by a flag, a script or a file is a *No
  affordance* finding, not a pass.

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

- **Evidence or it did not happen.** Keep, per step: a captured frame, the relevant slice of the
  accessibility dump, program output, and after closing,
  `xudu-dump --section=ops --permascroll=<dir> <store>`. Inspect frames rather than trusting exit
  codes; a program that draws nothing still exits 0.

- **Validate, then fix.** A validation run changes no product code. Fixes are separate changes, each
  followed by rerunning the journeys it touches.

## Agentic workflow

For a full audit, assign independent roles when subagents are available (keep to five or fewer
unless the user asks for more). Each works in its own run directory and edits no product code.

1. **Harness engineer (first, alone).** Inventory the automation (`xuzz --help-all`, the script step
   kinds in `src/renderer.cpp`) against every gesture J1–J6 needs, close the gaps, and prove each
   new step with a test and a captured frame. Hand the others the list of hands they now have.
1. **Journey runners (in parallel, one per journey or pair).** Before running, map each step to the
   affordance a user would find: a radial-menu entry, a command-bar command, a key binding, a
   pointer gesture. A step with nothing to map to is already a finding. Then run the journey
   headless, keeping evidence, and classify every step (Pass, Fail, No affordance, Missing, Harness
   gap).
1. **Persistence auditor.** Owns J5 and the save half of every journey: `xudu-dump` before closing
   and after reopening, the permascroll and system xanadocs, and whether caret, camera, slice focus,
   pouch items and selected link come back.
1. **Keyboard and accessibility reviewer.** Repeats each passing journey with the keyboard and the
   accessibility tree alone: every step reachable without the pointer, every control named, focus
   never lost. Reports steps that pass only by mouse.
1. **Lead.** Merges the reports into one findings table, removes duplicates, ranks by how badly each
   blocks real work, and proposes fixes. A disagreement between roles is settled by rerunning the
   step and reading the evidence, not by argument.

For a single fix, run just the journeys it touches, directly, without subagents.

## Report

One table for the run, then the findings:

| Journey | Step | Affordance used (menu, key, pointer) | Outcome | Evidence |
| ------- | ---- | ------------------------------------ | ------- | -------- |

For each finding that is not a Pass: what the user tried, what happened, the evidence path, and a
proposed fix with where in the code it lands. Say plainly what was not validated and why: a harness
gap left open, a journey skipped, a backend not tried.
