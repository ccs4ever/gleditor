# Start the spatial UI overhaul

Paste the prompt below into a fresh cloud session, or ask it to read and execute this file. It is a
handoff for native Xuzz implementation, based on the spatial design and journey representations
committed with this document.

______________________________________________________________________

Implement the spatial UI overhaul in `https://github.com/ccs4ever/gleditor.git`. Fetch and start
from `origin/design/spatial-ui-overhaul-2026-10-13`, confirm this handoff and the J17–J32
prototype/review are present, and create an implementation branch such as
`feat/spatial-ui-overhaul`. This design branch contains the complete handoff; it is based on `main`
at `c7f70cc757034c006a6ce18d2a11e9c962aecce4`. Inspect subsequent `main` changes and incorporate
relevant updates deliberately. If an implementation branch or worktree already exists, inspect its
progress before deciding whether to continue it. The older plan mentions
`feat/3d-spatial-hypermedia` and `.worktrees/3d-spatial-hypermedia`; their existence is historical,
not a guarantee in this cloud session. Preserve unrelated work.

This is an instruction to begin implementing and verifying working native functionality. After
inspecting the code and recording the dependency plan, proceed to the first runnable increment. Keep
advancing through tested increments; maintain a repository status file so another session can
continue precisely where this one stops.

## Read and inspect first

Read these sources before changing product code:

1. `AGENTS.md`, `.agents/rules/architectural_governance.md`, and the relevant README sections.
1. `.agents/skills/xuzz-ui-design/SKILL.md` and `.agents/skills/ux-validation/SKILL.md`. Follow the
   independent UX/aesthetics review and headless validation workflows they describe.
1. `design/3d-spatial-hypermedia-navigation-implementation-plan.md`,
   `design/view-system-implementation-plan.md`, `design/view-system.md`, and
   `design/world-space-rendering-plan.md`. Reconcile their packages and current code; retain the
   existing M0–M6 boundary and the Track H follow-on gates. Reuse the placed-plane, page-pose,
   render-region and view-host seams appropriate to the implemented milestone.
1. `design/ui_workflow_xuzz_navigation.md`, `design/store-slice-convergence.md` (especially R8),
   `design/ux_workflow_real_work.md` (J1–J16), and `design/ux_workflow_spatial_concepts.md`
   (J17–J32).
1. `design/prototypes/spatial-journeys-2026-10-13/README.md`, `contracts.md`, the three JSON packs
   under `packs/`, and the complete `evidence/index.html` gallery and route traces.
1. `design/reviews/spatial-concept-journeys-2026-10-13-rerun/report.md`, `coverage.md`,
   `results.json`, and the independent Luna reports. The earlier static review remains an audit
   baseline, not the final representation status.
1. Inspect the actual images and concept/prompt notes under `design/mockups/`, particularly:
   - `hypertime-loom-2026-10-12/` and `hypertime-loom-journeys-2026-10-12/`;
   - `link-forge-pouches-2026-10-12/`;
   - `store-telescope-2026-10-13/` and `omni-search-2026-10-13/`;
   - `spatial-elements-2026-10-13/` and `investigation-workspace-2026-10-13/`.

Follow repository-local instructions if these skill paths have moved. Inspect what is actually wired
in `apps/common/xanadu/`, `apps/common/ui/`, `apps/xuzz/`, the system documents, shaders, automation
and tests. Documents marked unbuilt are proposals; establish their current status from code and
evidence. Record discrepancies before depending on them.

## What the design evidence establishes

All 16 new journeys are represented: 86 base tasks, 49 alternate routes, 370 named transitions, 435
captured before/after states and 144 cross-journey representation checks. Three Luna runners
authored independent packs and reviewed one another's journeys. Representation and browser checks
finished with zero failures.

The HTML prototype is a scripted design oracle. Its search results, acquisition, verification, media
playback, lifecycle, authoring and commit outcomes are simulated. Its native browser focus, layout
and accessibility-tree evidence applies to that page. Build the corresponding behavior through real
Xuzz models, commands and services, with live validation. Do not transplant the fixture patch engine
or report its success as a product journey pass.

The intended language combines a readable anchored page with quiet liquid-glass tools, physical
depth, silver rims, hinged wings, bowed conduits, coiled history and bounded occurrence stacks.
Inspect both quiet and expanded investigation frames. Preserve the existing semantic palette and
non-color cues: stored ancestry, same-operation correspondence, filtered order and actual shared
source addresses have distinct meanings. Gold is reserved for actual sharing, not textual, numeric
or semantic similarity. Optical effects stay outside readable glyph backing.

## Contracts the implementation must preserve

- Select and inspect retain the reading place. Explicit Enter records one completed visit in the
  reader's activity store. Back/Resume restore an existing visit without adding one. Filtering,
  camera movement, folding, tugs and cues append no operations to visited sources.
- Carry authority/store, exact object/state, ordered disjoint ranges and gaps, occurrence and media
  locator separately. Missing historical data never falls back silently to the latest state. One
  Link retains its identity and both complete ordered endsets, with independent member and
  occurrence cursors. Crossing previews; entering travels.
- Omni-search declares match fields, media, local/remote origins, versions/branches, rules, units
  and coverage. Reject stale responses; retain an excluded selection with explicit recovery
  controls. Distinguish partial-zero, completed-empty, unsupported, locked and unavailable. Literal,
  whole-token, exact-number and related-idea modes remain explicit. Recipes and captured result
  snapshots are separate; refresh never silently changes curated membership.
- Acquisition pins an immutable publication/manifest/edition/request. Signature, payload validity,
  identity trust and availability remain separate. Moving publisher pointers never replaces an
  in-flight pin. Retain, enable and emphasis are separate package choices.
- Typed pouch references preserve their dispatch and exact identity. Whole-cell, operation, query,
  walk and draft cards are not implicitly converted into content endpoints. Exact duplicates, equal
  text at different addresses and repeated occurrences have distinct handling. Every counted item
  and branch remains individually reachable in a synchronized list.
- Forge and recovery pin destination, branch/base, author and payload. Preview is separate from
  authoring. Follow the proposed preflight/private-staging/validation/publication contract and
  stable attempt-token reconciliation: unknown outcomes block blind retry, status-created opens the
  existing receipt, and only confirmed-uncommitted permits same-token retry. Establish actual
  backend guarantees before presenting an operation as atomic. Recovery retains chosen base content
  and original futures, transcluding only selected exact historical material.
- Media cues retain separate clocks and locators, with explicit playback and honest unsupported seek
  handling. Saved viewing cues are distinct from authored Links.
- Loom parents come from stored ancestry. Additional presentations retain the same operation
  identity; filtered order is not a parent chain. Own, contained and affected-topology scopes are
  distinct. Effects use the owning frame and parent-state footprints. Captured page contents retain
  their snapshot/ranges/layout despite later reflow.

Use `contracts.md` for the detailed proposed semantics. If a contract conflicts with a real engine
invariant or requires a missing capability, document the issue and implement its prerequisite; keep
incomplete actions explicitly unavailable. Never replace a missing service with simulated success or
silently weaken the journey.

## Delivery order

Create `design/projects/spatial-ui-overhaul-status.md` with the baseline commit, code inventory,
dependency map, package order, journey-to-command map and validation gates. Coordinate D1–D5, S1–S3
and Track H with the existing view milestones rather than creating a second view or camera
architecture.

First deliver a native vertical increment: a shared exact selection/reference model, a quiet and
expanded investigation workspace, a retained Master Link Tablet with endset/member/occurrence
browsing, explicit Enter/Return, reader activity restoration and list/keyboard equivalents. Use the
existing services where valid; prioritize the relevant J22/J24/J25 steps and report which steps are
actually implemented. Capture real application frames before polishing additional optics.

Continue with the dependency-backed increments: telescope acquisition and package choices;
cross-media/version/store search and comparison; durable typed collections and dense inventory
access; forge and recoverable authoring; media cues; Loom enumeration/effect/content lenses and
recovery. Shared prerequisites may land earlier. Connect every visible action to the same real
command boundary used by pointer, keymap and accessibility input.

Keep the old navigation path available until equivalent live journeys and backend frame reviews
pass. Settings and capability gates may support gradual cutover, but a hidden command is not a user
affordance. Performance budgets must come from measurements, not mockup fixture counts.

## Architecture and verification

Respect repository layering: generic rendering/picking support belongs in `src/` and
`include/gleditor/`; graphics-free xanalogical services in `apps/common/xanadu/`; presentation/input
in `apps/common/ui/`; program wiring in `apps/xuzz/`. Preserve plain-editor isolation. Reuse Vortex
and its standard library for actions; justify new C++ at the appropriate low-level boundary.
Bindings belong in `system://keymap`; tunables and theme/metrics come from the governed system
documents and registered settings. Keep preparation outside draw and bound visible work.

Initialize submodules with `git submodule update --init --recursive`; build with `make -j$(nproc)`
and run the relevant owning tests and required gates. All display/audio work is headless. Outside
make set all six variables, using a fresh directory for each validation run:

```sh
spatial_run_dir=$(mktemp -d)
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1
export XDG_DATA_HOME="$spatial_run_dir/data" XDG_CONFIG_HOME="$spatial_run_dir/config"
export XDG_CACHE_HOME="$spatial_run_dir/cache"
```

Use real UI controls/default chords and the existing headless input path for graphical journeys; fix
harness gaps through that same event boundary before judging a task. Retain native screenshots,
accessibility dumps and before/after store/permascroll evidence. Validate relevant J17–J32 tasks and
preserve J1–J16; test offline/partial/stale/cancel/unknown-outcome alternatives, keyboard/list,
narrow/enlarged text and reduced motion. Run the required backend comparisons after drawing changes
and inspect the resulting frames. Keep tests with their owning code; run format/lint gates and
separate pre-existing/environment failures from regressions.

You are authorized to use parallel implementers on disjoint packages and independent UX/aesthetic
reviewers. Use three Luna runners for journey validation where available, with independent run
directories and rotated assignments; obey the skills' concurrency limits. Validators edit no product
code during a run. They judge native captures and transcripts, not implementation claims. The lead
resolves findings and owns integration.

Commit each tested increment and push the feature branch. Do not merge to `main` or deploy as part
of this implementation session. Update the status file in every implementation commit with the
package, implemented journey steps, evidence paths, checks, remaining gaps and exact next task.
Finish each session with working code, a clean checkpoint and a clear account of what runs natively
and what remains planned. Begin now with the inventory and first increment.
