# Sentinel and unexplained constant cleanup

This work is in progress in the `design/vagent-open-questions` worktree. It does not claim that a
regex search proves the absence of sentinels. The initial review used three Luna agents with the
system-architect skill's quality-auditor, architectural-synthesizer and systems-profiler roles.

## Contract

Use optional or expected results for missing values and failures in first-party C++ APIs. Keep
mandated wire/on-disk encodings at explicit conversion boundaries. Native API conventions such as
OpenGL uniform locations, POSIX return values and standard range sentinels need interpretation at
their boundary, rather than a mechanical replacement of every zero, negative number or limit.

Numeric constants need domain names and units when their purpose is unclear. Existing user
configuration remains the home of user policy; naming a constant does not make configurable policy
an invariant. Test data, ordinary arithmetic, directions, colors and encoding masks are not
independently evidence of a magic number.

## Incorporated batches

- Removed OptionalCell's implicit conversion to CellRef. Graph neighbours and the UI graph wrapper
  now use standard optionals; callers test presence and dereference explicitly. Rank iterator end
  state is optional, and packed neighbour absence is filtered before exposing a Hop.
- Removed the unused noDense declaration and unsupported zero-cost/branchless claims from the
  storage adapter documentation.
- Split VortexCore link operations into explicit read, set, allocate and isolate operations. Value
  reads and writes distinguish omitted, zero and negative slice lengths; invalid get/getGrid reads
  no longer masquerade as Boolean false. Updated list-dimension defaults, host focus, stepping and
  stack-frame APIs to carry optionals.
- Store link writes take an optional target; the Op record alone encodes an absent target as zero.
  Arena and manifold writes distinguish an absent target from an invalid zero target.
- Traversal and link discovery represent default focus and unbounded radius with optionals. Zero
  radius is a bounded query; a negative radius is refused.
- Replaced invalid digit results in Vagent and Vicon lexers, unavailable publication sequence and
  reconciliation PID/port metadata with optionals. Required transport values remain required.
- Replaced image atlas layer, progress bookkeeping, decoder stream selection, SDL dialog failure,
  and stepped-view unbounded-limit sentinels with optionals. Click positions transfer as one
  synchronized optional pair, preserving zero and negative coordinates.
- Fixed array displacement treating valid integer extrema as missing coordinates. Empty widths
  already have an explicit count; extrema remain valid values on active axes.
- Named rendering tessellation, tolerances, fallback dimensions, radial defaults and toolbar
  constants without changing their values. Reused the existing FreeType fixed-point scale and
  derived the synthetic drawer tag capacity from its reserved ranges, validating actual row bounds.

## Verified checkpoint: phase 5

The full native build passed. Headless verification then covered:

- 806 library tests: all passed.
- 2,034 enabled engine cases: every case passed across the full run and follow-up batches.
- 243 ZigZag tests: all passed.
- Rootless swarm groups: all 12 transport/name tests and all five publication/checkpoint tests
  passed.

This was not a clean one-shot full-suite pass. The full run received SIGTERM during a mesh-listener
case after 1,897 successful engine cases and one selection-capture failure. The capture showed a
blank paper despite correct text/selection model state; the same test passed both the earlier
focused run and its isolated rerun. The mesh-listener case passed in isolation, and the remaining
135 engine tests passed in a separate batch. The failed capture is retained for investigation. No
rendering/test workaround was introduced from that single failure.

The root/unknown spool lookup, branch-local cell edits, empty structure names, unresolved editions,
explicit-zero replay-scope refusal, text extents, VPL empty views, and optional picking/layout
regressions were included in this checkpoint. Later edits below still require their own build/test
gate. Formatting uses clang-format 19.

## Verified checkpoint: phase 6

The full native build passed after Make regenerated all compilation-database fragments. This batch
includes:

- Shared scanner and Vagent EOF as optional characters; overflow-safe lookahead; a real source NUL
  remains data rather than EOF. Invalid numeric conversions return errors rather than numeric zero.
- Optional TextFit line limits, wider line counts, close/save document indices, accessibility tree
  roots, layout index queries, and user-home directory lookup.
- Optional VQL query/path/value/Boolean helper results and bindings, Vortex dereference/shape/tally
  and term rendering results; valid false, zero and empty atoms remain present values.
- Optional surface/visualizer focus state, activation/navigation guards and result/view DTO focus.

The integrated checks passed: 193 focused library tests, 304 focused engine/UI tests, and all 246
ZigZag tests. A final full native build also passed after the fixes below.

The regressions exposed and corrected a Layoutfilade child-height subtraction bug, an old
zero-as-unbounded text line-limit caller, and stale focus when rebinding an empty store. The
deletion test now establishes the intended NEG-only fixture while preserving POS-first neighbor
selection. Return without focus has a regression proving that it invokes no activation callback.

## Phase 7 integration results

The current batch separates resource-map and allocation IDs, layout parent IDs, focus registration,
listener readiness, and omitted system-document parents/versions from valid zero values. Vortex pin
and host focus inputs, and VPL lazy default dimensions, now use optional state.

The graph adapter returns optional neighbors while preserving the 12-byte packed link layout. The
slice UI uses a tagged derived-dimension query instead of a fake dimension cell reference. Primedia
file-descriptor bookkeeping is optional; native POSIX failures are decoded immediately at opening.

The full native build, final incremental rebuild, and clang-format 19 check of all 299 changed
C++/GLSL files passed. Runtime checks then exposed integration gaps:

- Library: 810 of 812 passed. Two layout tests found roots still carrying engaged parent ID zero.
- ZigZag: 239 of 249 passed. Ten layout/ink tests exposed the same remaining root-parent callers.
- Focused engine: 291 of 292 passed. An explicit-root ensure attempted a second store genesis.

The root-parent callers and second-genesis refusal are corrected in source and await a fresh build
and rerun. Do not interpret these counts as passing suite results. The new graph, metadata-axis,
resource, focus, port-loss, and spool regressions passed within these batches.

## Verified checkpoint: phase 8

Source changes now cover media timing/decoded extents, optional Scroll publisher identity, draft
link identity, the distinct root-Xanadoc focus target, and absent-parent indices in generic, layout,
and hole enfilades. Staging returns an optional byte offset (zero remains valid), and missing focus
produces no staged geometry instead of selecting a fabricated first cell. Stream alignment now uses
one named default shared by the backends.

Measured node sizes before and after the parent change are unchanged: Layout 64 bytes, Rank 128,
Span 320, Array 1,536, Chrono 384, and Hole 128. These are representation measurements, not runtime
performance claims. Syntax checks pass for the parent/staging groups. The first integration attempts
found a missing shared-constant include and a loader publisher consumer; both are fixed in source.
Cross-review also caught anonymous scrolls with different salts deduplicating to one identity; the
fix and regressions distinguish named, anonymous, and fixed-file live buffers.

Focus opening sequence, Store omitted-context selection, global scroll names, and bootstrap keys are
implemented and independently reviewed. The full native build passed after correcting a shared link
fixture that still returned a reserved value. A final incremental build includes test-only changes
that make absent media queries fail reliably instead of dereferencing a disengaged optional.
Metadata traversal now uses a lazy CSR view with no 64-dimension cap; its regression enumerates 70
active dimensions and checks inactive-edge filtering.

Runtime results: all 816 library tests and all 251 ZigZag tests passed. The engine batch ran 2,043
cases: 2,025 passed, 17 namespace-network cases skipped, and one offline-anthology fixture failed
because it used an invalid all-zero publisher identity. That fixture now uses a nonzero offline
publisher; all seven anthology cases passed on rerun without relaxing unresolved-content assertions.
All 37 binary orchestration tests then passed. All three backends passed the headless frame and
orchestration comparisons in `tools/compare-backends.sh`. Wire operation context remains a resolved
version; an explicit root must not be silently treated as an omitted auto-selection request. Direct
APIs whose default already means a real root retain that meaning.

## Verified checkpoint: phase 9

VQL current-cell context uses optional refs in evaluation, predicates, lambdas, destructuring, and
user definitions. Missing bindings shadow outer variables with empty streams; actual zero, false,
and empty text remain values. VPL builders use optional row/head/predecessor refs; invalid origins
and scalar-conversion refs are errors instead of numeric zero or empty text.

Chronofilade distinguishes a known root depth of zero from unrecorded capacity. Its depth, ancestor,
and LCA queries are optional; indexed counts measure recorded operations. It validates the stored
operation and parent before changing tables and refuses invalid/nonancestor compose paths. Named
transport cadence/resource budgets, test transport overrides, collaborator timing, and pouch search
limits preserve their previous values.

The native build and all 214 focused engine tests passed after correcting a new VQL test that had
used text-creation syntax where typed scalars were intended. All 251 ZigZag tests passed again.

## Phase 10 runtime checkpoint

The remaining VQL scalar-expression miss is now optional rather than an empty-string placeholder.
Consumers distinguish missing arguments/values from explicit empty text and ordinary defaults;
empty/newline document search is bounded, and an explicit missing delta target differs from an
omitted target. GIF indexed/time lookups are optional, including moved-from objects. Active document
indices and publication selection are optional. Registry misses are optional and an interned name is
a required record whose first valid ID is zero. Vicon and OR worker cursors are optional in memory;
ORSP retains its fixed-width encoding at the wire adapter.

File streams refuse unavailable/nonregular files and I/O errors instead of returning an empty size
or EOF; genuine empty files retain size zero. Empty read callbacks are refused. LibVLC callbacks
translate exceptions to their ABI error code, and Xuzz reports file-load failures through its
existing warning path. Media release clears the prior published frame under its mutex.

The full native build and final rebuild passed. All 822 library tests and all 251 ZigZag tests
passed. The non-E2E engine run passed 2,034 cases, skipped the 17 namespace-network cases, and
failed one new delta test: it had asked a path step to demonstrate a scalar expression default. The
correction and additional missing-target path regression are in phase 11 and await runtime
verification. Namespace swarm tests, refreshed Doxygen coverage, and final review/format checks
remain pending. No exhaustive-completion claim is made.

## In progress: phase 11

The VM now represents an anonymous choice-point/execution cursor with an optional. Alternate opcodes
remain required: malformed `Choice` instructions are refused before a choice point is pushed.
Memo-key lookup distinguishes a missing key from a registered empty key. Goal helpers are being
migrated to optional output cells while preserving false, zero and empty-text outputs.

VQL clone/target lookups and ASCII-grid focus are optional. Explicit root store registration differs
from automatic current/latest selection. Core genesis establishes the dimension rank before using
required predecessor IDs; schema alternative construction uses an optional previous alternative.

A deeper focus collision was confirmed despite the earlier root-focus tag: view state and replay
still collapsed explicit root and automatic selection when a branch contained both legacy root text
and a minted Xanadoc. A three-state automatic/root/birth selector is being incorporated through
Store replay, Session authoring, views and persisted pouch sources. Its regression and integration
gates remain open.

The initial full native build passed (346 translation units rebuilt), and formatting passed for all
356 changed C++/GLSL files. A focused engine run passed 260 of 263 tests. The remaining failures
were an old Choice fixture with no alternate and two new fixtures that encoded the producer/query
incorrectly; their corrections await the final rebuild. Independent review also found a Chronofilade
root/automatic collision, automatic incremental replay across a changed active structure, malformed
pouch numbers treated as absent, and invalid empty-array origins not refused. Those review fixes are
incorporated in source and need runtime verification.

All 12 rootless namespace transport/name tests passed against the initial phase-11 binaries. The
five publication/checkpoint namespace tests are running; no native rebuild occurs during that run.
Vocabulary publication now requires a stable scroll identity instead of fabricating one from an
object address. Quotation-builder roots and projector metadata dimensions are optional, and Prolog
cell predicates reject oversized numeric IDs before a narrowing cast can alias another cell.

## Verified checkpoint before upstream integration

The last full native build and the Store-panel follow-up build passed. All 2,057 enabled non-E2E
engine cases passed across the 2,056-case full-run result and the corrected pouch-fixture rerun. All
253 ZigZag tests passed after repairing the Auto containment-path cache. All 37 E2E cases passed
across the 35-case bulk result and the two targeted reruns. The library's 822-case phase-10 result
remains the latest full library run; later generic-library changes only clarified comments.

The E2E selection calibration again produced a blank paper despite accessibility containing the
correct text and selection. The isolated selection/drag test passed without a code/test workaround.
The failed capture is retained at `/tmp/gleditor-sentinel-phase11-selection-failed.ppm`; this is an
intermittent verification limitation, not a clean one-shot full-suite result. The Store-panel
failure was a real caller error: genesis appends home/dimension operations after its Slice birth.
Its caller now uses the recorded Slice birth instead of assuming the operation count names that
birth.

All 12 namespace transport/name tests and all five publication/checkpoint tests passed against the
initial phase-11 binaries. The updated full Doxygen gate passed with Graphviz: 925/925 authored
source files, 40,001 member entries and no missing configured probes. Formatting and Markdown lint
passed for 357 changed C++/GLSL files and 111 changed Markdown files; subsequent touched files were
formatted individually. These are checkpoint results before integration with newer upstream work.

## Remaining audit queue

- Verify the combined result after integrating newer upstream work; the pre-integration checkpoint
  above has completed its runtime/network/documentation gates.
- Verify the incorporated VM cursor/choice-point and stdlib goal-output migrations; keep
  construction-guaranteed IDs required.
- The three-state replay/focus selection migration is incorporated and verified at the checkpoint
  above; preserve those contracts while integrating upstream consumers.
- Migrate Session authoring refusals and invalid `versionOf`/`storeIndexOf` lookups away from root
  version/store-zero results, following [the Session plan](sentinel-session-api-plan.md). Audit
  empty-path absence in Session path binding at the same boundary.
- Replace the native `MicroversionId::Segment` branch sentinel using
  [the measured representation plan](sentinel-microversion-plan.md).
- Revisit packed native `DimLink` neighbor absence: its 12-byte in-memory representation is not
  itself a wire/on-disk boundary. Review explicit presence storage versus native optionals with
  measured memory costs before treating the retained encoding as an exception.
- Review required VM program-counter validation separately from optional cursor state.
- Review native resource bookkeeping and remaining numeric defaults with semantic evidence. Native
  callbacks and actual packed/wire fields need boundary interpretation; arithmetic identities,
  required IDs, and test-fixture values are not independently absence sentinels.

`LinkKey::id` and `CellSite::cell` are required identity fields; no absence branch was found for
their zero defaults. Package ordinal zero is valid. A numeric default alone is not sufficient
evidence of an absence sentinel, and removing a default must not introduce uninitialized reads.

The existing `tools/code-quality-audit.py` is not evidence of sentinel coverage: it contains no such
check and still refers to retired application and asset paths. Searches locate candidates; semantic
review and regressions establish their meaning. No exhaustive-completion claim is made.

## Workspace integrity

A subagent accidentally changed the primary checkout's Arena header while beginning the later
migration. Root reversed only the verified accidental hunk and confirmed that file has no diff in
the primary checkout. All intended cleanup edits remain in the isolated worktree. Later edits use
explicit worktree paths; existing user changes must be preserved.

## Residual audit snapshots

The reports for [library and tools](sentinel-residual-library-audit.md) and
[applications](sentinel-residual-app-audit.md) are historical candidate maps. Their picking,
layout-maxima, recording-cost, endpoint-accessor and reading-place findings have been incorporated;
use the current phase status above when assessing verification. Other entries remain queued or are
classified as required IDs/native encodings.
