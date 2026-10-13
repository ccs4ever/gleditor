# Omni lens: unified search and filter study

Design proposal, 2026-10-13. [Final mockup](01-omni-search.png). Generated visual concept; no
application code changed. Counts and example histories are fixtures.

The Omni lens extends the [store telescope](../store-telescope-2026-10-13/concepts.md) into one
query across media, versions and selected local/remote stores. A persistent query instrument owns
scope and match rules. A hinged filter dock narrows that scope; one bowed result tray interleaves
every result type. Selection unfolds an occurrence inspector and version deck.

## 1. One query, multiple evidence types

The illustrated literal query is `pressure`, with case folding enabled. Native text, cell text, OCR,
transcripts and metadata are distinct match surfaces. A detailed rules control must expose case,
language, normalization and token boundaries. Related ideas is opt-in, with explanations; it must
not silently broaden literal results.

| Result            | Evidence            | Exact context and availability           |
| ----------------- | ------------------- | ---------------------------------------- |
| Lab notes         | Native passage text | Local document at V12                    |
| C17 / Rig slice   | Native cell text    | Local historical occurrence at V8, Run B |
| Calibration plate | OCR word region     | Cached remote image at V5                |
| Debrief           | Transcript passage  | Cached remote audio at V7, 02:14         |
| Pressure rig test | Publication title   | Remote V9 not held; content not searched |
| Field manual      | Native passage text | Cached remote document at V3             |

OCR and transcript references need extraction provenance, language, confidence where available, and
a mapping to the image region or media time range. A recognized word is not an authored span. An
audio/video result locates the media and transcript range separately. An image result locates a
region rather than inventing a character offset in the image bytes.

The uncached video's metadata match stays in the same result field, labelled **Title match** and
**Content not searched**. Its placeholder says **Preview not held**. Downloading V9 is explicit;
selection does not initiate acquisition or playback.

## 2. Filter without switching contexts

The dock exposes five media classes, four evidence surfaces, store selection, cached-only scope,
version selection, branch selection and historical removals. Selecting a class changes the same
query rather than opening a different search tool.

“All reachable versions” means reachable states within the selected store/branch scope. “Include
removed” includes occurrences that matched in historical states but are absent later; it does not
imply destruction of immutable source content. Date ranges, exact version ranges, authors, access
state and source-specific rules can unfold from these dock sections.

Local/remote describes origin; held/not-held describes availability. A remote-origin cached item is
not silently relabelled local. Verified payload integrity and independent author trust are separate
from both. Multiple replicas of one store need one authority identity, with replica locations
exposed separately.

## 3. Inspect the matched version

C17 is selected at V8 with native text `pressure: 3.14 bar`. Its version wing shows the same cell at
V6 (`3.14 bar`, no match), V8 (selected match), and V12 (`3.16 bar`, match removed). V6/V12 are
context previews, not additional search hits. Version deck ordering does not assert parentage. A
real branch comparison needs actual lineage and explicit branch labels.

**Enter V8** changes the reading place; **Compare states** inspects context; **Save reference**
preserves exact subject/state/occurrence in the reader's collection. None substitutes the latest
state. The neutral dotted projection guide identifies the selected occurrence in two presentations.
Equal words do not imply shared source, so there are no gold relations between the result cards.

## 4. Coverage, counts and persistence

The mockup shows **24 found so far / 6 visible**, with three local stores complete, two remote
stores complete and three remote stores pending. Completion must be evaluated against requested
media, evidence surfaces, branches and states. Metadata-only coverage cannot claim complete remote
content coverage. The coverage drawer must expose each incomplete or unsupported surface.

Grouping by subject is a view over individual match records. Subject/group counts, occurrences,
state runs and affecting-operation counts remain separate. Grouping must never merge unrelated
sources merely because their excerpts match. Sorting/filtering preserves selected occurrence
identity as asynchronous results arrive.

**Save search** stores the recipe, selected scope and rules. A captured result set is a separate
snapshot; rerunning is explicit. Remote discovery changes can expand the scope only according to a
visible saved rule. Cancel search retains collected results and their partial-coverage status. Empty
completed, pending, locked, unavailable and unsupported are distinct states.

A synchronized list uses the same records and commands. Keyboard focus, accessible names, narrow
layouts and reduced-motion unfolding must retain equivalent selection/inspection/entry.

## 5. Foundation and review

The repository has a [catalog index](../../../apps/common/xanadu/swarm_catalog_index.hpp) and
telescope publication UI. The spatial plan proposes D4 versioned occurrence search and D5 durable
references. None establishes this unified OCR/transcript/multi-store search as implemented.
Federation, historical occurrence indexing, media extraction/locators and honest coverage records
are prerequisites for the proposed interface.

Visual review: the final image carries the existing smoky glass, silver hinges, bowed surfaces and
violet highlights into a continuous query/filter/result composition. Media thumbnails and evidence
badges let the reader scan results without opening each one. The pinned V8 inspector makes
cross-version behavior concrete.

Implementation refinements: expose case folding visibly; expand truncated authority identifiers;
show exact transcript end times and OCR provenance; expose full branch/state coverage. The small
cell tables are content previews, not a claim about ZigZag dimension topology. Color-coded
availability dots must retain their adjacent text labels. Contrast and picking require actual
backend verification; this image is design intent only.

Generation history is in [prompts.md](prompts.md) and [refinement-prompt.md](refinement-prompt.md).
The initial image remains for comparison.
