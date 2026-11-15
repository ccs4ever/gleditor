# Portable mesh result occurrences

The baseline is `39406263`. This combined C8 route package is implemented and execution-validated.
§4.12 and the overall specification remain incomplete.

## Confirmed defect and reviewed representation

`ResultBuilder::append` links each result record directly to its value on `d.mesh.value`. Repeated
references contend for the reciprocal endpoint on the same value cell, so later occurrences remove
earlier value links. The signature package reproduces this with three references to Int64
9007199254740993\. Native export/reload and host preserve all three; SOURCE mesh packaging does not.

Independent Luna purist, systems and codebase reviews converged on using the existing portable cell
relocation convention. Version evaluator-produced result roots as `mesh:result/v2`. A direct
root-local `d.mesh.count` field holds one nonnegative typed Int64 scalar N. Finalization writes the
count after frontend execution. The reader requires exactly N reciprocal rows with no extra tail;
zero requires no items edge. This distinguishes empty results from a missing nonempty rank. Every
ordered record links to a fresh occurrence handle on `d.mesh.value`. That cell holds
`ValueKind::OpHandle` bits naming the canonical referent and owns a fresh `d.mesh.relocation` marker
containing `cell`. Both wrapper and marker are unique per occurrence, so all rank endpoints remain
reciprocal. Apply this uniformly to every semantic `ResultBuilder` value edge, including nested
VPL/logic fields and shared VLace constructor DAGs: the same fan-in defect occurs below the
top-level row. The handle is an identity reference, not a claimed reciprocal adjacency edge. Exact
scalar content/bits and arbitrary target topology remain on the original referent; no primedia copy,
scalar coercion, AST evaluator or process-local callable registry is introduced.

Existing promotion/import recognizes marked cell relocation handles. Repeated wrappers relocate to
the same target identity. Unmarked OpHandle referents remain ordinary result values. Decode exactly
one occurrence wrapper, never recursively unwrap the returned semantic value. Generic publication,
bundle and checkpoint envelopes keep their versions: their dictionary/signature shapes still carry
arbitrary graph roots. Only the result graph schema changes. A result reader refuses the old root
label rather than guessing between direct-value and wrapper rows.

A new shared `mesh_result.hpp/.cpp` provides `readResultCells` returning an expected ordered vector
of canonical CellRefs. Provide ArenaManifold, Manifold plus SpanReader, and EvaluatedValue entry
points. A shared bounded `readOccurrenceTarget` handles one explicitly supplied occurrence wrapper
for nested fields; it does not guess whether an arbitrary semantic value is a wrapper. The two
manifold entry points share one decoding implementation. ResultReadLimits owns explicit defaults of
100,000 items, 1,000,000 metadata relationships and 16 MiB metadata text. These are separate reader
ceilings, not total-heap bounds or replacements for evaluation budgets. Discover fields only from
each owner's `dimensionsOf` span, bounded before text materialization; never allocate or scan the
global dimension inventory. Unrelated dimensions with the same display name elsewhere in the value
graph must not invalidate the schema. Validate the version, unique owner-local dimensions/required
links, wrapper kind/marker, target existence and complete row rank with cycle guards. Bound metadata
before materializing text, including a charge for every span header (even zero length) and byte.
Charge arena work before allocation/publication. Public entry points translate exceptions into
errors without returning a partial vector. Return no partial decoded vector on failure. Evaluation
and transport tests must demonstrate refusal/retry at meaningful budgets.

The root count adds a fixed dimension and scalar cell. The wrapper costs two extra cells and one
marker edge per occurrence, in addition to the existing record and item/value edges. Existing
arena/result-graph cell, work and byte limits must account for all wrappers, markers and reachable
targets. Exhaustion discards the private evaluation context; no shortened answer is published. No
speedup, total-heap or cancellation claim follows. The corner-list alternative is rejected here
because it adds new incidence machinery and can require quadratic full-row resolution; the engine
already has portable constant-time identity references.

## Work and ownership

One Luna engineer owns the shared decoder and evaluator producer. A second migrates existing direct
mesh-value test readers to the shared API, preserving assertions about kinds, values, ordering,
identity and nested VPL/logic data. A third adds dedicated malformed/budget and publication/import
regressions in new test files; root coordinates plan/status, final review and validation. No writer
builds or runs executables. All writers freeze before `make -j$(nproc)`.

Cover Vquery named/rest duplicate source, ordinary repeated values, Int64 versus Double (including
integral Double), exact large integers and full-width handles, empty results, mixed/structured
values and original marked/unmarked OpHandle referents. Prove N distinct rows/wrappers retain
ordered duplicate multiplicity and one canonical target. Exercise Vortex and other supported source
frontends through the common producer, then publish/receive/import into a fresh Store and reload
without retaining the evaluator context. Verify reciprocal topology, shared target identity and no
source Store operations. Reject missing/wrong markers, bad wrapper kinds/targets, duplicate schema
dimensions, row cycles, old roots and metadata/count/work/byte limits. Keep generic arbitrary graph
publication working. Track existing unavailable Prolog routes as baseline failures.

Independent architecture, senior and resource gates review the final implementation and executed
evidence. Run focused source/transport tests, the affected VM/language matrix, full engine alone,
exact clang-format 19 repository formatting and lint. Every executable is headless with all three
XDG directories isolated. Commit only a validated bounded stage; static callable and remaining
lifecycle/route/measurement gates remain open.

## Primary-source design grounding

Nelson describes multiple arrangements with less copying and at most two connections per dimension
in [his ZigZag summary](https://www.xanadu.com.au/mail/zigzag/msg00274.html). The purist review also
consulted
[Nelson's 1998 talk](https://zzstructure.uniud.it/docs/pdf-pub/1998-c-Nelson-Whats_On_My_Mind.pdf)
and the [ZigZag introduction](https://gzigzag.sourceforge.net/gi/gi-ns4.html). The reviewed mapping
separates a result occurrence from its referent and uses the engine's existing portable identity
reference; it does not reinterpret a fan-in as a reciprocal ZigZag rank.

## Publication ownership safeguard

The successful all-fresh transport fixture surfaced a generic promotion warning. A subsequent
read-only audit proved a distinct hazard in direct cross-Store publication: a raw persistent base
CellRef can collide with a different valid cell or operation in the destination. The normal worker
path publishes into its base Store and avoids this ambiguity. The public API must refuse such raw
cross-Store references before any promotion, Store operations, primedia append or signer callback.

Add a bounded no-write preflight to `mesh_value.cpp`, using the existing graph/node budget and arena
work accounting. Fresh-only graphs over another base remain supported; same-Store base references
remain supported; explicit proxy/foreign identity paths retain their established promotion rules.
Skip dimension-filing/provenance edges consistently with promotion, and inspect local OpHandle
targets even when the target is an operation rather than a manifold cell. Tests include colliding
local numbers, absent destination numbers, raw operation handles, fresh results and same-Store
publication. This guards ownership rather than attempting to invent cross-Store identity from an
unqualified integer.

## Execution and independent review

The full `make -j$(nproc)` passes. All 50 focused
result/evaluator/artifact/publication/closure-route tests pass, including the four Store-ownership
cases. The broad VM, VQL, VPL, VLace and mesh run has 1,907 active tests: 1,884 passed, 21 unchanged
baseline failures and two skips, with eight disabled probes. The final complete engine run after the
publication guard has 4,468 active tests: 4,408 passed, the same 32 baseline failures, and 28 skips;
12 probes are disabled. No failing name is new. Thirteen tests were added. These are scoped gates,
not a full-suite pass.

Repeated named/rest results retain three ordered references to one canonical Int64 9007199254740993
cell. A separate case preserves the original distinct integer sequence. Source routes cover the
shared producer across frontends and a nested shared VLace constructor DAG. Publication, fetch,
import, promotion and Store save/load preserve duplicate identity, Int64 versus integral Double, and
the exact canonical bits of 1.2345678901234567 after evaluator destruction. Scalar zero retains the
existing R6 canonicalization policy. Imported spans use verified content sources; no copied primedia
or source Store operations are required. Malformed roots/counts, reciprocal rank extensions,
marker/type/target errors, cycles and item/metadata budgets refuse.

Independent Luna architecture, senior and resource/lifecycle gates accepted the result schema and
the additional publication preflight. The architecture review's count/value field-tail gap was fixed
and given explicit malformed-graph regressions. A proposed detached-owner concern was withdrawn
after re-reading: unknown owners already enter the conservative identity preflight. The normal
same-Store worker path and fresh-only cross-Store publication pass; ambiguous raw base references
refuse before either Store's operations or primedia change.

Logs: `/tmp/vql-mesh-results-build.log`, `/tmp/vql-mesh-results-focus.log`,
`/tmp/vql-mesh-results-broad.log`, `/tmp/vql-mesh-results-full.log`,
`/tmp/vql-mesh-results-format.log`, `/tmp/vql-mesh-results-lint.log`,
`/tmp/vql-mesh-results-lib.log`, `/tmp/vql-mesh-results-zigzag.log`.

This resolves the result-occurrence loss recorded by the signatures package. Remaining static
callable defaults/captures, decorated forms, lifecycle and final route/measurement acceptance keep
§4.12 and the overall specification incomplete. No speedup, total-heap or cancellation claim is
made.
