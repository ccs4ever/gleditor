# Runner 2: concept journey review

Mode: AI visual-support walkthrough of static final assets. This is not observed human behavior,
input delivery, store/network execution, accessibility validation or a live pass/fail run. All 26
assigned base steps have one conservative concept outcome; additional variants and 45 cross-check
repeats are recorded in `results.json`.

## Base journey steps

| Journey | Step | Outcome           | Affordance shown                                                                                                       | Evidence and visual finding                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| ------- | ---: | ----------------- | ---------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| J22     |    1 | Concept supported | Quiet-reading frame; visible reading place, search field, Link card and pouch dock.                                    | The frame labels CURRENT VISIT 03 / Review · V4; pressure; literal/case-folded local+remote scope; 24 found / 3 stores pending; L17 active right, Left 2/2, Right 1/3, origin Visit 01; pouch Pressure review · 3 saved. The image does not show a prior action sequence or verify that these are persisted reader state. Evidence: `design/mockups/investigation-workspace-2026-10-13/01-quiet-reading.png` (Top bar and center page identify pressure query, current visit 03 / Review V4; right Link card shows L17 active right, Left 2/2, Right 1/3, origin 01 and ordered members; lower dock shows source companions, Layers, saved pouch, and visit rail.)                                                                                                                                                              |
| J22     |    2 | Concept supported | Expanded-investigation control is visible in the quiet frame; expanded frame has Fold tools.                           | Expanded frame keeps the same Review V4 text and current-visit label while placing results, comparison, layers and pouch around it; visit rail says inspection keeps visit 03. Pixel-stable page bounds and transition are not represented; the two rasters show slight page-size/position drift. Evidence: `design/mockups/investigation-workspace-2026-10-13/01-quiet-reading.png` (Top-right Expand investigation and center Review V4.), `design/mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png` (Left results and C17 comparison; center current Review V4; right L17 summary, layers, three typed saved cards; bottom rail preserves current visit and sibling route 02→04.)                                                                                                                    |
| J22     |    3 | Concept supported | Expanded comparison cards and the quiet-frame complete L17 endset.                                                     | C17 V8 is labeled pressure: 3.14 bar / Historical match; pinned V12 is 3.16 bar / No pressure match. The quiet frame’s full L17 endset identifies Right 2 as C24 contents V8. The expanded view only summarizes L17 cursors and does not unfold the full endset there; query engine behavior and exact source-address identity are not run. Evidence: `design/mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png` (Left upper results and C17 V8/V12 comparison cards; right L17 compact cursor summary.), `design/mockups/investigation-workspace-2026-10-13/01-quiet-reading.png` (Right L17 card expanded to show all members, including Right 2 C24 contents · V8.)                                                                                                                                   |
| J22     |    4 | Not represented   | Visible Fold comparison, layer emphasis selector, Inspect saved, Fold tools controls.                                  | Controls and initial values exist; expanded frame shows L17 cursor summary, query, current Review V4, source companions and route. No post-action frame or ledger is provided. Effects of each fold/emphasis action and preservation across all requested state are unspecified visually; no resulting state is captured. Evidence: `design/mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png` (Center bottom Fold comparison; right layer Emphasis dropdowns and Inspect saved; top Fold tools; Link cursor header.)                                                                                                                                                                                                                                                                                    |
| J22     |    5 | Not represented   | Activity Back, Return to origin 01 and Walks controls are visible.                                                     | The frame offers Back to 02, Return to origin 01, and labels Other route: 02→04 Calibration. No after-return state, reopening state, caret/offset or simulated activity ledger is shown. Evidence: `design/mockups/investigation-workspace-2026-10-13/01-quiet-reading.png` (Bottom visit rail and buttons.), `design/mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png` (Bottom visit rail: other route 02→04; Back to 02; Return to origin 01.)                                                                                                                                                                                                                                                                                                                                                        |
| J23     |    1 | Concept supported | Layer console package checkboxes and emphasis sliders.                                                                 | Three packages are listed; Lab review #12 and Methods annotations #4 are checked, Open discussion #7 unchecked; Lab is normal and Methods quiet. Footer says 22 enabled, 85 retained, 6 visible. These are illustrative fixture counts and no live ledger is evaluated. Evidence: `design/mockups/spatial-elements-2026-10-13/04-layer-console.png` (Right Link layers panel lists Lab review #12 enabled, Methods annotations #4 enabled/quiet, Open discussion #7 disabled/signature unchecked, candidate #13; bottom states 22 enabled links / 85 retained / 6 visible.)                                                                                                                                                                                                                                                     |
| J23     |    2 | Not represented   | Inspect package controls; disabled Open discussion row.                                                                | Open discussion #7 is visibly unchecked and signature unchecked; the other two show valid signatures and identity unverified. No disabled-package inspector state, immutable carrier ID, publisher, independent trust field or declared policy for unchecked/invalid signatures is shown. Evidence: `design/mockups/spatial-elements-2026-10-13/04-layer-console.png` (Right package rows and verification lines.)                                                                                                                                                                                                                                                                                                                                                                                                              |
| J23     |    3 | No affordance     | Separate Methods emphasis dropdown/slider and enable checkbox; List layers button is present in integrated frame only. | The console shows Methods checked and quiet, distinct from Open discussion unchecked; no action-result state is depicted. No list result for quiet links and no post-disable state establish reachability or whether disabled items intercept input. Evidence: `design/mockups/spatial-elements-2026-10-13/04-layer-console.png` (Methods annotations #4 checkbox and Emphasis: quiet slider; Open discussion checkbox.)                                                                                                                                                                                                                                                                                                                                                                                                        |
| J23     |    4 | Not represented   | Compare update and Review version buttons on Lab review package.                                                       | Candidate #13 is labeled available/new version/not enabled; #12 remains checked; one Link L17 is shown. No candidate inspector, carrier identity, switch choice, or after-choice selected-Link descriptor is shown. Evidence: `design/mockups/spatial-elements-2026-10-13/04-layer-console.png` (Lab review #12 card and Lab review #13 available row; central Link L17.)                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| J23     |    5 | Not represented   | No reopen or preference-ledger control/frame in this concept.                                                          | Only the initial 22/85 counts are printed. No resulting preference ledger, session reopen state, or updated counts are represented; #13’s nine links are not in shown retained total. Evidence: `design/mockups/spatial-elements-2026-10-13/04-layer-console.png` (Footer count 22 enabled links · 85 retained · In selected scene · 6 visible.)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| J24     |    1 | Concept supported | Expanded integrated workspace L17 inspector; Enter selected occurrence button.                                         | The quiet frame displays both complete ordered endsets: left 1 Lab notes V12, 2 C17 V8; right 1 Review V4, 2 C24 contents V8, 3 unavailable Remote span V9. It labels active right, Left 2/2, Right 1/3, origin Visit 01. The expanded frame retains the cursor/origin summary and offers Inspect all members. No input is delivered and Enter’s completed-visit effect is not captured; the expanded frame itself does not unfold all member rows. Evidence: `design/mockups/investigation-workspace-2026-10-13/01-quiet-reading.png` (Right L17 card, ordered member rows 1–2 left and 1–3 right, active side, cursors and origin.), `design/mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png` (Right L17 compact cursor/origin summary, Inspect all members and Enter selected occurrence controls.) |
| J24     |    2 | Not represented   | Inspect all members and member/list affordances are visible.                                                           | Only one selected Link and cursor state are shown; no overlapping Link or repeated occurrence choices are pictured. Independent cursor changes, Link identity switching, new-session origin and restore of original context/return path have no states. Evidence: `design/mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png` (L17 header and member rows; Review current visit 03; source companions.)                                                                                                                                                                                                                                                                                                                                                                                                   |
| J24     |    3 | Not represented   | Unavailable member row and selected-occurrence entry control.                                                          | Right member 3 is labeled Remote span · V9 / unavailable. Pending request, cancel, changed-availability retry and no-visit ledger are absent. Evidence: `design/mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png` (Right L17 row 3 Remote span · V9, unavailable.)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| J24     |    4 | Not represented   | No declared temporary neighborhood control or availability-state card.                                                 | The integrated frame names C17 V8 and C24 V8 members. Distant-cell neighborhood, return result, wrong version and locked content states are not represented. Evidence: `design/mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png` (L17 rows 2 and 3.)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| J24     |    5 | Not represented   | Layer list shows enabled packages; no disable-while-pinned state.                                                      | Integrated frame shows Lab review #12 enabled and L17 selected. No disabled-contributor state or retained inspector/dismiss/re-enable choices. Evidence: `design/mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png` (Link L17 inspector and Link layers panel Lab review #12 checked.)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| J25     |    1 | Concept supported | Walk compass visualization and List view/Close controls.                                                               | Current 03 is Calibration plate V5; graph is 01→02→03 and 01→02→04→05, with visit 05 child of 04. The image does not establish how activity was stored or validate ancestry against a store. Evidence: `design/mockups/spatial-elements-2026-10-13/01-walk-compass.png` (Current visit 03 on left; previewing visit 05 on right with V7, cached status and playback 02:14; route cards show 01→02→03 and 01→02→04→05; controls show Activity Back, disabled forward from 03, Return to origin.)                                                                                                                                                                                                                                                                                                                                 |
| J25     |    2 | Concept supported | Preview visit 05 card; Resume visit 05 action.                                                                         | Current 03 and PREVIEWING VISIT 05 are separately labeled; Debrief V7 cached, waveform 02:14 and saved playback position 02:14 appear beside Resume. Exact occurrence and persisted Link context are not shown; resuming is not executed. Evidence: `design/mockups/spatial-elements-2026-10-13/01-walk-compass.png` (Left CURRENT VISIT 03 panel; right PREVIEWING VISIT 05 / Debrief V7 and 02:14 playback.)                                                                                                                                                                                                                                                                                                                                                                                                                  |
| J25     |    3 | Not represented   | Activity Back and Choose forward route controls.                                                                       | Current 03’s forward control states No forward visits from 03 and is disabled; graph labels 02→04 and 04→05; Activity Back names visit 02. No post-navigation states show returning to 02, choosing each child, resuming 03, or confirming the forward control remains empty after resume. Evidence: `design/mockups/spatial-elements-2026-10-13/01-walk-compass.png` (Bottom five-visit graph plus Activity Back / Visit 02 and disabled Choose forward route / No forward visits from 03.)                                                                                                                                                                                                                                                                                                                                    |
| J25     |    4 | State undefined   | Save reference and Add note controls.                                                                                  | Save reference and Add note are visible on preview; graph presents both futures. No reference/annotation result, continuation state or activity record shows whether children persist or operations add visits. Evidence: `design/mockups/spatial-elements-2026-10-13/01-walk-compass.png` (Preview panel buttons; five-node route graph.)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| J25     |    5 | Not represented   | Close button is shown; no reopen or unavailable-version card.                                                          | A close affordance exists. Reopen flow, unavailable version identity/status, retry and preserved-futures result are not shown. Evidence: `design/mockups/spatial-elements-2026-10-13/01-walk-compass.png` (Header Close and route graph.)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| J26     |    1 | Not represented   | Integrated pouch rows in expanded workspace.                                                                           | Pressure review lists PASSAGE Lab notes V12, CELL C17 V8 Whole-cell reference, and SEARCH pressure Saved recipe · rerun explicitly. None of the passage, whole-cell or saved-query inspectors is open; their preserved descriptors and type-specific actions therefore cannot be inspected in this frame. Evidence: `design/mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png` (Lower-right Pressure review / 3 saved rows labeled PASSAGE, CELL and SEARCH with source and recipe descriptions.), `design/mockups/investigation-workspace-2026-10-13/01-quiet-reading.png` (Pouch dock identifies Pressure review · 3 saved; the integrated frame has no selected pouch inspector.)                                                                                                                     |
| J26     |    2 | Not represented   | Save selection button is visible in typed pouch; type tabs/cards display six kinds.                                    | The pouch depicts span bundle, operation, query, walk and link draft as distinct typed cards; Save selection is visible. No action/result states demonstrate saving each type, preserving exact source descriptors, or no subject movement/edit. Composite typed persistence is described as extension in concept notes. Evidence: `design/mockups/link-forge-pouches-2026-10-12/01-typed-pouch.png` (Pressure review header: 146 saved, 18 filtered, 6 visible; type tabs and cards for span bundle, cell, op, query, walk and link draft; selected op inspector; dismissed-items count 12.)                                                                                                                                                                                                                                   |
| J26     |    3 | No affordance     | Filter count and category tabs are visible; no list/off-window control.                                                | Counts are distinct: 146 saved, 18 filtered, 6 visible. No list action, counted group expansion, off-window navigation or selected descriptor is shown; category tabs do not establish that path. Evidence: `design/mockups/link-forge-pouches-2026-10-12/01-typed-pouch.png` (Header counts and tabs; six visible stacked cards.)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| J26     |    4 | State undefined   | Save selection and card overflow menus are visible.                                                                    | No duplicate chooser or membership identity is shown. Policy and choice outcome remain undecided; there is no evidence of address-level distinctions. Evidence: `design/mockups/link-forge-pouches-2026-10-12/01-typed-pouch.png` (Card overflow menus; no chooser is present.)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| J26     |    5 | Not represented   | Dismissed items · 12 disclosure at bottom.                                                                             | Dismissed-items count is visible; selected operation inspector has Inspect op, Enter state and Open source. Recovery UI, reopen frame, availability distinctions, retry, and retained descriptor cards are absent. Evidence: `design/mockups/link-forge-pouches-2026-10-12/01-typed-pouch.png` (Selected OP · O42 inspector; Dismissed items · 12 row.)                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| J26     |    6 | Not represented   | No forge bay or pouch-to-forge chooser shown.                                                                          | Typed card categories reveal content and non-content types. No eligibility chooser or excluded-item list/result exists, so no endpoint conversion policy is visually confirmed. Evidence: `design/mockups/link-forge-pouches-2026-10-12/01-typed-pouch.png` (Card type labels CELL, OP, QUERY, WALK and LINK DRAFT.)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |

## Findings

### R2-01 · major

References: J22.4, J22.5, J23.5, J24.2, J25.4, J25.5.

Preserve workspace/Walk state across fold, return, reopen and continuation actions. Frames show
controls and initial state only; no post-action state or persistence ledger demonstrates
preservation.

Design change: Add paired before/after cards for fold-only, back/origin return, reopen, and
continue-from-earlier actions. Show visit identity, cursor pair, query, reading offset/version,
pouch/package preferences and retained sibling edges in each resulting state. Include a named reopen
card and unavailable target with retry.

Readiness: State transitions and lifecycle persistence need design resolution before implementation
claims can be made.

Evidence: `design/mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png` — Left
results and C17 comparison; center current Review V4; right L17 summary, layers, three typed saved
cards; bottom rail preserves current visit and sibling route 02→04.;
`design/mockups/spatial-elements-2026-10-13/01-walk-compass.png` — Current visit 03 on left;
previewing visit 05 on right with V7, cached status and playback 02:14; route cards show 01→02→03
and 01→02→04→05; controls show Activity Back, disabled forward from 03, Return to origin.;
`design/mockups/spatial-elements-2026-10-13/04-layer-console.png` — Right Link layers panel lists
Lab review #12 enabled, Methods annotations #4 enabled/quiet, Open discussion #7 disabled/signature
unchecked, candidate #13; bottom states 22 enabled links / 85 retained / 6 visible..

### R2-02 · major

References: J23.2, J23.4, J23.5, J24.5.

Inspect, compare or disable immutable package carriers while preserving selected Link identity.
Current frame shows independent signature and identity labels and distinct candidate, but omits
disabled-package inspector, full immutable carrier identity, candidate decision/result, updated
counts, and disabled-but-pinned L17 state.

Design change: Add package inspector states showing immutable carrier ID, publisher, signature,
independent trust, and unchecked/invalid policy; add explicit keep/switch choice for #13 with
selected Link ID preserved; add post-choice retained/enabled count reconciliation and
disabled-but-pinned inspector actions.

Readiness: Package trust, update choice and retained selection behavior remain unresolved.

Evidence: `design/mockups/spatial-elements-2026-10-13/04-layer-console.png` — Right Link layers
panel lists Lab review #12 enabled, Methods annotations #4 enabled/quiet, Open discussion #7
disabled/signature unchecked, candidate #13; bottom states 22 enabled links / 85 retained / 6
visible.; `design/mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png` — Left
results and C17 comparison; center current Review V4; right L17 summary, layers, three typed saved
cards; bottom rail preserves current visit and sibling route 02→04..

### R2-03 · major

References: J24.2, J24.3, J24.4.

Cycle member/occurrence selection and request distant or unavailable relation targets. The full
initial 2×3 ordered endsets and cursor are legible, including unavailable Right 3, but no cursor
mutation, overlapping-Link switch, new-session reselect/restore, pending/cancel/retry, temporary
neighborhood, wrong-version or locked state is shown.

Design change: Add state cards for independent cursor changes and repeated occurrence selection;
show Link ID on switch, new session origin 03 versus restored origin 01 context,
pending/cancel/retry on exact right member 3, distant neighborhood return, and wrong-version/locked
status without substitution.

Readiness: The relation overview is clear, while traversal and failure transitions are not yet
specified visually.

Evidence: `design/mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png` — L17
inspector and member rows..

### R2-04 · major

References: J26.1, J26.2, J26.3, J26.4, J26.5, J26.6.

Save, filter, deduplicate, recover and offer typed pouch items to the forge. Typed cards and
146/18/6 counts are clear, but the passage, whole-cell and recipe inspectors are absent; save
results, list-only off-window access, duplicate policy, recovery/unavailable states and endpoint
eligibility chooser are also absent.

Design change: Add type-specific inspectors for passage, whole-cell and recipe that show preserved
descriptors and supported actions; add a typed save receipt preserving source
address/state/occurrence; a searchable list with counted-group expansion; a duplicate chooser keyed
by exact identity versus same text/shared source; dismissal recovery and availability cards; and a
forge eligibility review that enumerates accepted spans and excluded cell/op/query/walk/draft types.
Keep recipe and optional captured results as distinct records.

Readiness: Pouch semantics are promising but composite typed persistence and forge handoff need
explicit design states.

Evidence: `design/mockups/link-forge-pouches-2026-10-12/01-typed-pouch.png` — Pressure review
header: 146 saved, 18 filtered, 6 visible; type tabs and cards for span bundle, cell, op, query,
walk and link draft; selected op inspector; dismissed-items count 12..

### R2-05 · minor

References: J22.2, J22.4.

Expand and fold investigation tools while keeping reading geometry stable. The central passage and
edition remain the same, but the final raster pair changes page size/position slightly; exact anchor
bounds and transition are not demonstrated.

Design change: Use a shared host-bound composition in the before/after states and show stable
reading bounds or clearly describe the accepted reflow; include reduced-motion cut state.

Readiness: The visual proposal communicates retained reading context, but pixel-stable geometry and
motion remain unassessed.

Evidence: `design/mockups/investigation-workspace-2026-10-13/01-quiet-reading.png` — Center Review
page.; `design/mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png` — Center
Review page..

### R2-06 · minor

References: J25.2, J25.4, J25.5, J26.5.

Restore visit target/media, add notes/references and recover pouch items. The compass gives a useful
preview/resume distinction and saved playback time, while note/reference results and
unavailable/reopen/recovery states are absent.

Design change: Provide result states for note/reference actions and reopen with unavailable targets,
preserved futures and explicit retry; offer pouch recovery with descriptor-retention evidence.

Readiness: Walk geometry is ready for further concept review; activity lifecycle and media
restoration still need explicit state designs.

Evidence: `design/mockups/spatial-elements-2026-10-13/01-walk-compass.png` — Current visit 03 on
left; previewing visit 05 on right with V7, cached status and playback 02:14; route cards show
01→02→03 and 01→02→04→05; controls show Activity Back, disabled forward from 03, Return to origin.;
`design/mockups/link-forge-pouches-2026-10-12/01-typed-pouch.png` — Pressure review header: 146
saved, 18 filtered, 6 visible; type tabs and cards for span bundle, cell, op, query, walk and link
draft; selected op inspector; dismissed-items count 12..

## Cross-checks

All nine requested checks are repeated for each of J22–J26 in `results.json`.
Pointer/discoverability and relation-language rows assess only visible naming and static identity;
they do not establish usable interaction. The remaining checks are marked Not represented or State
undefined where no matching frame or persisted-state evidence exists. Static images cannot establish
keyboard access, accessible names/focus, responsive density, enlarged text, reduced motion or
storage effects.

## Variant notes

The variants preserve the J22 integrated workspace fixture separately from J25’s independent
five-visit compass. J24 keeps C17 V12 comparison context separate from L17 Right 2 (C24 V8); J26
keeps whole-cell C17 reference separate from content spans, and a saved recipe separate from
captured results. For J23, 22 enabled / 85 retained is reported as the initial package fixture only;
no post-choice counts are inferred.

## Visual synthesis

The strongest represented ideas are persistent reading context around folded tools, explicit
current-versus-preview state, complete ordered L17 endsets with unavailable members retained,
package enablement separate from emphasis, and separate pouch membership/filter/visible counts. The
main missing visual states concern transitions, restoration, typed-reference policy, and failure
handling. The expanded investigation image also shows slight page geometry drift relative to quiet
mode; it preserves text and identity but does not prove a fixed anchor.
