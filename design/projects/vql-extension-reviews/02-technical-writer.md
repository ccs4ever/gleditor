# VQL workflow extension: technical writing review

Read-only review of `732b8280`. I read AGENTS.md, the VQL specification through its reconciliation
and version history, and the three supplied reviews. I directly inspected the implementation
excerpts cited below; I did not build, execute examples, benchmark, or inspect every claimed
subsystem. The proposed workflow model is a design recommendation. Evidence from another review is
identified rather than represented as my own inspection.

## Editorial conclusion

The strongest extension is a durable work record: people can return to a question, inspect the exact
evidence, understand a proposed action, and see what actually happened. “More lenses” does not
describe that outcome. Keep the extension separate from the canonical language specification, and
make its first paragraph say what a person gains.

The canonical spec currently blends syntax, desired semantics, implementation sketches, political
claims, and release assertions. Its principal obstacle is credibility: the 15.1 banner contradicts
v16 §4.12; the opening says nothing is built while a compiler and direct engine exist; later prose
asserts runtime behavior unsupported by the inspected implementations. An extension should
acknowledge these discrepancies explicitly rather than inherit their promises.

## A small evidence table to include

| Area              | Built evidence inspected                                                                                                                             | Specified design                                                                   | New workflow proposal                                                 |
| ----------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- | --------------------------------------------------------------------- |
| VQL execution     | Direct engine executes statements; compiler has a different supported subset (`vql_engine.cpp:139`, `compiler.cpp:255,360`)                          | Lazy rank streams, effects, gradual contracts, point-free functions                | Shared effect-aware plans and explicit unsupported outcomes           |
| Projection        | `ProjectionSession` provides roots and key lookup; `NodeContent` contains owned text/scalars (`projection_session.hpp:38`, `projection_lens.hpp:70`) | Span-run projection, transactional unprojection, universal lens dispatch (§4.11.3) | Retain the evidence and identity closure needed by an annotation      |
| Observation       | `executeWatch` visits current matches; its catch-up branch is explicitly a simulation (`vql_engine.cpp:396`)                                         | Durable suspended watch, replay and delta semantics (§4.9.6)                       | Source subscriptions with coverage, gaps, checkpoints, retention      |
| Source resolution | Named-store lookup and ambient-home fallback (`vql_engine.cpp:477,653`)                                                                              | DHT/catalog generators and foreign hops (§4.9.1)                                   | Connection/account/capability-aware adapters                          |
| Work and effects  | Systems reviewer identifies actor continuations/checkpoints and cancellation seams; not independently audited here                                   | Action cells and sealed write-sets (§4.9.5)                                        | Intent, attempt, receipt, unknown outcome, resumable dependency graph |

Use “built” only for the inspected scope. A parser test demonstrates accepted syntax; a codec test
demonstrates message round-tripping. Neither demonstrates a working service, durable workflow, UI
journey, or semantic equivalence between engines.

## Twelve priority specification fixes

1. **Publish the status and conformance boundary.** Fix `vql-query-language.md:3–12`, §4.12 and the
   appendix together. State the intended version, implemented subset, and which engine supports it.
   `compiler.cpp:255` compiles `query.expr`; `vql_engine.cpp:139` handles the script representation.
   The compiler explicitly refuses anchor predicates at `compiler.cpp:360`. Replace “canonical
   production examples” with “design examples” until they are executed and verified.

1. **Define primitives once, after convergence.** §1 still gives allocation/isolation targets
   `-1`/`0`, calls the core single-primitive while displaying two primitives, and says zero is both
   absence and traversable. §7.2 retires allocation sentinels and excludes cell zero. Move the
   current rules into §1 and grammar; archive reconciliation history. Keep absence in APIs distinct
   from wire/on-disk representations, as AGENTS.md requires.

1. **Reconcile quantification and identity.** §3 accepts existential stream/scalar comparison; §4.3
   at line 688 calls it a type error. §7.6 defines rendered equality but leaves “coordinates match”
   without a query operation for testing identity. State existential comparison consistently, define
   whether a `let` binds cells or rendered values, and specify an explicit identity predicate.
   Annotation/reconciliation workflows cannot infer identity from equal displayed text.

1. **Repair grammar/example mismatches systematically.** §3 lacks scalar arithmetic and general
   conditional value expressions used in §4.8.8; `ShapeLiteral` requires a field although `{}` is
   valid; arguments omit signed-dimension values and named arguments used by `link`/`sort_into`; v16
   `Cell {…}` refinements do not fit `PrimaryType`. §4.11 examples use record returns, direct-step
   `*`, anchor filters, and `value(.) :=` beyond that grammar. Also reconcile `unmount`, `@now`,
   `$fold/@ver`, and perspective placement. Publish a feature matrix and turn each documented
   example into an executable conformance fixture when implementing it.

1. **Correct clone topology before teaching composition.** §4.6 lowers `A><B><C` to
   `link(C,+d.clone,link(A,+d.clone,B))`; that does not describe a simple A–B–C rank under
   reciprocal single-neighbor links. `%VALUE><$a` also calls `$a` the master after saying the fresh
   leftmost cell is authoritative. Specify before/after adjacency and returned operand, then
   preserve both old-rank collision cases and leftmost authority as explicit decisions.
   `Manifold::cloneMaster()` delegates to a negward rank-end walk (`manifold.cpp:1131`); master
   lookup is not inherently constant-time.

1. **Separate views from edits.** §4.8.3 severs omitted dimensions; §4.10 uses the same braces for
   non-mutating projections. §6.6 calls destructive link stripping an export filter. Define separate
   operations or an unambiguous effect context. A visibility mask is not an authorization boundary
   or a clean publication artifact. The first extension should name this unresolved choice, not pick
   syntax silently.

1. **Separate references, occurrences, and shared editable content.** §4.11.3 maps equal-content
   files to `d.clone`; §4.7 silently inserts content-sharing clones even for one explicit target
   link. Equal bytes, a source reference, repeated membership, and one shared content authority are
   different relationships. Use ordinary occurrence/incidence cells to retain annotatable
   many-to-many relations while preserving one neighbor per direction. Resolve whether clone-based
   fan-out remains universal or becomes explicit. The proposed membership traversal must include the
   first child rather than accidentally skip it through `contains/seq`.

1. **Replace absolute performance promises with scoped hypotheses.** §1/§4.4 promise no allocation,
   but `vql_engine.hpp:31,161` exposes vector results and hash-map environments. §4.10.4 promises
   zero degradation and ≤3 ns arbitrary evaluators; §4.11.5 invents a 99.9% production distribution;
   §4.12.4 claims ABI passage and cycles without evidence. Move layouts to implementation notes,
   state bounds and fallbacks, and require measurements. Distinguish constant-time handle lookup
   from network I/O, rank walks, folds, sorting, and arbitrary computation.

1. **Correct immutable evidence and replay claims.** §4.9.6 says authors modify quoted permascroll
   spans and promises zero-loss reconnection. Immutable bytes do not change: later document
   assemblies or live selectors do. Frozen quotations remain pinned. The inspected watch
   implementation has no historical replay (`vql_engine.cpp:401`). Promise replay only over retained
   accessible history; external gaps and capture limits must be represented explicitly.

1. **Bound discovery and authority claims.** §4.9.1/§4.9.4 promise every backlink and no link rot.
   Say “known, reachable, authorized results,” with coverage and unavailable sources. Name URLs as
   useful external locators even when native publication IDs supply stronger naming. The
   ambient-home fallback at `vql_engine.cpp:490,665` must not be presented as successful remote
   discovery. A query must distinguish unsupported source, failed resolution, and a complete empty
   result.

1. **Distinguish source authorship, capture, and derivation.** §4.11.3's displayed span-aware
   content differs from inspected `projection_lens.hpp:70`; its `commit()` is absent from the
   inspected session header. A source without permascroll addresses requires initial retained
   capture before durable exact-span quotation can be promised. Record who captured bytes, the
   claimed source author, verification evidence, raw representation, and decoded/derived mapping
   separately. A SQL total or normalized mail body has derivation provenance, not automatically
   exact source-span identity.

1. **Specify effect and persistence boundaries before remote examples.** §4.9.5/§6.7 appear to
   append into a remote control manifold while promising no remote mutation. Say where intent is
   authored, how authority admits it, who commits remotely, and what acceptance/completion receipts
   prove. §7.1's “user-generated” is insufficient for authorized background workflows. Identify
   destination stores for retained evidence/actions and distinguish scratch collection from
   immutable history. Retrying an action, replaying recorded evidence, refreshing a source, and
   undoing local notes must have separate meanings.

## Recommended extension structure and vocabulary

Order the new document around reader needs: purpose and evidence status; shared model; four
end-to-end journeys; source-specific contracts; execution/authority/persistence boundaries; open
decisions; staged interface acceptance. Put byte layouts and proposed opcodes in an appendix or
runtime plan.

Use **connection** for source/account configuration, **subject** for a local continuity claim,
**observation** for an immutable witnessed state, **occurrence** for one membership/event,
**anchor** for an annotation target, **derivation** for a result's inputs/program/mapping,
**intent** for requested effects, **attempt** for one dispatch, and **receipt** for what was
acknowledged or observed. A **workflow definition** is versioned; its **run** records state and
checkpoints. Reserve “clone” for shared content authority, “capture” for retained external evidence,
“transclusion” for reuse of identified spans, and “projection” for a view. Avoid “quantum,”
“sacred,” “shadow copy,” “crum,” and unsupported universal guarantees in normative text.

Lead examples with outcomes: “I annotate yesterday's ticket amount; refresh shows today's amount
beside it.” “I reopen a shell-history entry; no command runs.” “The service accepted the mail;
recipient delivery remains unknown.” “Page three failed; the total is incomplete.” Mark new library
names as proposals, and keep examples small enough to reveal one identity or effect distinction.

Preserve disagreement visibly: should fan-out clone implicitly; should braces edit or view; what
annotation closure is retained; when does a view follow continuity rather than pin an observation;
which workflow steps may run unattended; how much source history is retained? The reviews support
making these decisions explicit, not pretending external identity, universal delivery, or
cross-service atomicity can be guaranteed by topology.

## Supplement: baseline 7617d3f0

This supplement reviews only `732b8280..7617d3f0`'s specification changes and targeted
implementation/tests. The report above remains a review of the older baseline; use this supplement
when citing current behavior. I inspected the new tests but did not execute them.

**Retracted findings:** the banner now correctly says 16.0. `PrimaryType` now admits `Cell { … }`
refinements, and `ShapeType` reuses `DestructurePattern`, addressing the v16 refinement/binding
grammar objection in priority fix 4. New §4.12.5 explicitly distinguishes implemented direct
evaluation from unbuilt specialized lowering. Do not describe v16 types and functors as merely
aspirational at this baseline.

Targeted evidence supports this distinction: `vql_engine.cpp:584–632` checks parameter contracts,
returns an empty match on failure, and throws `TypeContractError` on a violated return promise;
`:393–464` constructs/binds step-list functors. `tests/xuzz/vql_types_functors_test.cpp:173,330`
covers return-contract failure and direct functor composition/application, including capture of the
steps present when composed. `compiler.cpp:268–276` explicitly refuses a leading-path functor
binding; the new §4.12.5 states that compiled types remain unchecked. These are different execution
contracts, not interchangeable backends. The zero-closure claim does not remove vector allocation;
§4.12.4's register/ABI/cycle guarantees remain unbuilt and unmeasured.

The new section is a useful semantic ruling, but several phrases need qualification. “Types are
erased … as §4.12.1 allows” obscures that §4.12.1 also promises compile-time and mutation-boundary
enforcement: erasure after checks and omission of checks are distinct. Prefer “the compiler
currently ignores contracts; typed safety applies to direct evaluation.” Call parameter mismatch an
**empty match**, not source absence or a successful action. A workflow must retain rejected/invalid
inputs visibly when completeness matters. “Scalars are read as comparisons read them” should link to
a precise conversion rule, especially because `"95"` is both String and Int. Overlapping contracts
are not disjoint source datatypes; preserve SQL NULL, decimal and timestamp representations
independently.

The synthesis vocabulary remains sound: connection, subject, immutable observation, field and
relationship occurrence, anchor, derivation, intent, attempt, receipt, workflow definition and run.
Build proposed adapters as library functions above an explicit asynchronous capability boundary. No
new syntax is needed merely to add mail or SQL.

The largest decisions remain visibility versus destructive shape edits; clone authority versus
reference/membership identity; retained annotation closure and continuity policy; source-specific
effect admission; and retry/replay/refresh distinctions. The new direct evaluator resolves language
details, not those workflow contracts. Keep its built evidence separate from proposed durable
subscriptions, external actions and cross-source provenance.

## Final synthesis critique

Reviewed `design/vql-workflow-extensions.md` against the 7617 supplement. It correctly labels the
APIs as proposed pseudocode and separates direct v16 contracts/functors from unchecked compiled
types, refused functor bindings and unbuilt lowering. No further baseline retraction is needed.

Material edits recommended:

- Qualify stage-eight “Annotation survives … source deletion” with the retention policy. A retained
  annotation/identity survives; a reference-only anchor can retain an unavailable-content marker.
  The current unconditional acceptance sentence otherwise contradicts §2.3's useful reference-only
  option.
- In §2.4, allow the save operation to **apply a declared retention policy**, showing a review only
  when the closure, cost or authority requires it. Mandatory “show what evidence will be retained”
  on every note could become an accidental approval flow. The obligation is inspectability and
  honest limits, not repetitive confirmation.
- Add export determinism to decision seven: the manifest should pin selected observation/program
  editions and the permitted closure. Otherwise a clean export assembled from live subject-following
  references could acquire new private content between review and publication.
- Revise §4.3's checkpoint sentence from “R8's refusal to resume arbitrary Vortex cursor state” to
  the narrower rationale: “This persists meaningful workflow state without promoting speculative
  traversal cursors into visited stores.” R8 supplies the persistence boundary; the draft should
  avoid attributing a general runtime-resumption prohibition to it without a precise supporting
  ruling.
- State a minimum observation envelope before API implementation: task
  pending/failed/partial/completed, evidence captured versus reference-only, and source coverage.
  `await_observation` must return inspectable failure/partial records rather than only successful
  observations; the journeys depend on this invariant.

Topology, source-authorship/capture distinctions, first-member traversal, effect uncertainty and
explicit unresolved decisions are otherwise appropriately qualified.
