# Round 4: language and ergonomics critique

**Quiescent at the reviewed scope: no new material actionable findings.** I accept the combined
prerequisite repair set and full workflow extension proposal with their explicit implementation
boundaries. This is not certification of all VQL behavior or of unbuilt service integrations.

Read the shared R1–R3 dispositions and peer reports, then rechecked the macro termination guard,
caller-scope cell/functor argument capture, defaults, lexical/perspective restoration, scalar
unknown-call refusal, shape preflight and engine conformance boundaries. The macro guard closes L6
without changing optional/zero-repeat identity behavior. My final build of `xuzz_test` and `vquery`
succeeded; all 146 VQL tests passed headlessly, including the combined peer fixes. Clang-format19
accepted both owned files. No code was changed this round.

Validation claims remain appropriately limited: excess UDF arguments and non-final variadics refuse;
missing/default/type-match behavior is preserved. Defaults see earlier callee bindings; explicit
arguments retain caller identities. Bare calls and explicit `./f()` still share the current Context
AST. Scope guards restore lexical/perspective state rather than rolling back earlier arena edits.
Existing scalar absence/coercion policy remains distinct from unknown-function refusal and future
exact-value contracts. Unsupported compiler/service paths refuse within their documented subset.

The full extension model adequately covers SQL, REST, shell/history, email, IRC and fediverse work
through ordinary structural libraries: durable subjects/fields, immutable observations, relationship
occurrences, pinned anchors, derived/excluded-input lineage, strict validation, bounded asynchronous
ownership, explicit host dispatch, exact revisions and authority, admission ownership, unknown
outcomes, late receipts, coherent scan coverage, offline replay and selective publication. Effects,
exact values, durable adapters and interface journeys remain future work with stated acceptance
criteria. No additional syntax or workflow mechanism is necessary to prolong this review.
