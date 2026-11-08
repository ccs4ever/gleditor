# Vortex closure runtime plan

## Bounded stage and invariants

Iteration 50 establishes a VM foundation for first-class closure values. VQL lambda-value lowering
follows separately; its current explicit refusals stay until real graph creation and application
exist. Existing closed-functor frame variants remain valid, and named definitions keep their current
call-site capture behavior.

A closure owns the membership of its captured streams: resolved `CellRef` sequences in order,
including duplicates and empty streams. It retains the referenced cells, rather than copying their
contents. A data cell created in an activation may escape because its arena identity and contents
survive return. An activation's reference or stream binding is not environment storage: creation
must resolve it before `ReturnFrame` erases the runtime maps.

An open argument awaits application; a bound empty capture is already supplied. They have distinct
records and cannot share an absence sentinel. All dimensions and cell references keep their native
widths. No runtime AST, evaluator, callback, generated opcode chain or process-local closure
registry implements application.

## Graph protocol and VM boundary

Static closure descriptors bind the argument ports of an existing `VORTEX_FRAME/1` descriptor to
captured or applied stream indices. Each frame argument is mapped exactly once, every declared
external source is used, and a source may fan out to several frame arguments. Stage 50 supports
Stream argument ports; result ports retain the existing frame rules.

`CreateClosure` resolves capture inputs and constructs a versioned environment graph. `ApplyClosure`
validates that value, assembles the frame arguments and invokes the existing `invokeFramePrepared`
path. Native return, cursor stacks, choices, guard scopes and result publication remain the
activation mechanism. Both new opcode identifiers append to the existing enum. Unsupported parameter
pipelines, conditions and stdlib combinations refuse before effects.

Descriptor and value roots use a shared runtime-object probe and a structural claim relation. Each
root owns a unique positive claim endpoint containing its kind/version. Sharing that endpoint would
rewire the reciprocal ZigZag link, so factories must create one per root. The claim endpoint's
negative backlink does not make it an object root. The version tags are
`VORTEX_CLOSURE_DESCRIPTOR/1` and `VORTEX_CLOSURE_VALUE/1`.

The generic marker lives below Vortex. The core resolves the claim dimension once and supplies its
`DimRef` to the shared scalar reader; runtime reads must not use unbounded name lookup. Only an
untyped cell with the exact short probe text needs the bounded structural probe. Matching text
without the claim relation stays ordinary text. A claimed object is not a scalar String, numeric
value or Boolean. Typed scalar bits retain their current precedence; malformed typed closure roots
refuse at the closure parser.

Descriptor and environment parsers expose every reference-bearing record for relocation. Source
indices and counts are ordinary integers, not cell references. Export must preflight closure and
frame metadata before appending Store operations. Generated Create/Apply graphs must execute after
reload without the original VM or AST. Materialized local handles may be persisted only when a
fresh-VM application test proves their references relocate; otherwise the export boundary must
refuse them explicitly. Existing live-frame, guard and foreign/base export refusals remain.

The implemented export boundary accepts a materialized closure root supplied directly as graph
input. A VM register referring to that root requires a persisted binding initialization protocol;
export of such an Apply input slot currently refuses before Store writes. The exporter walks closure
frame bodies through declared instruction edges so disconnected parameter wings relocate too. Legacy
numeric `Choice` targets refuse alongside numeric `Jump`, `Branch` and `Call`; ordinary integer data
is never inferred to be code or a closure handle.

## Resource and lifecycle gates

Closure limits bound descriptor records, stream counts, captured references, environment records,
cumulative successful creations/environment cells and assembled invocation references. Apply's
fan-out must be counted before copying argument vectors. Bounds use checked arithmetic, and
traversed/copied metadata and references consume arena work. These are logical limits, not a total
CPU or heap guarantee.

Creation and publication use savepoints and staged VM updates. Failure restores arena and reference
state without refunding fuel. Successful creation/allocation accounting stays cumulative across
return and backtracking, as native frame backing already does. Environments survive their creator's
return or cursor close while rooted. Choice rollback restores reference registers with the arena;
there is no separate closure object table to become stale. Arena compaction currently compacts
content/link storage while preserving cell references.

Closure opcode wings use explicit cell-reference records from the core's binding API. Their port
counts, cycles and relation rows are checked before vectorization; collection consumes fuel.
Additional bounded vector copies and slot mapping are not separately charged, so fuel remains a
logical accounting measure rather than an exact CPU or memory cost.

Acceptance covers descriptor malformations/cycles/versions, port kinds and coverage, cardinality,
quotas and retry, capture rebinding and live data mutation, ordered duplicates/empties, frame
escape, choices/cancellation, later-cursor application, compaction, scalar typing and portable
graph/value export. Profiling separates binding/capture materialization, compilation and
application/allocation; measurements establish costs rather than a promised speedup.

## Frontend follow-up

The first lambda-value slice has one untyped named formal and a supported read-only path/alias body.
Free scalar/stream bindings are captured at creation; rebinding a name later does not replace that
membership, while changes to referenced cells remain visible. This ruling is new for closure values
and does not change named-function call-site capture.

Implicit outer-context roots, dot formals, defaults, destructuring and nested lambda creation are
outside the first slice. Candidate `.` inside a path predicate remains the traversal candidate; it
is distinct from capturing an ambient cursor. Existing inline path-step lambda behavior remains.
Further forms, foreign closure handles, general closure call syntax and route/lifecycle parity still
need explicit gates. Neither stage completes VQL §4.12.

## Primary-source grounding

The three-persona review used Nelson's accounts as design constraints, not a prescribed closure ABI.
His [ZigZag summary](https://www.xanadu.com.au/mail/zigzag/msg00274.html) supports local path
navigation and identity-preserving reuse. His
[rank discussion](https://www.xanadu.com.au/mail/zzdev/msg01772.html) and
[ZX Internals](https://www.xanadu.net/zigzag/fw99/zxInternals.html) distinguish ordered ranks from
dimensions. VQL's duplicate and lexical-binding rules remain language choices. The
[Xanalogical Structure account](https://xanadu.com.au/ted/XUsurvey/xuDation.html) motivates
retaining reference identity without turning scalar equality into shared primedia addresses; it does
not specify closure capture time, environment records or a calling convention.
