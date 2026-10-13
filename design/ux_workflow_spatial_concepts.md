# Spatial Concepts in Xuzz: Pre-implementation User Journeys

## Purpose

Test whether a reader can understand and complete the proposed spatial workflows before
implementation. These sixteen journeys, J17–J32, use the numbered steps, **Evidence** and **Watch
for** format of [J1–J16](ux_workflow_real_work.md). They extend that contract without changing its
live-build acceptance rules.

Scope: the store telescope, Omni lens, comparison prism, Walk compass, layer console, typed pouches,
link forge, media cue bridge, Hypertime Loom and integrated investigation workspace. This document
specifies tests; no participant sessions or journeys have been run.

A concept review establishes discoverability, interpretation and the clarity of proposed
transitions. It cannot prove storage behavior, cryptography, network acquisition, accessible input
delivery, animation, rendering budgets or backend correctness. Those require subsequent live
validation through the [UX validation workflow](../.claude/skills/ux-validation/SKILL.md).

## Running before implementation

Use the linked final images as starting frames. Prepare paper overlays, state cards or a clickable
prototype for the transitions before a session; no product implementation is required. A prototype
is optional: an absent state is a finding, not permission to invent a successful one.

Give the participant the journey's goal in ordinary language. Keep the steps and expected identities
on the evaluator's worksheet. Ask the participant to choose a visible action and predict its effect
before showing the next scripted state. Record their first choice and explanation without teaching
the control or pointing to the expected answer.

The facilitator may simulate only transitions already declared in the fixture contract. A prepared
fixture is test setup, not evidence that Xuzz can create, fetch or persist it. Record which
transitions were simulated. If a missing state is merely described aloud, label that part **Not
represented**; if its intended behavior is undecided, label it **State undefined**.

For each step, retain the frame/state-card ID, chosen affordance, participant prediction, scripted
result, interpretation afterwards, assistance given and one concept outcome. Time and hesitation can
be recorded descriptively; do not invent success-rate or speed gates.

Treat identity, ordering, branch choice, destination and write boundaries as observable facts. For a
simulated save, reopen or commit, record the proposed descriptor or state-ledger diff and the
participant's understanding. This is a proposed-effect record, never an actual store dump.

Before signing off a journey, prepare its unavailable/empty/loading variant and repeat the relevant
tasks with a synchronized list, a proposed keyboard path, a narrow layout and reduced motion. Apply
the [cross-journey checks](#cross-journey-checks) to every journey, not only J32.

## Shared fixtures and state cards

Use stable symbolic authorities, object IDs, source ranges, occurrences and versions in the fixture
manifest. Version labels are scoped to their owning store; equal labels are not globally equal
identities. Expand abbreviated identifiers in an inspector. Diagram order is not ancestry.

| Fixture                        | Required state contract                                                                                                                                                                                                                                                                                   |
| ------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Pressure investigation         | Local Lab notes V12; Rig slice C17 V8 with `pressure: 3.14 bar`; C17 V12 with `3.16 bar` and no pressure match; remote-origin verified cached Review V4.                                                                                                                                                  |
| Selected relation              | One Comment Link L17, Left 1 = exact Lab passage, Left 2 = exact C17 V8 content spans; Right 1 = Review V4 passage, Right 2 = C24 V8 contents, Right 3 = unavailable remote span V9. Keep source ranges separate from entry occurrences.                                                                  |
| Reader visits                  | 01 Lab → 02 C17 → 03 Review, with sibling route 02 → 04 Calibration. Current visit 03. L17 was selected at 01 and retained at 02, so its origin is 01.                                                                                                                                                    |
| Query/evidence                 | Literal pressure with case folding; native passage/cell matches, image OCR region, audio transcript time range, video title-only match with payload not held; store, version and evidence status on every result.                                                                                         |
| Coverage                       | Eight selected stores: local 3/3 complete, remote 2/5 complete, three remote stores pending. Twenty-four matches found so far; three visible in the integrated view. A completed surface never implies all media/branches were searched.                                                                  |
| Independent equal-text sources | Two passages with equal text and different primedia addresses; one true transcluded occurrence sharing addresses; an exact duplicate saved reference. Preserve all three distinctions.                                                                                                                    |
| Publication request            | Pinned publisher, publication, manifest and edition V12; a later pointer resolves to V13. Script signature-valid, payload-unchecked, interrupted 68/96 pieces, validated 96/96, invalid, unavailable and cached-offline states separately.                                                                |
| Packages                       | Lab review #12: 8 links, enabled/normal; Methods annotations #4: 14 links, enabled/quiet; Open discussion #7: 63 links, disabled. Lab review #13 has nine links and is a separate unenabled, unretained candidate until explicit retention. Immutable carrier IDs and verification status accompany rows. |
| Two-state prism                | Independent Store A fixture: C17 V8 and V12 retain the same original pressure prefix while the value changes 3.14 to 3.16 bar. This is not the integrated fixture where V12 lacks pressure.                                                                                                               |
| Comparison variant             | A distinct declared fixture: one state before the two branch tips' lowest common ancestor, and both tips; different text/value changes, cell membership and reciprocal dimension-connection changes. Exact shared ranges are known separately from equal text.                                            |
| Typed pouch                    | Integrated pouch has three cards: passage, whole-cell reference, saved search recipe. A separate dense pouch has 146 saved, 18 filtered, six visible, spanning spans, cells, ops, queries, walks and drafts.                                                                                              |
| Draft/forge                    | Draft L7 has two left and three right content members, one pinned destination/branch/base and an explicit authoring mode. A separate dense draft has 40 left/60 right members. Keep groups, ordered members and occurrence counts distinct.                                                               |
| Media cues                     | Cached video V9 at 00:38, audio V7 at 02:14–02:18, a document range and image region. Manual viewing cues are reader descriptors, separate from an authored Link.                                                                                                                                         |
| Loom/effects                   | 100 trunk ops and three 20-op branches; a separate ten-path fixture; exact ABCDEF insertion/deletion/move before/after states; C24 rewiring C17's neighbor; a captured page footprint that later reflows.                                                                                                 |
| Recovery variant               | Source passage in Draft A V27; separately selected target Draft B base V31, with its existing futures. Preview, cancel, commit and unresolved-outcome cards declare which branch and source ranges would be written.                                                                                      |

These are declared evaluation fixtures, not facts inferred from decorative mockup labels. Earlier
images use independent example histories. Reconcile an image to the chosen fixture with a labelled
overlay or record the mismatch; do not quietly reuse inconsistent numbers or identities. Optional
previews, required reference validation and destination validation are separate gates.

## Journeys

### J17. Discover and acquire an exact remote edition

1. Start from the [store telescope](mockups/store-telescope-2026-10-13/01-discovery.png). Find a
   publication through a topic, followed-author scope or supported publication URI. Inspect it while
   keeping the original reading place.
1. Identify the publisher key, publication, pinned edition, catalog signature, independent author
   trust, payload integrity and availability. Explain which facts have and have not been checked.
1. Request the pinned edition. Advance the publisher's pointer to V13 while the V12 request is
   pending; choose whether to keep V12 or make a separate request for V13.
1. Use the [acquisition states](mockups/store-telescope-2026-10-13/02-acquisition.png) to inspect
   interruption at 68/96 verified pieces, retry and cancellation. Encounter an invalid payload and
   unavailable peers as separate variants; opening remains gated on complete-store validation.
1. At Ready V12, explicitly open it, then reopen the declared cached V12 offline. Check separately
   whether author following or third-party package enablement changed.

Evidence: chosen discovery controls and predictions; publisher/publication/edition identity at each
state; scripted request and cache records; explanations of signature, trust and integrity; separate
invalid/interrupted/ready frames. Retry, verification and offline persistence remain simulated until
a live download run supplies actual evidence.

Watch for: selection starting acquisition; a title treated as identity; catalog signature reported
as payload verification; V13 replacing the request; unchecked availability described as zero peers;
opening an invalid/incomplete store; automatic following or package activation.

### J18. Find one term across media and store origins

1. Start from [Omni lens](mockups/omni-search-2026-10-13/01-omni-search.png) and search for pressure
   in the eight-store scope. Find a native passage, cell match, image OCR region, audio transcript
   and video title-only result without switching to unrelated search tools.
1. Inspect each result and identify its media, originating store, exact version, matching field and
   held/not-held status. Explain why cached remote content still has a remote origin.
1. Locate the image's recognized word region and the recording's transcript time range. Distinguish
   derived text from an authored content span and from the media bytes themselves.
1. Inspect the not-held video's title match. Choose to acquire its pinned edition or leave it
   untouched; no frame or transcript appears merely because metadata matched.
1. Open one resolved exact occurrence explicitly, then return to the original reading place. Inspect
   independent equal-text passages and the true shared-source occurrence separately.

Evidence: selected card/inspector frames, media locators and source descriptors; participant
explanations of native/OCR/transcript/metadata evidence; staged acquisition choice; before/after
reading-place cards and retained independent source identities.

Watch for: retyped equal words merged into one source; missing media locators; OCR treated as author
text; transcript words gold-linked to audio on semantic similarity; cached items relabelled local;
metadata-only matches counted as searched payload; inspection autoplaying or entering.

### J19. Filter a running query without losing the selected result

1. From the same query, pin C17 V8 and narrow to cells, native text and historical states. Restore
   all media, then select cached-only and a subset of stores. State which exclusions each choice
   makes.
1. Switch grouping and sort order while keeping the exact selected occurrence. Apply a filter that
   excludes it; find the retained inspector's excluded-from-results status and choose to clear it or
   restore the filter.
1. Deliver new results asynchronously and a response from the superseded query. The participant
   identifies which request the displayed counts and selected record belong to.
1. Inspect store/media/branch coverage with three stores pending and an unsupported OCR surface.
   Cancel the current request without losing collected results. Rerun explicitly.
1. Inspect both a completed-empty scope and an incomplete zero-found-so-far scope. Save the recipe
   separately from a captured result set; later rerun it with one new match.

Evidence: query/request and selection descriptors before/after each filter; total/group/visible
counts and coverage frames; cancellation and stale-response cards; saved recipe versus result
snapshot records, with the participant's interpretation of zero results.

Watch for: selecting the first surviving result automatically; a filter deleting saved members;
stale replies replacing the current query; whole-store completion inferred from one indexed surface;
cancel erasing results; a saved recipe silently refreshing or editing a collection or Link.

### J20. Search historical occurrences with visible matching rules

1. Start from the
   [content-history study](mockups/hypertime-loom-journeys-2026-10-12/03-content-search-v2.png).
   Search 3.14 in math prose and a pressure cell across declared branches and historical states.
1. Compare literal text with whole-token mode using 3.14159 and 3.140 variants. Switch to numeric
   matching only after inspecting its normalization and unit rules. Related ideas requires a
   separate explicit expansion; pi is not exactly equal to 3.14.
1. Change the query explicitly to literal pressure with case folding. Find C17 V8's pressure match
   while V12 lacks that word. Inspect the older occurrence without treating the current state as a
   result or replacing the historical reference.
1. Expand a compacted unchanged state run and a repeated source occurrence. Distinguish occurrences,
   states and affecting-operation counts; retain independent equal-text sources.
1. Show partial branch coverage, then a missing historical state and a completed branch with no
   matches. Save the query recipe and its optional captured results separately.

Evidence: rule and scope controls, exact version/occurrence descriptors, included/excluded variant
cards and explanations; expanded state-run counts; missing/partial/completed scope states; recipe
and captured-results records. This is not proof of a historical search index.

Watch for: numeric normalization applied to literal text; units omitted; pi suggested as equality;
current-head fallback; raw permascroll matches presented as document occurrences; state/operation
counts conflated; unseen branches reported complete; gold used for textual or numeric similarity.

### J21. Compare pinned states at text and topology changes

1. Start from the [comparison prism](mockups/spatial-elements-2026-10-13/02-comparison-prism.png).
   Use its independent two-state fixture, with the shared pressure prefix present in both V8 and
   V12, rather than the integrated investigation's word-removal fixture. Pin both exact states of
   C17, inspect the reading difference and distinguish unchanged shared source ranges from changed
   spans and independently equal text.
1. Use the comparison-variant state cards to select an earlier state before the lowest common
   ancestor and both branch tips. Identify actual ancestry without inferring it from left/right
   order.
1. Inspect changed text and typed values with units, then changed cell membership and reciprocal
   dimension connections. Focus each change site with its surrounding page or rank neighborhood.
1. Swap sides or choose another difference without replacing either pinned version. Inspect an
   unavailable region with a retained target descriptor rather than a substitute preview.
1. Explicitly enter one chosen occurrence, return, save the comparison and replay a reopen card.
   Recovery opens a separate authoring preview rather than applying the difference immediately.

Evidence: both pinned descriptors, three-state ancestry and change-site cards, exact shared-range
ports and typed/topology differences; predicted inspection/entry/save effects; reopen and
unavailable states. The two-face image alone does not represent the required three-state or topology
transitions.

Watch for: gold touching changed values; equal words asserted as shared addresses; omitted topology;
a changed cell focused outside its neighborhood; direct parenthood inferred from deck order; version
replacement on swap; comparison modifying sources; recovery committing on inspection.

### J22. Investigate around a stable reading place

1. Start from [quiet reading](mockups/investigation-workspace-2026-10-13/01-quiet-reading.png).
   Identify current visit 03, Review V4, search scope, selected L17 cursors, origin and saved pouch.
1. Open [expanded view](mockups/investigation-workspace-2026-10-13/02-expanded-investigation.png).
   Locate search results, pinned C17 comparison, layers and pouch without losing the readable Review
   passage or treating a tool preview as a completed visit.
1. Inspect C17 V8 and pinned V12. Explain why the latter's no-pressure state is not a search hit and
   why it is unrelated to L17's C24 right member.
1. Fold comparison alone, change a layer's view emphasis, inspect saved evidence, then fold all
   tools. Keep the query, both member cursors, reading offset, version and return path.
1. Use Activity Back to visit 02 and Return to origin 01. Reopen the tools after returning, and
   inspect the retained sibling route 02 → 04.

Evidence: paired frames with current/preview labels, selected descriptors and route graph; chosen
fold/inspect/return actions and simulated visit ledger; before/after reading offsets. Note the
existing raster pair's small page drift; exact stable bounds and motion are not demonstrated.

Watch for: a panel covering body text; folding resetting state or recording a visit; V12 mistaken
for the current reading place; cyan relation landing on a layer control; source companions lost;
return resetting the caret; extra mini-apps competing with the reading surface.

### J23. Control immutable link packages without editing their relations

1. Start from the [layer console](mockups/spatial-elements-2026-10-13/04-layer-console.png).
   Identify the three retained packages, two enabled packages and their distinct normal/quiet
   emphasis.
1. Inspect a disabled package without enabling it. Inspect immutable carrier identity, publisher,
   signature and independent trust; encountering an unchecked/invalid package exposes a declared
   policy.
1. Quiet Methods annotations and disable it separately. Explain the difference between reducing
   visual emphasis and excluding its links, including how a quiet link remains reachable by list.
1. Inspect Lab review #13 as a separate candidate while #12 stays enabled. Compare and explicitly
   choose whether to switch; retain the old package and exact selected Link identity.
1. Reopen the scripted session and restore the participant's chosen carrier-specific preferences.
   Compute enabled/retained counts from that resulting ledger: 22/85 describes only the initial
   fixture. Retaining candidate #13 adds its nine links to the retained inventory; merely reviewing
   it does not. Disabling a package changes enabled counts without removing its retained links.

Evidence: checkbox/emphasis/inspect choices, package IDs and verification explanations; update
comparison cards; before/after view-preference ledger and count reconciliation. No actual signature
check or preference persistence is established by this exercise.

Watch for: emphasis rewriting stored prominence; inspection enabling a package; an update
auto-enabled by mutable curator name; a new carrier inheriting the old Link ID; quiet text becoming
unreadable; disabled items intercepting input; source-store writes attributed to visibility changes.

### J24. Keep one complete relation through distant and unavailable members

1. In the integrated workspace, inspect the already-pinned L17 session and identify both complete
   ordered endsets, active right, remembered Left 2/2, Right 1/3 and link origin 01. Cross previews
   a side; Enter changes place.
1. Cycle left and right members independently, choose an exact repeated occurrence, and inspect the
   other overlapping Link without inheriting L17's cursors. Reselect L17 deliberately as a new
   session rooted at current visit 03 and choose its cursors; then restore the original saved
   visit-03 context, with origin 01 and both cursors intact, before testing its original return
   path.
1. Request right member 3's exact unavailable span, cancel, and retry it after a scripted
   availability change. Keep its ordinal and identity; no visit completes while the target is
   unresolved.
1. Enter a distant resolved cell through a declared temporary neighborhood, then return to origin.
   Inspect a wrong-version and locked-content variant without silently choosing another occurrence.
1. Disable L17's contributing package while it is pinned. Its drawing hides, but the retained
   inspector explains the disabled layer and offers explicit dismissal or re-enable.

Evidence: each selected Link/member/occurrence descriptor, full count/cursor frames, pending/cancel
cards, distant neighborhood and proposed completed-visit ledger; disabled-but-pinned context. This
extends J16 in the [live navigation journeys](ux_workflow_real_work.md#journeys) without claiming
those live navigation checks have passed.

Watch for: a beam inventing left/right pairings; a covering range filling disjoint gaps; hidden
members dropped from counts; a second Link silently replacing the selection; inaccessible distant
cells; cancellation inventing an arrival; a disabled layer discarding the selected context.

### J25. Preview and resume branching reader visits

1. Start from the [Walk compass](mockups/spatial-elements-2026-10-13/01-walk-compass.png) with its
   declared independent five-visit fixture: 01 → 02 → 03 and 01 → 02 → 04 → 05, current 03.
1. Preview visit 05 and distinguish current visit 03 from the selected preview. Inspect the saved
   version, occurrence and playback position before choosing Resume.
1. Return to 02 and choose each forward child. Resume 03 and verify that its forward control has no
   child; its sibling 04 is reachable by returning to the parent, not by inventing 03 → 04.
1. Reference one visit, annotate another and continue from an earlier visit. Existing children
   remain; preview, annotation and restore do not create completed visits.
1. Replay a close/reopen card, then an unavailable saved version. Restore named targets and notes
   before previews, offer retry and retain the original futures.

Evidence: current/preview frames, route graph, chosen forward/back/resume controls; saved view/link
context and proposed visit/annotation records; reopen/unavailable cards. Route geometry can be
reviewed now; activity storage and media-position restoration remain live checks.

Watch for: reader visits shown as source microversion ancestry; a sibling treated as a child;
preview changing place; Resume picking the latest version; notes creating visits; an earlier branch
erasing futures; corrupt/unavailable activity replaced with a newly invented walk.

### J26. Collect typed evidence without changing what it refers to

1. From the integrated pouch, inspect its passage, whole-cell reference and saved-search recipe.
   Explain what each preserves and which actions it supports.
1. Use the [typed dense pouch](mockups/link-forge-pouches-2026-10-12/01-typed-pouch.png) variant.
   Save a disjoint span bundle, arbitrary operation, query, reader walk and link draft by visible
   actions; saving does not move or edit their subjects.
1. Filter 146 saved items to 18 with six visible, expand one counted group and find an off-window
   item through the list. Identify which counts describe saved membership, matches and previews.
1. Save an exact duplicate, equal text at different addresses and another occurrence of shared
   source. Inspect the explicit duplicate/membership policy before merging or replacing anything.
1. Dismiss and recover a card, then reopen with previews not held, a locked source and an unresolved
   authority. Keep descriptors and type-specific inspect/enter/rerun/resume actions.
1. Offer the pouch to a forge bay. Choose eligible concrete content spans and review exclusions;
   neither an op, recipe, walk, draft nor whole-cell label silently becomes a content endpoint.

Evidence: typed card/descriptor records, source-range and occurrence distinctions, counts and
off-window selection; duplicate chooser, dismissal/recovery and availability cards; eligibility
review. Saved query rules and captured results must remain separate records.

Watch for: bytes copied instead of references; span gaps lost; whole-cell and selected-content
actions conflated; same-worded sources deduplicated; dismissed source destroyed; every reference
given Quote; rerun changing authored membership; hidden collection items becoming unreachable.

### J27. Assemble one ordered Link with an explicit destination

1. Start from the
   [disjoint forge](mockups/link-forge-pouches-2026-10-12/02-disjoint-link-forge.png). Select two
   separated source ranges, preserve the unmarked gap, and stage two left and three right content
   members without creating a Link.
1. Inspect, reorder, remove and undo members through pointer and list actions. Distinguish source
   packs, capture groups, authored member order and occurrence choices.
1. Declare receiving store, branch, base state, author identity/status and authoring mode. Browse
   another document and an older source state without redirecting that pinned destination.
1. Preview the relation. Identify one proposed Link, both full endsets, exact anchors, selected
   relation type and prominence. Selecting an occurrence changes entry context, not source
   addresses.
1. Explicitly forge or discard. The scripted authoring result names one new Link, although its
   protocol may require multiple operations. Keep draft saving, source edits and destination effects
   separately declared.

Evidence: ordered descriptors before/after edits, unmarked source gap, destination/base cards and
proposed effect ledger; preview-versus-commit choice and authored Link record. Record the draft
authoring policy; unresolved destination or commit semantics are **State undefined**, not success.

Watch for: all left/right pairs minted as Links; source grouping reordering authored members; caret
choosing a new destination; unsupported endpoint types converted silently; staging represented as
Link creation; claims that all private staging metadata is write-free; Remove deleting source.

### J28. Resume a dense draft through unresolved and unknown outcomes

1. Reopen [Draft L7](mockups/link-forge-pouches-2026-10-12/04-reopen-draft.png) with both exact
   endsets, grouping, relation and pinned destination restored before preview bytes arrive.
1. Distinguish optional preview-not-held from required reference-validation pending and destination
   validation failure. Forge availability follows the declared policy, not thumbnail presence alone.
1. In the [40/60 member variant](mockups/link-forge-pouches-2026-10-12/03-dense-endsets.png), find
   left member 21, inspect its five occurrences, filter/reorder a bay and restore its true ordinal.
   Both endsets remain complete without drawing 2,400 pair connections.
1. Review a preflight failure with a retained draft, then a declared mid-commit failure. Identify
   what was written, what can be recovered and what may safely be retried.
1. Encounter **Commit outcome unknown** after an interruption. Use Check status before retry;
   resolve to either the existing created Link or a confirmed uncommitted draft under a declared
   retry protocol. An unknown outcome never becomes permission to forge a duplicate.
1. Keep or discard the draft separately, reopen it again, and compare ordered members/destination.

Evidence: restore, validation, dense-list/occurrence, failure and unknown-outcome state cards;
proposed draft/commit/status records and participant predictions. If no recoverable/idempotent
protocol is declared, record **State undefined**. No storyboard proves rollback or duplicate
prevention.

Watch for: missing previews replacing members; newer-state fallback; 40/60 totals mistaken for
occurrences; hidden members truncated; filter silently rewriting order; failed and unknown outcomes
conflated; status inspection creating a Link; blind retry; discard erasing authored source data.

### J29. Align mixed-media cues without authoring a relation

1. Start from the [media cue bridge](mockups/spatial-elements-2026-10-13/03-media-cue-bridge.png).
   Identify cached video V9, audio V7, the selected document range and image region.
1. Inspect the manual cue joining video 00:38 to audio 02:14–02:18. Explain the separate clocks,
   derived transcript and exact locators before starting any playback.
1. Preview alignment explicitly, adjust its offset, remove a cue and add another through an
   equivalent list action. Keep each source version and locator independent.
1. Save the cue set, clear staging and replay its reopen card. Encounter an unavailable media
   segment or unsupported seek basis; preserve the cue descriptor and offer a defined correction.
1. Open Forge relation from the cue set and review a separate draft's eligible content selections
   and exclusions. Cancel without treating the view cue as a stored Link.

Evidence: locator/cue descriptors, predicted playback/adjust/save effects, separate clocks and
derived-text labels; staged cue changes and unavailable/reopen cards; separate forge chooser. Moving
playback and synchronization accuracy remain unrepresented until a prototype/live run shows them.

Watch for: manual alignment asserted as shared identity; gold on equal transcript words; autoplay;
time units or seek basis omitted; all media forced onto one timestamp; removing a cue deleting
media; image/transcript locators substituted with invented spans; saving cues automatically forging.

### J30. Investigate exact range and topology effects in the Loom

1. Start from the [loom journey study](mockups/hypertime-loom-journeys-2026-10-12/journeys.md).
   Inspect the declared 100-op trunk/three-branch fixture, then the ten-path variant. Expand one
   coil and find every branch exit through its counted pack or synchronized list.
1. Tug a container into a parallel lens. Keep source operations at their original loom positions;
   distinguish stored ancestry, same-op correspondence and filtered view order.
1. Apply History lens · Touches to exact ABCDEF ranges. Inspect insertion xy, deletion CD from its
   before-state and the moved fragments; compare Original source with an explicit Evolving passage
   rule and This occurrence with All uses.
1. Exclude an unrelated earlier offset shift, then include it as context. Each affecting result
   names its owning frame, parent state, reason and before/after footprint.
1. Inspect the C24 operation that replaces C17's neighbor C18 with C24 on d.reading while C17 text
   stays unchanged. Distinguish own edits from affected topology and preserve reciprocal endpoints.
1. Capture a page footprint, then view reflow that splits a selected passage across later pages.
   Preserve the snapshot/layout and exact ranges; page numbers are not permanent object identity.
1. Cancel a partial lens, save its recipe/results separately, and inspect a completed-empty scope.
   Local expansion and tugging create no new source ops.

Evidence: branch/operation identity cards, before/after range and topology diagrams, original/
parallel same-op descriptors, page footprint and coverage records; participant explanations of
silver, pearl, seamed and gold relations. Concept counts do not establish query/GPU budgets.

Watch for: folds hiding branch exits; new branches rearranging the selected anchor; tugs inventing
ancestry or ops; own-edit chains called complete effect coverage; deletion coordinates used as
immutable source addresses; captured page replaced by later pagination; partial scopes called empty.

### J31. Recover a selected historical passage into a declared new branch

1. Start from the
   [recovery preview](mockups/hypertime-loom-journeys-2026-10-12/06-branch-recovery.png). Inspect
   the source passage at Draft A V27, keeping its exact ranges, state and occurrence.
1. Choose Draft B V31 as target base separately from the source. Inspect the full target state and
   existing futures; selecting a recovery source does not choose a writable destination.
1. Open the authoring preview and identify proposed inserted/transcluded ranges, destination, branch
   mode, author verification and which effects would be written. No change is yet saved.
1. Cancel and verify that both sources, existing futures and draft choice remain. Repeat with an
   unavailable source and then a receiving branch whose head advances beyond V31. The immutable
   pinned base V31 stays fixed until the participant explicitly retains it or chooses a new base;
   inspect the resulting authoring mode and validation before continuing.
1. Explicitly create the separately identified recovery branch only after source/base validation.
   Inspect its returned branch identity and origin addresses; no existing future is overwritten.
1. Reopen its result card and distinguish the new authored branch from the old source and a reader
   visit. Resolve inconsistent pictured difference counts against the declared fixture first.

Evidence: source and target/base descriptors, full-state preview, proposed effect ledger, cancel/
validation/commit/reopen cards and branch graph; count reconciliation and participant prediction.
This is selected content recovery, not automatic merge or replay of old coordinates.

Watch for: source V27 mistaken for target base; caret redirecting destination; preview creating a
branch; replacement of existing futures; copied equal text losing original addresses; unavailable
content substituted; a decorative difference total treated as validated change enumeration.

### J32. Resume an interrupted investigation offline with equivalent access

1. At integrated visit 03, keep the query, comparison, L17 cursors, source companions, package
   preferences, typed pouch and sibling reader route. Save/close through a presented control.
1. Replay a reopen card while offline. Restore exact identities before previews. Cached verified
   Review V4 remains readable; not-held media and incomplete search surfaces retain named statuses.
1. Use the proposed keyboard/list path to inspect the historical C17 reference, cycle a Link member
   and resume the chosen reader visit. Every action has a visible equivalent and named focus target.
1. Repeat the relevant inspection in a narrow layout, enlarged text and reduced motion. Access
   off-window cards, counted branch exits and both full endsets without pointer-only gestures.
1. Encounter unreadable saved reader data and a cancelled request. Preserve the data/descriptor and
   explain repair/retry options rather than regenerating a default session.
1. Reconnect, refresh the query and explicitly acquire a missing pinned edition. New results,
   publication pointers and package updates do not replace saved identities or enable new layers.

Evidence: proposed session descriptor before/after reopen; offline/locked/corrupt/cancel cards;
annotated focus order and action map; enlarged/list/reduced-motion state cards and refresh
decisions. Actual restart, keymap, accessibility tree, rendering parity and frame-time behavior
remain later tests.

Watch for: reopening at defaults; previews treated as required identity storage; latest-state
fallback; offline absence reported as completed zero matches; hidden controls intercepting focus;
inaccessible counted items; simulated focus mistaken for working assistive technology; reconnect
silently replacing editions, references, selected matches or package choices.

## Cross-journey checks

Record each applicable repeat per journey. A static label can support a design prediction but cannot
establish that a key, focus manager, accessibility tree or motion setting works.

| Repeat                           | What to observe now                                                                | Evidence still required after implementation                            |
| -------------------------------- | ---------------------------------------------------------------------------------- | ----------------------------------------------------------------------- |
| Pointer and discoverability      | First visible action chosen, hit-target proposal, equivalent menu for tugs/drops   | Real pointer/gesture delivery and picking against source identity       |
| List and keyboard                | Same records, commands, proposed focus order and return target                     | Default keymap, actual focus movement and semantic state parity         |
| Accessibility                    | Labels/types/states, non-color cues, planned reading order                         | Actual accessibility tree and assistive-technology interaction          |
| Density and narrow views         | Counts, individually readable rows, bounded fan and continuation paths             | Real scroll/windowing behavior, layout bounds and retained selection    |
| Enlarged text and optics         | Calm glyph backing, body/caption hierarchy, no beam over body text                 | Measured contrast, including 4.5:1 text contrast at rendered opacities  |
| Reduced motion                   | Immediate/stabilized state cards retain selected destination and source companion  | Actual camera/wing continuity, cancellation and no focus jump           |
| Lifecycle and unavailable states | Exact identity survives scripted close/offline/cancel/locked variants              | Restart, networking, cache validation and persistence evidence          |
| Storage boundaries               | Proposed ledger separates reader activity/collections from authored source effects | Actual visited/reader/destination store diffs and commit/retry behavior |
| Relation language                | Actual sharing versus equal text; ancestry versus correspondence versus filtering  | Correct source anchors, stable hues and picking in every backend        |

The existing images do not provide every required variant. Especially absent are keyboard focus
sequences, loading/empty/error transitions, dense scroll sequences, retry execution, moving media,
real restart and reduced-motion captures. Mark them **Not represented** until prepared.

## Findings and run record

Classify every step using concept outcomes; none is a live J1–J16 Pass.

| Outcome           | Meaning                                                                                                                             |
| ----------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| Concept supported | Participant chose an unaided visible action and correctly interpreted the represented scripted result. Simulation is named.         |
| Confusing         | The represented control, identity, result or recovery is misunderstood, or the scripted result conflicts with the declared fixture. |
| No affordance     | The supplied concept offers no discoverable control/equivalent action for the required task.                                        |
| State undefined   | The intended transition, side effect, identity rule or failure protocol has not been decided.                                       |
| Not represented   | A necessary frame, transition, input sequence or variant is absent from the supplied prototype.                                     |

A decision to supply a missing screen does not resolve an undefined behavior. If a participant
needed coaching, retain that assistance and classify discoverability as Confusing; do not replace
their first attempt with the correct coached action. Attach findings to the exact step/variant.

Use this worksheet for a session; it is a template, not a completed test report.

| Journey/step/variant | Fixture + frame IDs | Chosen affordance | Prediction + scripted result | Assistance | Concept outcome | Evidence |
| -------------------- | ------------------- | ----------------- | ---------------------------- | ---------- | --------------- | -------- |
| To be run            | —                   | —                 | —                            | —          | —               | —        |

For each finding, record what the reader tried, what they expected, what the represented concept
did, the exact evidence and a proposed design change. Rank identity loss, unintended entry/write,
inaccessible branches and misleading verification above visual polish. Do not fix product code
during a concept session.

Before implementation begins, resolve State undefined findings that affect identity, authoring
destination or commit/retry semantics; prepare missing critical frames and retest revised paths.
Concept supported outcomes may justify further design work but do not waive later live checks. No
mockup outcome certifies performance, complete indexing, atomic recovery or equivalent input.

Once implemented, rerun the relevant journeys through ordinary UI affordances with captured frames,
actual accessibility output and real store/cache/activity evidence. Use the existing live Findings
outcomes from [J1–J16](ux_workflow_real_work.md#findings), and the
[publication journeys](ux_workflow_publication.md) for actual discovery/acquisition/layer behavior.
