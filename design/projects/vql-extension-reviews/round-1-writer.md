# Round 1: technical writer critique

Scoped to the current working tree, including VQL 16.1, conformance, workflow §§1.2–1.3, and
targeted evaluator/regression-test evidence. I inspected source; I did not run tests. The repair is
substantial: unsupported services now refuse, compiler scope is explicit, the corpus replaces
misleading production examples, shape views are separated from edits, and the proposal preserves
evidence/attempt/receipt distinctions.

## Material findings

1. **High — remaining effect claims overstate implementation.** Spec §4.8.8 says `pure` is
   statically read-only and the compiler infers effects. Conformance says definitions are refused by
   the compiler; direct `callUserDef` checks contracts and executes the body without an inspected
   effect-validation pass. Its examples also use unsupported infix arithmetic and bare shape
   mutation. **Fix:** label effect declarations as parsed intent with enforcement unbuilt; replace
   examples with accepted positional forms, or clearly mark the entire block proposed. Add an
   explicit regression that a mutating `def pure` refuses before mutation when enforcement is
   delivered. This is current documentation overreach, not a claim that the future effect model
   exists.

1. **High — laziness is still promised in the semantic chapter.** §4.1 says paths “compile to” its
   generator loop and `[1]` short-circuits. Current `evaluateStep` traverses into `stepOutput` first
   (`vql_engine.cpp:1253` vicinity), then filters and applies ranges at `:1867`; conformance
   explicitly refuses compiled ordinary traversal. **Fix:** describe the loop as intended streaming
   lowering; state current direct traversal/materialization and post-filter slicing. If early
   termination is required, implement and measure it separately. This matters for large
   database/REST workflows and effectful predicates.

1. **High — scratch GC contradictions survived the repair.** §5 correctly says mark-and-sweep/pinned
   cache islands are unbuilt. §7.1 still describes scratch as “collected when unreachable” and calls
   §5 its collector; §7.5 promises one-break wholesale reclamation and refers to deleted §6.1/§6.2
   examples. **Fix:** mark §7.5 a proposed lifetime policy, replace current-tense reclamation
   promises, and remove broken example references. No cache memory bound follows from severing links
   today.

1. **Medium — workflow status still mixes history with current facts.** Workflow §1.1 calls the
   opening unbuilt claim stale even though the latest spec opening now correctly identifies built
   engines. Its heading says “currently” above the explicitly historical 7617 table; §1.2 still
   lists empty-shape grammar as a remaining gap although conformance tests cover it. **Fix:** title
   the table “Baseline before repairs,” remove obsolete stale-introduction commentary, and make §1.2
   a historical issue register whose delivered/current scope points directly to §1.3.

1. **Medium — snapshot annotation correspondence is unspecified.** Spec §4.8.3 correctly remaps
   identity and preserves span references; `applyShape` maintains a local `copies` map
   (`vql_engine.cpp:2218` onward) but returns only roots. Workflow requires every computed result to
   support derivation and durable anchors. **Fix:** define a proposed queryable source-to-snapshot
   correspondence/derivation record, including source observation/version and deliberate clone
   authority. Shared bytes alone cannot recover structural identity after the local map disappears.
   This is a future workflow contract, not a failure of today's arena-only snapshot scope.

Also fix the malformed clone example in §4.6 (\`$a>\<%VALUE\`\`); it obscures the newly clarified
authority rule.
