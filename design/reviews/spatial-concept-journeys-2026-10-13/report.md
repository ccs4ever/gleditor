# Parallel Luna Review: Spatial Concept Journeys J17–J32

Three gpt-6-luna runners completed independent visual walkthroughs of all sixteen journeys. The lead
reconciled evidence and classifications. These are AI expert reviews of static concepts, not human
participant sessions or live implementation tests.

No journey has all its required steps represented end to end. The current assets support useful
static interpretation tasks, but missing transitions and unresolved authoring rules prevent a
complete pre-implementation journey run. The next step is to resolve the authoring protocol and
prepare targeted state cards, rather than start the full implementation.

## Coverage and results

Covered 86 base steps, 37 explicit variants and 144 cross-check repeats. Runners inspected 24
distinct frozen PNGs. The specification and all 31 frozen non-initial image assets remained
unchanged. No product code, mockup or fixture was edited.

| Lead base-step outcome | Count |
| ---------------------- | ----- |
| Concept supported      | 13    |
| Not represented        | 66    |
| State undefined        | 5     |
| No affordance          | 1     |
| Confusing              | 1     |

Concept supported means the assigned AI runner found direct support for the represented task. It is
not an observed participant success or a live Pass. Partial positive observations remain in the step
records even when the whole step is classified Not represented.

| Repeat/variant set    | Assessments | Outcome counts                                                           |
| --------------------- | ----------- | ------------------------------------------------------------------------ |
| Cross-journey repeats | 144         | 18 Concept supported, 126 Not represented                                |
| Explicit variants     | 37          | 18 Not represented, 10 Concept supported, 7 State undefined, 2 Confusing |

Base steps, variants and repeated checks are different denominators; do not combine them into a
success rate. No keyboard, accessibility-tree, motion, cryptographic, networking, storage or
performance execution is inferred from a label in an image.

| Runner/model        | Journeys                     | Base steps | Variants | Repeats | Detailed report                |
| ------------------- | ---------------------------- | ---------- | -------- | ------- | ------------------------------ |
| Luna 1 / gpt-6-luna | J17, J18, J19, J20, J21      | 25         | 10       | 45      | [Runner 1](runner-1/report.md) |
| Luna 2 / gpt-6-luna | J22, J23, J24, J25, J26      | 26         | 15       | 45      | [Runner 2](runner-2/report.md) |
| Luna 3 / gpt-6-luna | J27, J28, J29, J30, J31, J32 | 35         | 12       | 54      | [Runner 3](runner-3/report.md) |

## Journey outcomes

| Journey | Task                                                               | Supported | Not represented | Undefined | No affordance | Confusing | Readiness                   |
| ------- | ------------------------------------------------------------------ | --------- | --------------- | --------- | ------------- | --------- | --------------------------- |
| J17     | Discover and acquire an exact remote edition                       | 2         | 3               | 0         | 0             | 0         | Further cards/policy needed |
| J18     | Find one term across media and store origins                       | 1         | 4               | 0         | 0             | 0         | Further cards/policy needed |
| J19     | Filter a running query without losing the selected result          | 0         | 5               | 0         | 0             | 0         | Further cards/policy needed |
| J20     | Search historical occurrences with visible matching rules          | 1         | 3               | 1         | 0             | 0         | Further cards/policy needed |
| J21     | Compare pinned states at text and topology changes                 | 0         | 5               | 0         | 0             | 0         | Further cards/policy needed |
| J22     | Investigate around a stable reading place                          | 3         | 2               | 0         | 0             | 0         | Further cards/policy needed |
| J23     | Control immutable link packages without editing their relations    | 1         | 4               | 0         | 0             | 0         | Further cards/policy needed |
| J24     | Keep one complete relation through distant and unavailable members | 1         | 4               | 0         | 0             | 0         | Further cards/policy needed |
| J25     | Preview and resume branching reader visits                         | 2         | 3               | 0         | 0             | 0         | Further cards/policy needed |
| J26     | Collect typed evidence without changing what it refers to          | 0         | 4               | 1         | 1             | 0         | Further cards/policy needed |
| J27     | Assemble one ordered Link with an explicit destination             | 0         | 4               | 1         | 0             | 0         | Further cards/policy needed |
| J28     | Resume a dense draft through unresolved and unknown outcomes       | 0         | 4               | 2         | 0             | 0         | Further cards/policy needed |
| J29     | Align mixed-media cues without authoring a relation                | 2         | 3               | 0         | 0             | 0         | Further cards/policy needed |
| J30     | Investigate exact range and topology effects in the Loom           | 0         | 7               | 0         | 0             | 0         | Further cards/policy needed |
| J31     | Recover a selected historical passage into a declared new branch   | 0         | 5               | 0         | 0             | 1         | Further cards/policy needed |
| J32     | Resume an interrupted investigation offline with equivalent access | 0         | 6               | 0         | 0             | 0         | Further cards/policy needed |

The clearest represented ideas are separate verification labels, mixed-media evidence badges,
current reading place versus inspected version, complete Link context, enablement versus emphasis,
branching reader visits, and separate clocks/manual media cues. This does not waive their missing
action-result states, exact descriptors or later runtime checks.

## Prioritized design findings

21 runner findings are grouped into 16 themes below. Two concern critical authoring readiness; the
remaining themes distinguish missing cards, semantic decisions and current-frame clarity. This is a
review backlog, not a list of observed implementation failures.

| Finding | Priority | Required next design work                                                 |
| ------- | -------- | ------------------------------------------------------------------------- |
| F01     | blocker  | Define forge commit, failure and unknown-outcome handling                 |
| F02     | blocker  | Complete the recovery authoring review and validation states              |
| F03     | major    | Show the pinned acquisition lifecycle and pointer advance                 |
| F04     | major    | Expose exact result origins and media evidence locators                   |
| F05     | major    | Represent filter, stale response, coverage and saved-query transitions    |
| F06     | major    | Declare numerical/history matching rules and their result states          |
| F07     | major    | Extend comparison beyond its readable two-state scalar example            |
| F08     | major    | Show link/visit transitions and restoration without identity loss         |
| F09     | major    | Make carrier update, verification and disabled-but-pinned choices visible |
| F10     | major    | Complete typed pouch actions, duplicate policy and forge eligibility      |
| F11     | major    | Show forge staging, ordered-member edits and dense draft restoration      |
| F12     | major    | Complete manual cue editing, save/reopen and separate relation handoff    |
| F13     | major    | Enumerate Loom expansion, effect scope and exact preserved descriptors    |
| F14     | major    | Supply lifecycle and equivalent-input representations                     |
| F15     | minor    | Specify accepted reading bounds when tools unfold                         |
| F16     | minor    | Resolve three visible wording and count ambiguities                       |

### F01. Define forge commit, failure and unknown-outcome handling

Priority: blocker. Sources: R3-01. Journeys/steps: J27/5, J28/4, J28/5.

The forge/draft images stop at an uncommitted bench. No attempted-commit identity, partial-write
ledger, Check status result or safe retry resolution is represented; the failure/retry protocol is
still undecided.

Next: Decide the declared authoring mode and recoverable/idempotent protocol. Prepare separate
preflight-failed, mid-commit-failed, outcome-unknown, status-confirmed-created and
confirmed-uncommitted cards with exact destination and returned Link identity. Retain the draft and
never infer that retry is safe from a missing success message.

Resolve the protocol before the relevant authoring implementation; then review its cards. Cards
cannot prove atomicity or duplicate prevention.

Evidence:
[02-disjoint-link-forge.png](../../mockups/link-forge-pouches-2026-10-12/02-disjoint-link-forge.png);
[04-reopen-draft.png](../../mockups/link-forge-pouches-2026-10-12/04-reopen-draft.png). Exact image
regions are retained in the source step records.

### F02. Complete the recovery authoring review and validation states

Priority: blocker. Sources: R3-07. Journeys/steps: J31/3, J31/4, J31/5, J31/6.

The existing recovery preview names source A V27, base B V31 and retained future B V40 correctly.
Author identity/validation, source unavailable, advancing-head choice, cancel result, commit result
and reopened branch identity are not represented.

Next: Add a full source/base/author/effect inspector and critical validation/cancel/returned-branch
cards. Keep pinned V31 distinct from any advancing branch head, and preserve the old futures. This
is selected content recovery, not arbitrary operation replay or automatic merge.

Complete critical authoring review states before recovery implementation. Do not reinterpret the V40
comparison tip as the V31 write base.

Evidence:
[06-branch-recovery.png](../../mockups/hypertime-loom-journeys-2026-10-12/06-branch-recovery.png).
Exact image regions are retained in the source step records.

### F03. Show the pinned acquisition lifecycle and pointer advance

Priority: major. Sources: R1-01. Journeys/steps: J17/3, J17/4, J17/5.

The telescope distinguishes signature, trust, integrity, unchecked availability and
interrupted/ready states, but lacks V12-request/V13-pointer, invalid/unavailable, retry/cancel
results, explicit arrival and offline-reopen cards.

Next: Prepare separate lifecycle cards retaining publisher/publication/edition and showing the
independent follow/layer choices. Keep optional preview, payload validation and independent identity
trust separate.

A button and Ready offline label do not establish acquisition or persistence; these are prototype
gaps.

Evidence: [01-discovery.png](../../mockups/store-telescope-2026-10-13/01-discovery.png);
[02-acquisition.png](../../mockups/store-telescope-2026-10-13/02-acquisition.png). Exact image
regions are retained in the source step records.

### F04. Expose exact result origins and media evidence locators

Priority: major. Sources: R1-02. Journeys/steps: J18/2, J18/3, J18/5.

Mixed-media matching is readable, but store authority, image-region descriptor, transcript end
locator and independent equal-text versus true shared-source inspection are incomplete. The
entered/returned occurrence states are absent.

Next: Add type-specific result inspectors with store/object/version/range or time/region locator,
extraction provenance and availability. Include equal-text/different-address and shared-address
examples plus entered/returned states.

Resolve the exact evidence descriptors before a unified media-result model is built.

Evidence: [01-omni-search.png](../../mockups/omni-search-2026-10-13/01-omni-search.png). Exact image
regions are retained in the source step records.

### F05. Represent filter, stale response, coverage and saved-query transitions

Priority: major. Sources: R1-03. Journeys/steps: J19/2, J19/3, J19/4, J19/5.

A selected result, filters and partial counts exist, but excluded selection, current request
ownership, stale response, unsupported surfaces, retained cancellation, empty/completed versus
incomplete-zero and recipe/snapshot/rerun states are missing.

Next: Prepare these result-lifecycle cards and show one retained exact selected occurrence across
each transition. Keep saved recipes, captured results and curated collection membership separate.

The required retention behavior is already declared; missing frames are not evidence of a live
search failure.

Evidence: [01-omni-search.png](../../mockups/omni-search-2026-10-13/01-omni-search.png). Exact image
regions are retained in the source step records.

### F06. Declare numerical/history matching rules and their result states

Priority: major. Sources: R1-04. Journeys/steps: J20/2, J20/3, J20/4, J20/5.

The rule controls are visible and the integrated pressure V8/V12 comparison already exists. Numeric
normalization/units and variant results, explicit query-switch retention, state-run expansion and
missing/completed coverage are not adequately represented.

Next: Choose and display literal/token/numeric/unit/related-idea rules, then add included/excluded
3.14159/3.140/pi states, a query-switch card linked to the existing pressure state,
occurrence/state/op count expansion, and saved recipe/result states.

Numerical matching rules need an explicit decision; other gaps are missing result/transition cards.

Evidence:
[03-content-search-v2.png](../../mockups/hypertime-loom-journeys-2026-10-12/03-content-search-v2.png);
[02-expanded-investigation.png](../../mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png).
Exact image regions are retained in the source step records.

### F07. Extend comparison beyond its readable two-state scalar example

Priority: major. Sources: R1-05. Journeys/steps: J21/1, J21/2, J21/3, J21/4, J21/5.

The independent shared-prefix prism is clear. Its independently equal-text variant, three-state
ancestry, reciprocal topology/membership neighborhoods, unavailable region and
entered/saved/reopened outcomes are absent.

Next: Prepare the declared independent comparison fixtures separately, with exact state descriptors,
change-site neighborhoods and an unavailable target that is never replaced.

The two-state picture supports the composition, not the complete comparison journey.

Evidence:
[02-comparison-prism.png](../../mockups/spatial-elements-2026-10-13/02-comparison-prism.png). Exact
image regions are retained in the source step records.

### F08. Show link/visit transitions and restoration without identity loss

Priority: major. Sources: R2-01, R2-03, R2-06. Journeys/steps: J22.4, J22.5, J23.5, J24.2, J24.3,
J24.4, J25.2, J25.4, J25.5, J26.5.

Current versus preview, complete 2-by-3 endsets and both retained route graphs are visible. Cursor
changes, overlapping-Link sessions, pending/cancel/retry, distant arrival, restore, notes and
continued sibling futures lack resulting states.

Next: Add state cards retaining Link ID, both cursors, exact occurrence, current visit and origin.
Distinguish new reselect origin 03 from restored origin 01. Include the leaf Back/Forward behavior,
unavailable target, reference/note result and reopen cards.

Retained identity is the stated contract; the walkthrough does not prove traversal or activity
storage.

Evidence:
[02-expanded-investigation.png](../../mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png);
[01-walk-compass.png](../../mockups/spatial-elements-2026-10-13/01-walk-compass.png);
[04-layer-console.png](../../mockups/spatial-elements-2026-10-13/04-layer-console.png);
[01-typed-pouch.png](../../mockups/link-forge-pouches-2026-10-12/01-typed-pouch.png). Exact image
regions are retained in the source step records.

### F09. Make carrier update, verification and disabled-but-pinned choices visible

Priority: major. Sources: R2-02. Journeys/steps: J23.2, J23.4, J23.5, J24.5.

Package enablement and emphasis are distinct. Full carrier/trust inspectors, the unchecked/invalid
policy, explicit candidate #13 choice, choice-dependent counts and disabled-but-pinned L17 states
are absent.

Next: Add package inspector and keep/switch/result cards. Inspecting does not enable. Derive
retained/enabled counts from actual chosen carriers, and keep a selected disabled Link inspectable
until explicit dismissal.

Define verification policy and provide the missing choices before testing package update behavior.

Evidence: [04-layer-console.png](../../mockups/spatial-elements-2026-10-13/04-layer-console.png);
[02-expanded-investigation.png](../../mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png).
Exact image regions are retained in the source step records.

### F10. Complete typed pouch actions, duplicate policy and forge eligibility

Priority: major. Sources: R2-04. Journeys/steps: J26.1, J26.2, J26.3, J26.4, J26.5, J26.6.

Typed rows and 146/18/6 counts are clear; type-specific inspectors, off-window list access,
save/restore, duplicate/address policy, dismissal recovery, unavailable states and eligible-endpoint
chooser are missing.

Next: Add inspectors and typed save receipts; a discoverable searchable list/continuation path;
exact-duplicate versus equal-text/shared-source choices; recoverable dismissal; and an explicit
span-eligibility chooser. Whole-cell/query/op/walk/draft cards are not automatic span endpoints.

Duplicate policy remains a design decision. Off-window list access is the remaining absent
affordance in this run.

Evidence: [01-typed-pouch.png](../../mockups/link-forge-pouches-2026-10-12/01-typed-pouch.png).
Exact image regions are retained in the source step records.

### F11. Show forge staging, ordered-member edits and dense draft restoration

Priority: major. Sources: R3-02, R3-03. Journeys/steps: J27/1, J27/2, J27/3, J27/4, J28/3, J28/6.

The disjoint two-left/three-right draft and gap are represented. Selection-to-stage,
undo/group/order/occurrence effects, destination retention during browsing, relation preview, dense
reorder/true ordinal and keep/discard/reopen outcomes are absent.

Next: Prepare exact descriptor before/after cards, retaining source gaps, ordered members and pinned
store/branch/base. Include left member 21 with five occurrences in the 40/60 fixture and full
retained membership.

The illustrative 40/60 state is not a scalability claim or proof of list navigation.

Evidence:
[02-disjoint-link-forge.png](../../mockups/link-forge-pouches-2026-10-12/02-disjoint-link-forge.png);
[03-dense-endsets.png](../../mockups/link-forge-pouches-2026-10-12/03-dense-endsets.png);
[04-reopen-draft.png](../../mockups/link-forge-pouches-2026-10-12/04-reopen-draft.png). Exact image
regions are retained in the source step records.

### F12. Complete manual cue editing, save/reopen and separate relation handoff

Priority: major. Sources: R3-04. Journeys/steps: J29/3, J29/4, J29/5.

The media faces, separate clocks and derived transcript are readable. Cue-adjust/add/remove results,
saved descriptor/reopen, unavailable/seek errors and forge eligibility are absent.

Next: Add cue result cards retaining native locators/time bases and explicit playback. Save reader
cues separately from authored Links; the Forge action opens an eligibility-reviewed draft.

No playback accuracy or media persistence was measured.

Evidence:
[03-media-cue-bridge.png](../../mockups/spatial-elements-2026-10-13/03-media-cue-bridge.png). Exact
image regions are retained in the source step records.

### F13. Enumerate Loom expansion, effect scope and exact preserved descriptors

Priority: major. Sources: R3-05, R3-06. Journeys/steps: J30/1, J30/2, J30/3, J30/4, J30/5, J30/6,
J30/7.

The 100 + 3-by-20 = 160 fixture exists with three fork locations, and the ten-path fixture is
separately labelled. Missing expansion/list exit access, scope/rule results, effect
owner/parent/ranges and exact captured page/topology descriptors prevent completing J30.

Next: Use the existing 07-scenario frame rather than inventing a replacement. Add one expanded coil
and synchronized exit list, exact-range/evolving rules, parent-state effect metadata,
partial/cancel/save/completed-empty cards and descriptor inspectors.

The pictured density and topology diagrams do not certify enumeration completeness or
rendering/query budgets.

Evidence:
[07-scenario-1-trunk-branches.png](../../mockups/hypertime-loom-2026-10-12/07-scenario-1-trunk-branches.png);
[10-scenario-3-ten-branches-v2.png](../../mockups/hypertime-loom-2026-10-12/10-scenario-3-ten-branches-v2.png);
[02-container-thread-tug.png](../../mockups/hypertime-loom-2026-10-12/02-container-thread-tug.png);
[02-span-effects.png](../../mockups/hypertime-loom-journeys-2026-10-12/02-span-effects.png);
[04-cell-topology.png](../../mockups/hypertime-loom-journeys-2026-10-12/04-cell-topology.png);
[05-page-footprint.png](../../mockups/hypertime-loom-journeys-2026-10-12/05-page-footprint.png).
Exact image regions are retained in the source step records.

### F14. Supply lifecycle and equivalent-input representations

Priority: major. Sources: R3-08. Journeys/steps: J32/1, J32/2, J32/3, J32/4, J32/5, J32/6.

No offline save/reopen, corrupt-data, reconnect, keyboard-focus, narrow/enlarged/reduced-motion or
actual accessibility/motion evidence is supplied. This affects every journey, not only J32.

Next: Prepare proposed session descriptors and state cards for the critical lifecycle cases, plus
keyboard/list focus maps and alternate layouts. Later validate real input, cache/store effects,
rendering and restart separately.

Static mockups cannot prove these repeats; all nine repeats were nevertheless assessed per journey.

Evidence:
[02-expanded-investigation.png](../../mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png);
[01-walk-compass.png](../../mockups/spatial-elements-2026-10-13/01-walk-compass.png);
[03-dense-endsets.png](../../mockups/link-forge-pouches-2026-10-12/03-dense-endsets.png). Exact
image regions are retained in the source step records.

### F15. Specify accepted reading bounds when tools unfold

Priority: minor. Sources: R2-05. Journeys/steps: J22.2, J22.4.

The quiet/expanded pair preserves the reading text and current visit but shifts the page
size/position slightly.

Next: Prepare a shared-host-bound pair or declare the accepted reflow. Keep reading offset and
source companions stable; represent a reduced-motion cut before testing actual animation.

This is a visual continuity limitation, not evidence of a completed visit or source change.

Evidence:
[01-quiet-reading.png](../../mockups/investigation-workspace-2026-10-13/01-quiet-reading.png);
[02-expanded-investigation.png](../../mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png).
Exact image regions are retained in the source step records.

### F16. Resolve three visible wording and count ambiguities

Priority: minor. Sources: R1-01, R1-06, R3-09. Journeys/steps: J17/3, J17/4, J17/5, J20/1, J31/6.

The acquisition heading says transfer is active while the detail says interrupted. Historical
results say 2 shown of 3 beside three result-like cards. Recovery comparison says 1 difference while
listing additional wording/reorder categories without stating their count scope.

Next: Make the transfer heading consistent; label result versus context membership and reconcile
shown counts; declare whether comparison counts summarize selected-passage differences or all listed
categories. Keep comparison delta separate from the proposed recovery write.

These are present-frame clarity findings; no runtime count or transfer behavior was exercised.

Evidence: [01-discovery.png](../../mockups/store-telescope-2026-10-13/01-discovery.png);
[02-acquisition.png](../../mockups/store-telescope-2026-10-13/02-acquisition.png);
[03-content-search-v2.png](../../mockups/hypertime-loom-journeys-2026-10-12/03-content-search-v2.png);
[06-branch-recovery.png](../../mockups/hypertime-loom-journeys-2026-10-12/06-branch-recovery.png).
Exact image regions are retained in the source step records.

## Lead reconciliation

The lead checked actual images and the journey contract before accepting the findings:

- The 100-op trunk/three 20-op branches are already drawn in the scenario-1 image. The remaining gap
  is expansion/list access, not absence of that fixture.
- Draft B V40 is the compared/retained future while B V31 is the recovery base. Different labels are
  intentional; the ambiguous comparison count is a separate finding.
- The integrated C17 V8/V12 pressure-removal example already exists. The gap is the represented
  query-switch sequence and retention, not a missing example.
- The two-state shared-prefix prism remains independent from that integrated word-removal fixture.
- Evidence paths were corrected, and base steps with missing required subtasks were downgraded while
  preserving positive static observations.

Three base-step classifications and five storage-boundary repeats were adjudicated by the lead. The
final runner reports remain separate; the consolidated JSON retains each original outcome and the
reason wherever a lead outcome differs.

| Reference              | Runner outcome  | Lead outcome    | Reason                                                                                                                                                                                                       |
| ---------------------- | --------------- | --------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| J19/3                  | State undefined | Not represented | The suite already requires replies to belong to the current request and not replace retained selection. The missing request/stale-response state is absent representation, not an undecided acceptance rule. |
| J23/3                  | No affordance   | Not represented | Emphasis, enablement, Inspect package and List layers are visible. The resulting quiet-link list and disabled state are absent, rather than every starting affordance being absent.                          |
| J25/4                  | State undefined | Not represented | The suite already declares sibling preservation and no visits from preview, annotation or restore. Result/ledger cards are missing; the intended boundary is defined.                                        |
| J22/Storage boundaries | State undefined | Not represented | The journey contract declares reader-owned versus visited-source effects. Proposed effect/transition records are absent; no real store behavior was executed.                                                |
| J23/Storage boundaries | State undefined | Not represented | The journey contract declares reader-owned versus visited-source effects. Proposed effect/transition records are absent; no real store behavior was executed.                                                |
| J24/Storage boundaries | State undefined | Not represented | The journey contract declares reader-owned versus visited-source effects. Proposed effect/transition records are absent; no real store behavior was executed.                                                |
| J25/Storage boundaries | State undefined | Not represented | The journey contract declares reader-owned versus visited-source effects. Proposed effect/transition records are absent; no real store behavior was executed.                                                |
| J26/Storage boundaries | State undefined | Not represented | The journey contract declares reader-owned versus visited-source effects. Proposed effect/transition records are absent; no real store behavior was executed.                                                |

The raw base totals were 13 Concept supported, 63 Not represented, 7 State undefined, 2 No
affordance, 1 Confusing. Lead totals in the first table resolve missing representation separately
from policies the journey contract already defines.

## Before implementation

1. Decide and document the forge/draft unknown-outcome and recovery-validation contracts. Review the
   exact source/destination/author/effect and returned-identity states before relying on commit.
1. Prepare state cards for query/filter/cancel, exact-version acquisition, Link and walk navigation,
   package updates, typed save/duplicate/eligibility, and offline restore. Keep state/fixture IDs
   and proposed effects visible.
1. Clarify the three visible wording/count ambiguities. Add exact evidence/locator inspectors and a
   discoverable off-window pouch list. Preserve existing correctly labelled fixtures.
1. Add keyboard/list focus maps and narrow/enlarged/reduced-motion representations, then rerun the
   affected concept steps with the same outcome rules. A later participant session should test
   unaided first choices; these three expert runners already knew the contract.
1. Once implemented, validate real input, networking, persistence, source/write boundaries and
   backend behavior separately under the live journey rules.

## Artifacts and method

- [Frozen manifest](manifest.json): assignments, journey steps, specification hash and image hashes.
- [Run brief](run-brief.md): AI review method, evidence rules and schemas.
- [Consolidated machine-readable results](results.json): all steps, variants, repeats, source
  findings and lead decisions.
- [Runner 1 JSON](runner-1/results.json), [Runner 2 JSON](runner-2/results.json),
  [Runner 3 JSON](runner-3/results.json).

The review used the [J17–J32 contract](../../ux_workflow_spatial_concepts.md) and evidence
principles from [UX validation](../../../.claude/skills/ux-validation/SKILL.md) and
[Xuzz UI design](../../../.claude/skills/xuzz-ui-design/SKILL.md). The concept contract governs this
pre-implementation run; no live harness or product implementation was added.
