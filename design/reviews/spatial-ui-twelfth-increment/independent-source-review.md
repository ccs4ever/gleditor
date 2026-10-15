# Independent source review

This review checked the J25 exact-occurrence persistence changes in the current candidate diff. It
was read-only; no product code was changed. The owning run reports the added navigation and codec
tests passing, with Xuzz at 106/106. The native baseline reproduces the defect: after selecting
closed endpoint `b1`, restarting resolves the saved ordinal to cell 24. The candidate restores the
exact `b1` site, reports it closed, refuses entry until the existing map reopens that version, and
then enters the same endpoint. Those build and native results are reported by the owner; this source
review did not run them.

## Occurrence identity and restoration

`LinkVisitContext` now carries an `OccurrencePin` for each side. A pin stores the authored
`PrimediaSpan` and its previously observed `Occurrence`, including the exact document or cell site.
The span check prevents a changed endset member at the same index from inheriting the old target.
The site check makes the stored occurrence identity independent of the order of the current query's
open documents and cells.

`selectionContext()` derives pins from the same `LinkOccurrences` snapshot used by Enter. Both Enter
and shutdown's `ReadingPlace` capture call this helper, so visits and the pinned selection record
the same target. During restore, `restorePins()` matches each saved pin against the current member
span and exact site, then updates the transient occurrence index. If the current query omits that
site, it adds the known occurrence to the selected snapshot so the panel can continue to identify
it. If the member span changed, the cursor is left unset and the pin remains available for
inspection. Explicit member or occurrence selection abandons that side's saved pin; dismissing the
link drops the whole selection.

This gives the pin a deliberately narrow meaning: it records a previous observation, not current
availability. It does not assert that the source version is open or that the reader can enter it.
The existing `LinkContext` entry preflight checks the exact saved source before focus or visit
creation. For a closed document or cell, the target stays addressable and Enter remains unavailable
until its exact source is available. The restoration path adds no camera, loader, or service.

The test additions reported by the owner cover reordered results on both sides, omitted sites,
changed member spans, visit restoration, a runtime context with an ordinal but no pin, and a
wrong-version document view. The new strict codec and failed-resolution/model tests are reported as
passing after this review's earlier feedback.

## Activity payload boundary

The pin fields are encoded in the shared activity context. Visit payloads now use `visit3` and saved
selection payloads use `selection3`; decoding is strict, with no `visit2` or `selection2` shim. A
chosen occurrence without its exact pin is rejected. The codec also checks pin flags, coverage and
member-span bounds. This changes only activity payloads: `CompactOpNode` and the store's operation
ABI/layout are unchanged.

This follows the repository's no-shim format rule while respecting R8's user-data rule. A legacy or
malformed activity payload raises `ActivityRecordUnreadable`. `Session::activity()` validates the
activity log and latest reading place before accepting the store; on refusal it reports the problem
and leaves the activity path untouched instead of replacing or appending to it. The consequence is
that older activity does not resume through this reader, but its bytes are preserved for inspection.

## Cell visibility boundary and remaining limit

The UI now considers a pinned cell visible only when the bound manifold matches its store authority
and version and contains that cell. Changing the manifold binding increments the context revision,
so the link panel and cell highlighter refresh. The highlighter suppresses a historical cell when a
different version is bound; the test reported by the owner checks that it becomes highlighted again
when the exact version is rebound. Entry validation separately checks the saved byte range against
the selected cell's content before allowing focus.

The optional extra range check in `cellInView()` was not added. A malformed but structurally
decodable activity pin whose source store/version/cell exists but whose range exceeds the cell's
content could therefore pass the highlight visibility gate; Enter still checks the range and refuses
it. Valid pins are produced from resolver observations, and the codec validates shape and numeric
bounds, but it does not revalidate every saved site against its source store during decode. That
semantic validation boundary remains open.

## Evidence limits

The source and owning regressions support exact-site preservation, no silent ordinal substitution,
closed-target refusal, and no new activity visit on refusal. They do not establish performance under
large endsets, backend parity for this change, or completion of J25 and the full navigation
journeys. Those remain separate validation work.
