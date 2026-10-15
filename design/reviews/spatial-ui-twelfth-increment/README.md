# Twelfth spatial UI checkpoint

Based on `c5f21617`, this increment preserves both exact selected occurrence identities across fresh
resolution and same-profile restart. It changes the private activity payloads to `visit3` and
`selection3`. Older activity payloads are refused and left untouched; they will not resume. The
store/node formats and sample fixtures are unchanged.

## Failure and implementation

The native baseline selects Link 22's Left document occurrence `b1`, enters it, returns to Origin,
closes `b1` and exits. After restart, its saved occurrence ordinal 2 selects cell 24 at `a34`. Enter
records a visit to that cell. Source operations remain unchanged, but the reader goes to a different
target without choosing it. The earlier normal restart with both views open retained the right
target; changed view membership is essential to the failing case.

Each saved side now carries an `OccurrencePin`: the authored `PrimediaSpan` and observed exact
`OccurrenceSite` with coverage. A shared `selectionContext()` captures both choices for completed
visits and the latest reading place. On restore, each side independently matches its member span and
exact site. Its transient ordinal is rebased to that site. A previously observed site absent from
the current query remains in the selected snapshot, just as a live selection retains a place whose
view closes. Known-place counts can therefore include closed targets. The existing exact
store/version entry preflight determines actual availability before appending a visit.

Changed authored members retain their saved pins for diagnosis without selecting a replacement;
explicit member/occurrence input abandons only that side's pending pin. Failed resolution keeps the
saved context. Restoring an unpinned runtime ordinal clears its occurrence choice, and v3 payloads
reject a chosen occurrence without an exact pin. The codec checks flags, coverage, source span
overflow and pin/cursor consistency through the existing typed refusal path. There is no v2 reader
or migration shim. `Session::activity()` validates both visits and saved places before any writer
can append, preserving refused activity files.

Historical cells also need an exact current-frame check before highlighting. The overlay now checks
store authority, version and cell presence; binding changes invalidate cached marks. A cell number
reused in another state cannot receive the saved target's chosen highlight. Document marks already
use an exact view check. Cell descriptions say when a known target is outside this view. This C++
code belongs to the existing typed model/codec and render bridge; no action, setting, loader,
service or camera is added.

## Validation and evidence

Fresh Xuzz tests pass 106/106, UI tests 238/238 and owning ReadingPlace tests 4/4. New model cases
cover both sides reordered, omitted targets, changed member spans, restoring an existing visit,
failed resolution, explicit dismissal and unpinned ordinals. Codec cases cover both document/cell
pins and malformed pin data. UI regressions refuse a different open version, permit exact reopen,
protect previous-format activity bytes, and exclude historical chosen cell ink until its exact frame
is bound. The former Activity Back test expected a missing cell cursor to disappear; it now checks
retention of the exact saved cell target.

The native candidate authors a fresh v3 profile, closes the chosen `b1`, and restarts with only
`--profile`. The known Left target remains `b1`, now place 3/3 because the resolved set also
contains a document and cell 24. The Right authored member and exact `a34` site remain independent.
Enter while `b1` is closed refuses entry and adds no visit. The visible Hypertime map reopens the
exact `b1`; fresh Enter then reaches bytes 0–20 and adds one completed visit. Total visits stay 2
across restart/refusal/reopen and become 3 only on successful entry. Every source dump has hash
`49a12edef0113d9b6f6b7a120c8f2f7cbf7dd1f6d865e8606dc7dfaac4a164d6`. Reading-place metadata can add
private activity operations without adding visits.

`native-restart/` retains baseline and candidate logs, snapshots, lossless frames, original hashes,
exact input vectors, six-variable headless provenance and frozen binary hashes. A supplemental
Inspector capture visibly shows the closed `b1` identity and reference failure. Both core phases
exit 0. That supplemental runner captured its two pages, then tried a disabled third-page action and
exited 1; it is not a full pagewalk pass. Native evidence is OpenGL at 1440×1000.

The native executable was frozen before two header-comment edits and test refinements. Its hash and
the final rebuilt app hash differ; `native-final-elf-equivalence.json` verifies every allocated
section's address, flags, size, alignment and contents agree except the build-ID note. Native
results belong to the frozen executable; final tests use the final rebuilt binaries.

Fresh strict core GL/GLES/Vulkan comparison passes unchanged criteria: GL/GLES ordinary and overlay
match exactly; Vulkan differs by 0.5104% and 0.5096%. Configured one/four-thread frames match, but
the small sample does not split actual recording. Broad E2E is explicitly excluded. The unchanged
shared library and library test binary permit reuse of library 799/799 and owning mip/cache 53/53;
Xuzz/UI are fresh. No optimized-performance acceptance is claimed.

Next exercise J25 branching Back/Return and each retained forward child through actual controls,
including unavailable exact targets after restart. Complete J24/J25, exact cell/history endpoints,
remote/locked/package lifecycle variants, naturally pending-page interruption, native AT-SPI, other
native backends/scales and the complete 16+16 journeys remain open. Publication/pin-lifecycle audit
still precedes D2 history enumeration. The source review records the remaining semantic
range-validation boundary for malformed historical cell pins.
