# VQL language and ergonomics review

This review treats §4.12 and the v16 changelog as the intended baseline, despite the 15.1 banner. It
is a proposal, not an implementation report. I read the full VQL specification and the workflow
review, then checked the current parser and engine. New functions below describe proposed
`std:bridge`, `std:query`, and diagnostic packages; their namespaced spelling is illustrative until
package-qualified identifiers have a grammar. Examples are pseudocode and are not runnable.

## Fix the semantic foundation before adding protocols

**Rank traversal and one-hop structure must have different contracts.** Keep `/+d.name` as the whole
outward rank. A shape field checks one immediate neighbor; it is not equivalent to the path with the
same dimension. Add ordinary `neighbor(cell, dim)`, `one(stream)`, `optional_one(stream)`, and
`first(stream)` functions. `one` rejects zero and multiple results; `optional_one` rejects
multiplicity; `first` deliberately accepts it. Require callers to choose rather than implicitly
rendering the first result. A type contract `Cell { +d.title: String }` should describe the
immediate neighbor's value, not claim that `$p/+d.title` has scalar cardinality. The latter remains
a stream.

For containment, `$parent/+d.contains` reaches its first child; siblings need `d.seq`. Provide
`children(parent)` and `descendants(parent, options)` with documented inclusion, traversal order,
cycle, and duplicate policies. The helpers prevent every adapter from inventing a slightly different
recursive walk. The full-rank `/` remains intact, including `::from` versus `::rank`.

**Quantification needs substitution-safe values.** Existential comparisons can remain convenient,
but flag `!=` over a possible multi-result path with an actionable lint: “some differs; use none
matching for no member equals.” §3 defines existential comparison while §4.3 still calls direct
stream/scalar comparison a type error in intent; resolve that contradiction. Specify stream/stream
comparisons too, including empty operands and pair quantification.

`all($xs[predicate])` currently needs the original unfiltered rank, which makes meaning depend on
expression syntax and lost provenance. What happens after `let`, a UDF, two filters, concatenation,
or an index window? Make `all($xs, predicate)` the canonical overload and allow the filtered form
only as documented sugar retaining an explicit domain. `any` and `none` can have the same overload.
Evaluate the predicate once per occurrence, short-circuit only where effects permit, and preserve
vacuous truth. An incomplete source is a separate issue: universal truth over captured members must
not be presented as universal truth about uncaptured remote records.

**Value equality is not evidence identity.** Keep `=` for value comparison and add named
`same_cell`, `same_subject`, `same_observation`, and `same_occurrence` functions. Cell identity must
include store identity; a session spool slot or bare op index is insufficient across stores.
Observation equality additionally identifies the captured edition. Two repeated shell commands or
identical IRC messages are different occurrences. Clone members sharing displayed content do not
thereby identify the same source entity. Names, message text, and coincidentally equal SQL values
never justify automatic identity merging.

Define exact typed equality, numeric promotion, collation, NaN, and text normalization. The current
engine's `compareValues` uses double conversion and a `1e-9` tolerance; it is unsuitable for invoice
keys or exact amounts. Add decimal/exact-number library values represented structurally, preserving
source representation and scale; approximate comparison should be named and intentional.

**Absence must not silently become an empty string.** The current engine's `evaluateValueExpr`
returns the first path cell and returns `""` when there is none. Before adapters depend on this,
separate optional absence, explicit source null, empty text, redacted/unsupported fields, failed
acquisition, truncated/gapped capture, and pending work. SQL NULL is present data with a null tag; a
missing REST member is absent; an unavailable page is an acquisition failure. They are not one
truthy/falsy bucket. Represent these as tagged structural result/field cells with ordinary payload
and status relations. `T?` should mean optional presence only. Defaulting applies to absent input,
not null, failure, or pending. Scalar operations on unhandled non-values should produce typed
errors; explicit `value_or`, `match_state`, and `require_complete` make policy visible.

## Make the shape and effect boundary legible

Shape predicates, contracts, projections, and updates need one shared descriptor algebra with
separate operations. §4.8 describes `$c{...}` as severing omitted links; §4.10 uses the same form
for virtual views. A field selection that accidentally disconnects data is a serious ergonomics
defect. Choose and version a single rule: recommend `project($c, $shape)` for an arena view and
`weave apply_shape($c, $shape)` for source edits. Existing shorthand may remain only with an
unambiguous effect classification and a diagnostic naming deleted edges. A normal annotation helper
should add an annotation relation, not apply an exclusive replacement shape. A masked view is
neither an authorization boundary nor a safe publication export.

Retain `pure`/`weave` as approachable summaries, but infer a richer effect set internally: snapshot
read, mutable external read, clock/random input, arena construction, local persistence, host
process, network delivery, and subscription. Allocating arena cells without Structure operations
does not establish externally deterministic evaluation. Pure functions should be deterministic
relative to explicit pinned inputs; a separate “no persistent write” property accommodates ordinary
live reads. Higher-order effects propagate through lambdas and function values. Unknown plugin calls
carry conservative effects. Optimizers cannot duplicate, discard, reorder, or memoize external
effects.

Capabilities belong in typed arguments or host context, and inference should show requirements
before execution: connection/account scope, allowed operation, filesystem/process scope, and any
spending limit. A cell containing a capability identifier is not authority by itself. Submission
crosses an explicit host boundary; constructing or navigating action cells cannot secretly send
mail, run a command, or modify SQL. Lens edits aimed at external services stage action proposals
rather than commit on query completion. This needs reconciliation with §4.11.3's automatic
unprojection model, not a protocol-specific exception hidden in an adapter.

## Compose work from structural library values

Build `std:bridge` around connection, subject, observation, field, occurrence, action, attempt,
receipt, and checkpoint cells. Each is annotatable and addressable independently. Use relation
occurrences/proxies and `d.contains`/`d.seq` to preserve the one-neighbor-per-direction invariant.
Do not place N external relationships in one direct slot or confuse a mutable clone master with
immutable evidence. Captured bytes enter the user's permascroll once; derived quotations retain span
lineage, while external identities and source-authorship claims remain explicit metadata.

Keep adapter verbs behind the same lifecycle: describe connection/schema; create request/action
proposal; host-acquire or host-submit; inspect immutable observation/attempt/receipt; reconcile;
checkpoint. SQL templates plus parameter cells, HTTP method/path/header/body cells, process argv and
cwd cells, mail envelope/body cells, IRC target/message cells, and social object/audience cells are
different structural schemas, not new language keywords. History import returns observations of
entries and never execution requests. An ActivityPub adapter advertises its supported server API and
delivery semantics; federation does not imply one universal REST endpoint.

Add reusable `join`, `group`, `aggregate`, `distinct`, `order`, and `window` library functions. They
consume structural streams and key functions and return arena result cells with relation occurrences
back to inputs. Joins declare inner/left/semi/anti policy and null-key behavior. Groups retain their
members and aggregate witnesses. `distinct` declares identity or value equivalence; ordering
declares a stable tie rule. Pure sorting produces a new rank rather than invoking mutative
`sort_into`. Windows declare occurrence time versus observation time, boundaries, watermark,
late-event behavior, and gaps. Unbounded groups/sorts require a bounded window or explicit resource
policy. Explain when an operation materializes state; “zero allocation” cannot describe arbitrary
joins, sorting, capture, or recovery.

Derived lineage should record query/function edition, pinned input observations, parameters,
normalization policy, and result occurrences. This lets an aggregate be annotated and later expanded
to the contributing fields. Recomputing makes a new result edition rather than moving an old
annotation to a different answer. Predicate pushdown requires semantic equivalence, including NULL,
collation, timezone, completeness, and authority; otherwise evaluate locally.

## Proposed pseudocode

```vql
// Proposed libraries, explicit acquisition; not runnable.
let $db := bridge_acquire($billing, $parameterized_query, $capture_policy, $read_cap)
let $api := bridge_acquire($support, $paged_request, $capture_policy, $read_cap)
let $pairs := query_join(require_complete($db), require_complete($api),
                        /+d.invoice_key, /+d.invoice_key, "left")
return derive($pairs, $query_edition, list($db, $api))
```

The observation envelope retains failed pages even when `require_complete` refuses to yield a
complete row stream. It must not turn acquisition failure into a plausible smaller join.

```vql
// Proposed query API: no hidden source-domain dependency.
let $entries := bridge_history($history_observation)
let $reviewed := query_filter($entries, \($e) none(fields($e, "status"),
                                                \($s) value_equal($s, "success")))
return derive($reviewed, $query_edition, $history_observation)
```

This does not assert that history entries lacking captured status failed. The predicate finds “no
captured success assertion”; a strict failure query must explicitly require a known exit status.

```vql
// Proposed draft/submit separation; host calls require authority.
let $draft := bridge_action("mail.reply", $thread_subject, $selected_evidence, $audience)
let $sealed := bridge_seal($draft, $exact_draft_revision)
let $attempt := host_submit($sealed, $send_capability)
return $attempt/+d.receipt, $attempt/+d.reconciliation
```

Reopening these results performs no submission. An ambiguous receipt stays unknown until a
reconciliation action resolves it; re-evaluating the query must not imply an automatic safe retry.

## Minimum language change versus full extension

The minimum is a semantic repair plus library APIs: cardinality checks, predicate-taking
quantifiers, explicit identity functions, tagged result cells, effect inference, and explicit host
submission. No SQL, HTTP, shell, mail, IRC, or ActivityPub keyword is justified.

Grammar repairs are necessary independently: `LambdaParam` accepts a type but not the v16
`Cell { ... }` refinement with bound fields; `ShapeTypeField` accepts a type but not `$alias: Type`.
Factor a shared structural contract production into parameter and destructuring forms. Specify empty
shapes, named arguments (`by:` appears outside `ArgumentList`), conditional/arithmetic expressions,
signed dimensions as function values, and qualified package identifiers consistently. Distinguish
partial-path function values from slash-prefixed `FieldWeave` returns by syntactic context. Do not
claim LL(1) until tokenization and production tests demonstrate it.

The full option adds reusable type aliases, callable signatures carrying cardinality and effects,
and structural tagged-union narrowing. These earn their grammar by making user libraries checkable,
not by encoding adapters. Gradual checking inserts boundary checks when `Any` enters typed code;
unknown data cannot justify erasing checks. Parameter mismatch must distinguish deliberate pattern
filtering from a contract failure that otherwise loses rows silently.

Provide `describe`, `schema`, `explain`, `trace`, and `why_empty` as queryable structural results.
Show inferred cardinality/effects/capabilities, source versions/completeness,
pushdown/materialization, and predicate witnesses. Static explain performs no acquisition or action;
bounded trace is explicit. A typo dimension, a filtered row, denied authority, and an incomplete
source need distinct diagnoses.

Acceptance: every documented example parses; tests distinguish rank and neighbor, substitution of
quantifiers, duplicate occurrences, exact equality, missing/null/error/pending/gap, and contract
failure versus filtering. Interface journeys must annotate a derived field, reopen offline, refresh
without retargeting old evidence, inspect an ambiguous attempt, and resume without repeating a
completed effect. Reject speed and allocation claims until measured against these semantics.

## Supplement: VQL16 implementation update at 7617d3f0

This supplement reviews only `732b8280..7617d3f0`, including the parser, evaluator, compiler,
specification changes, and new contract/functor tests. It supersedes specific implementation-gap
claims above; it does not re-evaluate the whole baseline. Tests were inspected, not executed.

The stale banner is corrected to 16.0. The parser now accepts typed parameters, return annotations,
`Cell { ... }` refinements, bound typed fields, defaults, unions, optionals, and generics. The EBNF
now reuses `DestructurePattern` for shape contracts. My earlier claim that parameter aliases and
return annotations are absent is resolved. Direct evaluation implements these contracts, including
singleton versus `Stream<T>` cardinality, and preserves references in `let $y := $x`. New tests
exercise all four shape modes, optional fields, defaults, reference preservation, and return errors.
First-class path functors now work directly, compose as captured step lists, and accept untyped
function parameters. Compiled functor bindings are explicitly refused. These are substantive
improvements; they do not establish compiler/evaluator semantic equivalence.

The parameter-filter decision remains risky for workflows. A typed call with a malformed amount,
missing owner, or unexpected multiplicity returns no match; the same empty result can mean a
legitimate absence. Filtering is useful in exploratory paths, but adding a type annotation to an
existing business function can now silently remove evidence. Keep the chosen convenience while
providing an explicit strict invocation/validation function returning accepted and rejected
occurrences, field paths, expected contracts, and source observations. Require successful strict
validation before deriving a complete aggregate or submitting an intent. `why_empty` must expose
contract rejection. Return mismatches correctly raise `TypeContractError`, but that cannot recover
inputs discarded before the function body ran. Acceptance should include a malformed invoice
remaining visible as a rejected occurrence rather than disappearing from a total.

“Textual Int” is a coherent parsing predicate, not an exact storage-type guarantee. The engine
checks whether text parses without replacing the original cell, so `"95"` can satisfy both `String`
and `Int`. Document this as readable-as semantics, including signed forms, whitespace, overflow,
nonfinite floats, locale, and whole-input consumption. Libraries need a distinct explicit conversion
that returns an exact typed numeric value plus its source lineage, and an exact-kind predicate when
preservation of source typing matters. An identifier `"00104"` must not silently become equivalent
to numeric 104 simply because both pass an Int contract. SQL decimal scale and NULL still need
structural types. Contract acceptance also does not fix the existing approximate double-based
comparison operation; parsing an integer successfully cannot justify comparing it through a lossy
double conversion.

The compiler's documented unchecked type erasure is a remaining semantic gap. Erasure after static
proof or inserted boundary checks is sound; dropping an unchecked contract changes filtering and
return-error behavior. Until parity exists, expose the engine's supported-contract mode and refuse
unsupported typed execution or apply the same boundary checks. Do not teach the direct evaluator's
contract as a portable safety guarantee. Callable cardinality/effects remain absent from functor
signatures, and untyped-only functor parameters constrain gradually checked reusable libraries.

The common workflow synthesis remains compatible with these changes: connection, subject,
observation, field and relationship occurrence, anchor, derivation, intent, attempt, receipt, and
workflow records can all be library-defined structural cells. Specify an anchor as a durable target
at an observation/version/span, distinct from the evolving subject proxy. Define derivation as the
query edition plus contributing occurrences, not just matching scalar content. Contract rejections,
unknown outcomes, and completeness belong in this graph too. Such records do not confer authority:
explicit host acquisition/submission and capability checks remain necessary. Reference-preserving
`let` helps lineage, but preserving a live cell reference alone does not freeze evidence or make an
arena reference durable.
