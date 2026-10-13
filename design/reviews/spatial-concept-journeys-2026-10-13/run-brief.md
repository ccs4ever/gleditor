# Parallel concept journey run: 2026-10-13

The user requested three Luna runners for J17–J32. The lead assigned the journeys in
[manifest.json](manifest.json), which freezes the journey text and 31 final/non-initial image assets
by SHA-256. All 86 base steps and each journey's cross-check matrix must be accounted for.

Mode: AI reviewer concept walkthrough using the supplied static artifacts. This is not a human
participant session or a live implementation run. Predictions are the runner's interpretations;
there is no observed human behavior, real input delivery or store/network execution.

## Rules for the runners

Read [the journey contract](../../ux_workflow_spatial_concepts.md), its fixture distinctions and
outcome definitions. Inspect each relevant PNG with the image-viewing tool. A filename or prompt is
not visual evidence. Read supporting concept notes to interpret the declared fixture, but record
when behavior is text-only. Work in your assigned directory and edit no product or fixture files.

For each base step, record the proposed visible action, interpretation/prediction, visual state
actually observed, missing transitions, primary concept outcome and exact evidence path/region. Do
not invent clicks, unaided participant behavior, network replies, persisted stores or generated
frames. Do not add state cards during the run; newly designed states would need a separate rerun.

Use Concept supported only for an explicitly represented subtask whose affordance and result can be
inspected. It is an AI visual-support assessment, not empirical usability acceptance. A base step
requiring an absent transition cannot receive that outcome merely because a button exists. Record
positive observations separately when the primary outcome is a gap.

Use Confusing for an actual visible ambiguity or contradiction. No affordance means the provided
concept lacks a discoverable action for a task, whereas Not represented means necessary state,
transition or input evidence is absent. State undefined means a policy or state transition remains
undecided. Distinguish these rather than classifying every missing behavior as an implementation
bug.

A primary outcome summarizes the whole base step conservatively. If multiple gaps apply, record each
variant as a separate finding with its outcome, including represented observations alongside it.
List every journey's nine cross-check repeats, and state what can and cannot be assessed. Static
images never prove keyboard, accessibility-tree, reduced-motion, runtime density or restart.

Preserve independent fixture identities, including the two-state shared-prefix prism versus the
integrated V12 pressure-removal example; both Walk fixtures; and initial package counts versus
choice-dependent retained/enabled counts. The protocol has no live Pass/Fail outcomes.

## Deliverables

Each runner writes `report.md` and `results.json` under its assigned directory. The JSON has:

- `runner`, `model`, `journeys`, `inspected_assets`;
- `steps`: one record per assigned base step with journey, step, outcome, affordance, prediction,
  observed, missing, represented_observations and evidence (path/region objects);
- `variants`: explicit additional tested/missing/error variants with the same outcome/evidence;
- `cross_checks`: all nine repeats per assigned journey, with outcome, observation and evidence;
- `findings`: unique IDs, severity (blocker/major/minor), journey/step/variant references, what was
  attempted, visible result or gap, evidence, proposed design change and readiness implication.

Do not report issue counts or supported totals that disagree with the JSON. Keep recommendations
concrete: which state card, visible action, policy or identity descriptor needs clarification. The
lead checks coverage, reconciles disagreements and deduplicates findings in the consolidated report.
Original runner reports remain as evidence.
